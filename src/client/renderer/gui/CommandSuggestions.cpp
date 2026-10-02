// File: src/client/renderer/gui/CommandSuggestions.cpp
#include "CommandSuggestions.hpp"
#include "ChatScreen.hpp"   // GetServerCommandNames

#include "../../network/NetworkClient.hpp"
#include "../../entity/RemotePlayerManager.hpp"
#include "client/sound/SoundManager.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/inventory/SlotRanges.hpp"
#include "common/entity/EntityNbtHints.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/ItemComponentHints.hpp"
#include "common/world/loot/ContextNumberProviderNames.hpp"
#include "common/data/DataComponentType.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Attributes.hpp"
#include "common/entity/SelectableEntityTypes.hpp"
#include "common/entity/Morph.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/alchemy/Potions.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundSource.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/advancements/AdvancementLoader.hpp"
#include "common/text/TextComponent.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Render::CommandSuggestions {

    namespace {

        using Game::Cmd::Arg;
        using Game::Cmd::Node;

        // ── Text helpers ───────────────────────────────────────────────────

        std::string Lower(std::string v) {
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return v;
        }

        bool StartsWithIgnoreCase(const std::string& haystack, const std::string& needle) {
            if (needle.size() > haystack.size()) return false;
            for (size_t i = 0; i < needle.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(haystack[i])) !=
                    std::tolower(static_cast<unsigned char>(needle[i]))) {
                    return false;
                }
            }
            return true;
        }

        bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
            return a.size() == b.size() && StartsWithIgnoreCase(a, b);
        }

        // SharedSuggestionProvider.matchesSubStr: `pattern` starts `input` at
        // its beginning or right after a '.', '_' or '/'.
        bool MatchesSubStr(const std::string& pattern, const std::string& input) {
            for (size_t index = 0;;) {
                if (input.compare(index, pattern.size(), pattern) == 0) return true;
                const size_t next = input.find_first_of("._/", index);
                if (next == std::string::npos) return false;
                index = next + 1;
            }
        }

        constexpr const char kMc[] = "minecraft:";
        constexpr size_t     kMcLen = sizeof(kMc) - 1;

        // "minecraft:dirt" -> "dirt"; other namespaces kept.
        std::string Bare(const std::string& id) {
            return id.compare(0, kMcLen, kMc) == 0 ? id.substr(kMcLen) : id;
        }

        // Sorted, unique.
        void SortUnique(std::vector<std::string>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        // Entries starting with `typed` (case-insensitive), in order.
        std::vector<std::string> FilterPrefix(const std::vector<std::string>& entries, const std::string& typed) {
            std::vector<std::string> out;
            for (const std::string& e : entries) {
                if (typed.empty() || StartsWithIgnoreCase(e, typed)) out.push_back(e);
            }
            return out;
        }

        // SharedSuggestionProvider.filterResources over DISPLAY ids (bare for
        // the minecraft namespace, "ns:path" otherwise): with a ':' typed the
        // whole id must match; without, its namespace or its path may — so
        // "creeper" finds entity.creeper.primed and "twilight" every
        // twilightforest: id. Ids starting with what was typed come first
        // (so Tab lands on "stone", not "chiseled_stone_bricks"). Typing the
        // "minecraft:" prefix completes in that form.
        std::vector<std::string> FilterResources(const std::vector<std::string>& ids, const std::string& typedRaw) {
            std::string typed = Lower(typedRaw);
            std::string outPrefix;
            bool onlyVanilla = false;
            if (typed.compare(0, kMcLen, kMc) == 0) {
                typed.erase(0, kMcLen);
                outPrefix = kMc;
                onlyVanilla = true;
            }
            const bool hasNamespace = typed.find(':') != std::string::npos;
            std::vector<std::string> first, rest;
            for (const std::string& id : ids) {
                const size_t colon = id.find(':');
                if (onlyVanilla && colon != std::string::npos) continue;
                if (typed.empty() || id.compare(0, typed.size(), typed) == 0) {
                    first.push_back(outPrefix + id);
                    continue;
                }
                const bool match = hasNamespace
                    ? MatchesSubStr(typed, id)
                    : (colon == std::string::npos
                           ? MatchesSubStr(typed, id)
                           : (MatchesSubStr(typed, id.substr(0, colon)) || MatchesSubStr(typed, id.substr(colon + 1))));
                if (match) rest.push_back(outPrefix + id);
            }
            first.insert(first.end(), rest.begin(), rest.end());
            return first;
        }

        bool IsNumber(const std::string& s, bool integer) {
            if (s.empty()) return false;
            char* end = nullptr;
            if (integer) std::strtol(s.c_str(), &end, 10);
            else         std::strtod(s.c_str(), &end);
            return end && *end == '\0' && end != s.c_str();
        }

        // A coordinate token: [~|^][number], or a plain number.
        bool IsCoord(const std::string& s, bool allowLocal, bool integer) {
            if (s.empty()) return false;
            if (s[0] == '~' || (allowLocal && s[0] == '^')) {
                return s.size() == 1 || IsNumber(s.substr(1), false);
            }
            return IsNumber(s, integer);
        }

        // What a coordinate might still become: "", "-", "1.", "~", "^-".
        bool IsCoordPrefix(const std::string& s) {
            for (char c : s) {
                if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '~' || c == '^' || c == '-' ||
                      c == '.' || c == '+')) {
                    return false;
                }
            }
            return true;
        }

        // ── Registries (the display forms) ─────────────────────────────────

        std::filesystem::path DataRoot() {
            const char* env = std::getenv("MC_DATA_ROOT");
            return std::filesystem::path(env ? env : "data");
        }

        // Every data/<ns>/<dir>/**.json as "ns:path" (bare for minecraft),
        // cached per dir. `tagPrefix` "#" for tag directories.
        const std::vector<std::string>& DataFileIds(const std::string& dir, const char* tagPrefix = "") {
            static std::unordered_map<std::string, std::vector<std::string>> cache;
            auto it = cache.find(dir);
            if (it != cache.end()) return it->second;
            std::vector<std::string> ids;
            std::error_code ec;
            for (const auto& nsDir : std::filesystem::directory_iterator(DataRoot(), ec)) {
                if (!nsDir.is_directory(ec)) continue;
                const std::string ns = nsDir.path().filename().string();
                const std::filesystem::path base = nsDir.path() / dir;
                for (auto e = std::filesystem::recursive_directory_iterator(base, ec);
                     !ec && e != std::filesystem::recursive_directory_iterator(); e.increment(ec)) {
                    if (!e->is_regular_file(ec) || e->path().extension() != ".json") continue;
                    const std::string path =
                        std::filesystem::relative(e->path(), base, ec).replace_extension().generic_string();
                    ids.push_back(tagPrefix + Bare(ns + ":" + path));
                }
                ec.clear();
            }
            SortUnique(ids);
            return cache.emplace(dir, std::move(ids)).first->second;
        }

        const std::vector<std::string>& EntitySlugs(bool summonable) {
            static std::vector<std::string> all, summon;
            if (all.empty()) {
                for (int i = 0; i < Game::kEntityTypeCount; ++i) {
                    const auto& info = Game::kEntityTypeTable[i];
                    if (info.slug.empty()) continue;
                    all.emplace_back(Bare(std::string(info.slug)));
                    if (info.slug != "player") summon.emplace_back(Bare(std::string(info.slug)));
                }
                // A selector's `type=` also knows MC's ids outside the table
                // (player, item, experience_orb, …).
                for (const std::string_view id : Game::kSelectorOnlyEntityTypes) all.emplace_back(id);
                SortUnique(all);
                SortUnique(summon);
            }
            return summonable ? summon : all;
        }

        bool EntityTypeFromText(std::string text, Game::EntityTypeId& out) {
            text = Bare(Lower(text));
            for (int i = 0; i < Game::kEntityTypeCount; ++i) {
                if (Game::kEntityTypeTable[i].slug == text) {
                    out = static_cast<Game::EntityTypeId>(i);
                    return true;
                }
            }
            return false;
        }

        // Block registry slugs — what BlockStateArgument resolves (not the
        // display names). Air and its two aliases are hand-registered.
        const std::vector<std::string>& BlockIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
                ids = {"air", "cave_air", "void_air"};
                for (size_t i = 1; i < static_cast<size_t>(Game::BlockID::Count); ++i) {
                    const std::string& slug = Game::BlockRegistry::Get(static_cast<Game::BlockID>(i)).registrySlug;
                    if (!slug.empty()) ids.push_back(Bare(slug));
                }
                SortUnique(ids);
            }
            return ids;
        }

        bool BlockFromText(const std::string& text, Game::BlockID& out) {
            const std::string id = Bare(Lower(text));
            if (id.empty() || id == "air") return false;
            const Game::BlockState state = Game::BlockStates::FromSlug(id);
            if (Game::BlockRegistry::Get(state.Block()).registrySlug != id) return false;
            out = state.Block();
            return true;
        }

        const std::vector<std::string>& ItemIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
                for (const std::string& s : Game::Morph::ItemSlugs()) ids.push_back(Bare(s));
                SortUnique(ids);
            }
            return ids;
        }

        const std::vector<std::string>& EffectIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
                for (int i = 0; i < Game::kMobEffectCount; ++i) {
                    ids.push_back(Bare(Game::GetEffectName(static_cast<Game::MobEffectId>(i))));
                }
                SortUnique(ids);
            }
            return ids;
        }

        // /place feature: the configured features the server can run
        // (the same list LiveFeatureLevel resolves).
        const std::vector<std::string>& ConfiguredFeatureIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
#define CONFIGURED_FEATURE(id, cls, name) ids.push_back(Bare(id));
#include "common/world/level/ConfiguredFeatureIds.inc"
#undef CONFIGURED_FEATURE
                SortUnique(ids);
            }
            return ids;
        }

        const std::vector<std::string>& EnchantmentIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
                for (const Game::Enchantment& e : Game::EnchantmentRegistry::All()) ids.push_back(Bare(e.slug));
                SortUnique(ids);
            }
            return ids;
        }

        // MC SuggestionProviders.AVAILABLE_SOUNDS: the client's sound
        // registry (vanilla sounds.json, the engine's overlays, resource
        // packs), plus the vanilla event list so completion works before the
        // sound assets exist. Rebuilt per request: packs can reload.
        std::vector<std::string> SoundIds() {
            std::vector<std::string> ids;
            ids.reserve(static_cast<size_t>(Game::SoundEvents::Count()) + 256);
            for (int i = 0; i < Game::SoundEvents::Count(); ++i) {
                if (const char* id = Game::SoundEvents::ByRegistryId(i)) ids.push_back(Bare(id));
            }
            for (const auto& [id, events] : Client::SoundManager::Get().Registry().All()) {
                (void)events;
                ids.push_back(Bare(id));
            }
            SortUnique(ids);
            return ids;
        }

        const std::vector<std::string>& ParticleIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
                for (const auto& info : Game::ParticleTypes::All()) {
                    if (info.commandVisible) ids.push_back(Bare(info.name));
                }
                SortUnique(ids);
            }
            return ids;
        }

        // MC's attribute registry (BuiltInRegistries.ATTRIBUTE), bare ids.
        const std::vector<std::string>& AttributeIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
                for (size_t i = 0; i < static_cast<size_t>(Game::Attribute::Count); ++i) {
                    ids.emplace_back(Game::kAttributeTable[i].name);
                }
                SortUnique(ids);
            }
            return ids;
        }

        // SetOwnAttributes' rows.
        std::vector<OwnAttribute> s_ownAttributes;

        const OwnAttribute* OwnAttributeRow(const std::string& typedId) {
            Game::Attribute attribute{};
            if (!Game::AttributeFromName(Lower(typedId), attribute)) return nullptr;
            const std::string_view name = Game::AttributeName(attribute);
            for (const OwnAttribute& row : s_ownAttributes) {
                if (row.id == name) return &row;
            }
            return nullptr;
        }

        // A double the way Java's Double.toString writes the common ones
        // ("0.6", "20.0") — what a value suggestion offers.
        std::string AttributeNumber(double v) {
            char buf[64];
            for (int precision = 1; precision <= 17; ++precision) {
                std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
                if (std::strtod(buf, nullptr) == v) break;
            }
            std::string out = buf;
            if (out.find_first_of(".eEn") == std::string::npos) out += ".0";
            return out;
        }

        const std::vector<std::string>& GameRuleIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
                const Game::Rules::Def* defs = Game::Rules::AllDefs();
                for (size_t i = 0; i < Game::Rules::kCount; ++i) ids.emplace_back(defs[i].key);
                SortUnique(ids);
            }
            return ids;
        }

        // The engine's dimension names, as /dimension and /execute in take them.
        const std::vector<std::string>& DimensionIds() {
            static const std::vector<std::string> ids = {"aether", "end", "hush", "nether", "overworld", "twilight"};
            return ids;
        }

        // The biome registry (every dimension's): the engine biome table.
        const std::vector<std::string>& BiomeIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
                for (Game::BiomeId id = 0; id < Game::BiomeRegistry::Count(); ++id) {
                    ids.push_back(Bare(std::string(Game::BiomeRegistry::Get(id).name)));
                }
                SortUnique(ids);
            }
            return ids;
        }

        // Tags without a '/' in their path — what MC lists before a '#' is typed.
        std::vector<std::string> TopLevel(const std::vector<std::string>& tags) {
            std::vector<std::string> out;
            for (const std::string& t : tags) {
                if (t.find('/') == std::string::npos) out.push_back(t);
            }
            return out;
        }

        // ── The current dimension's ids (WorldgenIdsS2C) ───────────────────
        // What /locate offers once the server has said where the player is:
        // only this dimension's biomes and structures (and the tags with a
        // member here), shown by path — "hush_meadows" — unless two
        // namespaces share a path. The server resolves a bare name against
        // the player's dimension first (LocateFinder CanonicalWorldgenId).
        struct DimensionWorldgen {
            bool received = false;
            std::vector<std::string> biomes, structures;         // display names
            std::vector<std::string> biomeTags, structureTags;   // "#name"
        };
        DimensionWorldgen s_dimension;

        std::vector<std::string> DisplayNames(const std::vector<std::string>& ids, bool tags) {
            auto path = [&](const std::string& id) {
                const std::string body = tags ? id.substr(1) : id;
                const size_t colon = body.find(':');
                return colon == std::string::npos ? body : body.substr(colon + 1);
            };
            std::unordered_map<std::string, int> uses;
            for (const std::string& id : ids) ++uses[path(id)];
            std::vector<std::string> out;
            out.reserve(ids.size());
            for (const std::string& id : ids) {
                const std::string p = path(id);
                out.push_back(uses[p] > 1 ? (tags ? "#" + Bare(id.substr(1)) : Bare(id)) : (tags ? "#" + p : p));
            }
            SortUnique(out);
            return out;
        }

        // MC's substring rule against a shown name: "mead" offers
        // hush_meadows, "#in" the in_* tags.
        std::vector<std::string> FilterNames(const std::vector<std::string>& names, const std::string& typedRaw) {
            const std::string typed = Lower(typedRaw);
            std::vector<std::string> first, rest;
            for (const std::string& name : names) {
                if (typed.empty() || name.compare(0, typed.size(), typed) == 0) { first.push_back(name); continue; }
                if (MatchesSubStr(typed, name) ||
                    (typed[0] == '#' && name[0] == '#' && MatchesSubStr(typed.substr(1), name.substr(1)))) {
                    rest.push_back(name);
                }
            }
            first.insert(first.end(), rest.begin(), rest.end());
            return first;
        }

        // Every known player (self + remotes), case-insensitively sorted.
        std::vector<std::string> PlayerNames() {
            std::vector<std::string> names;
            if (Client::g_networkClient) {
                const std::string& self = Client::g_networkClient->GetPlayerName();
                if (!self.empty()) names.push_back(self);
            }
            if (Client::g_remotePlayerManager) {
                for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                    (void)id;
                    if (!rp.name.empty()) names.push_back(rp.name);
                }
            }
            std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
                return Lower(a) < Lower(b);
            });
            names.erase(std::unique(names.begin(), names.end()), names.end());
            return names;
        }

        // ── Trees ──────────────────────────────────────────────────────────

        std::vector<std::string> s_entityNames;   // CommandsS2C: known custom names

        std::unordered_map<std::string, Node> s_serverTrees;
        bool     s_serverTreesReceived = false;
        uint64_t s_treeGeneration = 1;   // bumped by SetServerSyntax (the hint cache key)

        // Client-side commands (ChatScreen handles them without the server).
        const std::unordered_map<std::string, Node>& LocalTrees() {
            static const std::unordered_map<std::string, Node> trees = [] {
                std::unordered_map<std::string, Node> t;
                Node clearchat = Game::Cmd::Literal("clearchat");
                clearchat.executable = true;
                t.emplace("clearchat", std::move(clearchat));
                return t;
            }();
            return trees;
        }

        const Node* FindTree(const std::string& lowerName) {
            if (auto it = s_serverTrees.find(lowerName); it != s_serverTrees.end()) return &it->second;
            if (auto it = LocalTrees().find(lowerName); it != LocalTrees().end()) return &it->second;
            return nullptr;
        }

        std::vector<std::string> CommandNames() {
            std::vector<std::string> names = Render::GetServerCommandNames();
            for (const auto& [name, tree] : LocalTrees()) { (void)tree; names.push_back(name); }
            SortUnique(names);
            return names;
        }

        // ── Tokens ─────────────────────────────────────────────────────────

        struct Token {
            int         start = 0;   // index in the full text
            std::string text;
        };

        // [begin, end) split at spaces outside brackets, braces and quotes —
        // so `@e[type=cow, limit=1]` and `dust{color:[1,0,0], scale:1}` stay
        // one token each, as the server's argument readers see them. The
        // last token is the one being typed (`partial`, empty after a space).
        void Tokenize(const std::string& s, int begin, int end,
                      std::vector<Token>& complete, Token& partial) {
            complete.clear();
            Token cur;
            cur.start = -1;
            int  depth = 0;
            char quote = 0;
            for (int i = begin; i < end; ++i) {
                const char c = s[static_cast<size_t>(i)];
                if (quote) {
                    cur.text += c;
                    if (c == '\\' && i + 1 < end) cur.text += s[static_cast<size_t>(++i)];
                    else if (c == quote) quote = 0;
                    continue;
                }
                if (c == ' ' && depth == 0) {
                    if (cur.start >= 0) complete.push_back(std::move(cur));
                    cur = Token{};
                    cur.start = -1;
                    continue;
                }
                if (cur.start < 0) cur.start = i;
                // A quote opens a string inside brackets/braces or at a
                // token's start (as the server's tokenizer reads it); inside
                // a plain word ("don't") it is just a character.
                const bool opensQuote = (c == '"' || c == '\'') && (depth > 0 || cur.text.empty());
                cur.text += c;
                if (opensQuote) quote = c;
                else if (c == '[' || c == '{' || c == '(') ++depth;
                else if ((c == ']' || c == '}' || c == ')') && depth > 0) --depth;
            }
            if (cur.start >= 0) {
                partial = std::move(cur);
            } else {
                partial = Token{};
                partial.start = end;
            }
        }

        // ── Walking a tree ─────────────────────────────────────────────────

        struct Step {
            const Node* node = nullptr;
            std::string value;   // what was typed for it
        };

        // A node the token under the cursor belongs to.
        struct Candidate {
            const Node*              node = nullptr;
            int                      component = 0;   // which of its tokens the cursor is in
            std::vector<std::string> prior;           // its tokens already typed (multi-token args)
            std::vector<Step>        path;            // what led here
            bool                     optional = false;   // its parent could end the command
        };

        struct Walk {
            const std::vector<Token>& toks;
            const Token&              partial;
            const Node&               root;
            std::vector<Candidate>    candidates;
            std::vector<int>          subCommands;   // text offsets where a `run` command starts
            int                       budget = 20000;
        };

        bool Accepts(Arg type, const std::vector<const Token*>& tokens) {
            switch (type) {
                case Arg::Integer: return IsNumber(tokens[0]->text, true);
                case Arg::Float:
                case Arg::AttributeValue: return IsNumber(tokens[0]->text, false);
                case Arg::Bool:    return tokens[0]->text == "true" || tokens[0]->text == "false";
                case Arg::Vec3:
                case Arg::Vec2:
                    for (const Token* t : tokens) if (!IsCoord(t->text, true, false)) return false;
                    return true;
                case Arg::BlockPos:
                case Arg::ColumnPos:
                    for (const Token* t : tokens) if (!IsCoord(t->text, true, true)) return false;
                    return true;
                case Arg::Rotation:
                    for (const Token* t : tokens) if (!IsCoord(t->text, false, false)) return false;
                    return true;
                case Arg::Player:
                case Arg::Players:
                case Arg::Entity:
                case Arg::Entities: {
                    const std::string& t = tokens[0]->text;
                    // A name or selector never starts like a coordinate —
                    // the rule TeleportCommand splits its forms by.
                    if (t.empty()) return false;
                    return t[0] == '@' || !IsCoord(t, true, false);
                }
                case Arg::GameMode: {
                    static const std::unordered_set<std::string> modes = {
                        "survival", "creative", "adventure", "spectator", "s", "c", "a", "sp", "0", "1", "2", "3"};
                    return modes.count(Lower(tokens[0]->text)) != 0;
                }
                case Arg::SoundSource: {
                    const std::string t = Lower(tokens[0]->text);
                    for (int i = 0; i < Game::kSoundSourceCount; ++i) {
                        if (t == Game::SoundSourceName(static_cast<Game::SoundSource>(i))) return true;
                    }
                    return false;
                }
                case Arg::Time: {
                    std::string t = tokens[0]->text;
                    if (!t.empty() && (t.back() == 'd' || t.back() == 's' || t.back() == 't')) t.pop_back();
                    return IsNumber(t, false);
                }
                default:
                    return true;
            }
        }

        // Could the token under the cursor still become a value of `node`?
        // (Only prunes the hint's branches.)
        bool CouldAccept(const Node& node, const std::string& partial) {
            if (partial.empty()) return true;
            if (node.IsLiteral()) return StartsWithIgnoreCase(node.name, partial);
            switch (node.type) {
                case Arg::Integer:
                case Arg::Float:
                case Arg::AttributeValue:
                case Arg::Vec3:
                case Arg::Vec2:
                case Arg::BlockPos:
                case Arg::ColumnPos:
                case Arg::Rotation:
                    return IsCoordPrefix(partial);
                case Arg::Bool:
                    return StartsWithIgnoreCase("true", partial) || StartsWithIgnoreCase("false", partial);
                case Arg::Player:
                case Arg::Players:
                case Arg::Entity:
                case Arg::Entities:
                    return partial[0] == '@' || !IsCoordPrefix(partial);
                default:
                    return true;
            }
        }

        bool EndsListElement(const std::string& token) {
            return !token.empty() && token.back() == ',';
        }

        void WalkFrom(Walk& w, const Node& at, size_t i, std::vector<Step>& path) {
            if (--w.budget < 0) return;
            const std::vector<Node>& kids = at.redirectRoot ? w.root.children : at.children;
            const bool optional = at.executable;

            if (i == w.toks.size()) {
                for (const Node& kid : kids) {
                    if (kid.type == Arg::Command) { w.subCommands.push_back(w.partial.start); continue; }
                    Candidate c;
                    c.node = &kid;
                    c.path = path;
                    c.optional = optional;
                    w.candidates.push_back(std::move(c));
                }
                return;
            }

            // Brigadier: a literal that matches wins over every argument.
            const Token& token = w.toks[i];
            bool literal = false;
            for (const Node& kid : kids) {
                if (!kid.IsLiteral() || !EqualsIgnoreCase(kid.name, token.text)) continue;
                literal = true;
                path.push_back({&kid, token.text});
                WalkFrom(w, kid, i + 1, path);
                path.pop_back();
            }
            if (literal) return;

            for (const Node& kid : kids) {
                if (kid.IsLiteral()) continue;
                if (kid.type == Arg::Command) { w.subCommands.push_back(token.start); continue; }
                const int arity = Game::Cmd::TokenArity(kid.type);
                // The greedy kinds take the rest of the line: the cursor is
                // always inside them.
                if (kid.type == Arg::Greedy || kid.type == Arg::Message || kid.type == Arg::TextComponent) {
                    Candidate c;
                    c.node = &kid;
                    c.component = static_cast<int>(w.toks.size() - i);
                    for (size_t k = i; k < w.toks.size(); ++k) c.prior.push_back(w.toks[k].text);
                    c.path = path;
                    c.optional = optional;
                    w.candidates.push_back(std::move(c));
                    continue;
                }
                if (arity == 0) {
                    // A comma list runs on while its tokens end in ','.
                    size_t j = i;
                    while (j < w.toks.size() && EndsListElement(w.toks[j].text)) ++j;
                    if (j == w.toks.size()) {
                        Candidate c;
                        c.node = &kid;
                        c.component = static_cast<int>(j - i);
                        for (size_t k = i; k < j; ++k) c.prior.push_back(w.toks[k].text);
                        c.path = path;
                        c.optional = optional;
                        w.candidates.push_back(std::move(c));
                        continue;
                    }
                    std::string value;
                    for (size_t k = i; k <= j; ++k) value += (k > i ? " " : "") + w.toks[k].text;
                    path.push_back({&kid, value});
                    WalkFrom(w, kid, j + 1, path);
                    path.pop_back();
                    continue;
                }
                std::vector<const Token*> span;
                for (size_t k = i; k < w.toks.size() && k < i + static_cast<size_t>(arity); ++k) span.push_back(&w.toks[k]);
                if (span.size() < static_cast<size_t>(arity)) {
                    // The cursor is inside this argument (its later tokens).
                    if (!Accepts(kid.type, span)) continue;
                    Candidate c;
                    c.node = &kid;
                    c.component = static_cast<int>(span.size());
                    for (const Token* t : span) c.prior.push_back(t->text);
                    c.path = path;
                    c.optional = optional;
                    w.candidates.push_back(std::move(c));
                    continue;
                }
                if (!Accepts(kid.type, span)) continue;
                std::string value;
                for (size_t k = 0; k < span.size(); ++k) value += (k ? " " : "") + span[k]->text;
                path.push_back({&kid, value});
                WalkFrom(w, kid, i + static_cast<size_t>(arity), path);
                path.pop_back();
            }
        }

        const std::string* ValueOf(const std::vector<Step>& path, const char* name) {
            for (auto it = path.rbegin(); it != path.rend(); ++it) {
                if (it->node && !it->node->IsLiteral() && it->node->name == name) return &it->value;
            }
            return nullptr;
        }

        // ── Providers (MC ArgumentType.listSuggestions) ────────────────────
        //
        // Each returns entries for the token being typed, replacing from
        // `anchor` (an index in the full text, at or after the token's start).

        struct Provided {
            int                      anchor = 0;
            std::vector<std::string> items;
        };

        // Selector options this engine's EntitySelector parses.
        const std::vector<std::string>& SelectorKeys() {
            static const std::vector<std::string> keys = {
                "distance=", "dx=", "dy=", "dz=", "gamemode=", "limit=", "name=", "nbt=",
                "sort=", "tag=", "type=", "x=", "x_rotation=", "y=", "y_rotation=", "z="};
            return keys;
        }

        // Defined with the SNBT completion below.
        bool SelectorNbtCompletion(const Token& partial, size_t bracket, Provided& out);

        // EntityArgument: player names and selectors, with `@e[key=value,...]`
        // completing inside the brackets.
        Provided Entities(Arg type, const Token& partial) {
            Provided out;
            out.anchor = partial.start;
            const std::string& word = partial.text;
            std::vector<std::string> selectors;
            switch (type) {
                case Arg::Player:     selectors = {"@p", "@r", "@s"}; break;
                case Arg::Players:    selectors = {"@a", "@a[", "@p", "@r", "@s"}; break;
                case Arg::Entity:     selectors = {"@e[", "@n", "@p", "@r", "@s"}; break;
                case Arg::Entities:   selectors = {"@a", "@e", "@e[", "@n", "@p", "@r", "@s"}; break;
                default: break;
            }
            const size_t bracket = word.find('[');
            if (word.empty() || word[0] != '@' || bracket == std::string::npos) {
                std::vector<std::string> all;
                if (word.empty() || word[0] != '@') all = PlayerNames();
                all.insert(all.end(), selectors.begin(), selectors.end());
                out.items = FilterPrefix(all, word);
                return out;
            }
            // Inside an `nbt={...}` value: the compound's keys and values.
            if (SelectorNbtCompletion(partial, bracket, out)) return out;
            // Inside the brackets: the slot is the run since the last '[',
            // ',' or '=' (spaces after a ',' skipped).
            size_t sub = bracket + 1;
            for (size_t i = sub; i < word.size(); ++i) {
                const char c = word[i];
                if (c == '[' || c == ',' || c == '=') sub = i + 1;
            }
            while (sub < word.size() && word[sub] == ' ') ++sub;
            const char prev = [&] {
                size_t p = sub;
                while (p > 0 && word[p - 1] == ' ') --p;
                return p > 0 ? word[p - 1] : '[';
            }();
            std::string slot = word.substr(sub);
            out.anchor = partial.start + static_cast<int>(sub);
            if (prev == ']') return out;
            if (prev == '=') {
                size_t ke = sub;
                while (ke > 0 && word[ke - 1] != '=') --ke;
                size_t ks = ke > 0 ? ke - 1 : 0;
                while (ks > bracket + 1 && word[ks - 1] != ',' && word[ks - 1] != '[') --ks;
                std::string key = Lower(word.substr(ks, ke - 1 - ks));
                key.erase(0, key.find_first_not_of(' '));
                // A negated value keeps its '!'.
                if (!slot.empty() && slot[0] == '!') {
                    slot.erase(0, 1);
                    ++out.anchor;
                }
                std::vector<std::string> values;
                if (key == "type") {
                    // Every entity type, then the entity_type tags (MC offers
                    // both; a `#` shows the tags alone).
                    const auto& tags = DataFileIds("tags/entity_type", "#");
                    if (!slot.empty() && slot[0] == '#') return Provided{out.anchor, FilterNames(tags, slot)};
                    std::vector<std::string> items = FilterResources(EntitySlugs(false), slot);
                    if (slot.empty()) {
                        const auto top = TopLevel(tags);
                        items.insert(items.end(), top.begin(), top.end());
                    }
                    return Provided{out.anchor, std::move(items)};
                }
                else if (key == "sort")     values = {"arbitrary", "furthest", "nearest", "random"};
                else if (key == "gamemode") values = {"adventure", "creative", "spectator", "survival"};
                else if (key == "limit")    values = {"1", "10", "100"};
                else if (key == "name") {
                    // Players, then the server's named entities; MC readString
                    // needs quotes around anything but [0-9A-Za-z_.+-].
                    values = PlayerNames();
                    for (const std::string& n : s_entityNames) {
                        const bool plain = !n.empty() && std::all_of(n.begin(), n.end(), [](char ch) {
                            return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.' ||
                                   ch == '+' || ch == '-';
                        });
                        std::string quoted = "\"";
                        for (char ch : n) {
                            if (ch == '"' || ch == '\\') quoted += '\\';
                            quoted += ch;
                        }
                        quoted += '"';
                        values.push_back(plain ? n : quoted);
                    }
                    values.erase(std::unique(values.begin(), values.end()), values.end());
                    // A quote typed first still matches the quoted entries.
                }
                else if (key == "distance") values = {"..10", "..5", "1..5", "5.."};
                else if (key == "x_rotation" || key == "y_rotation") values = {"-90..0", "0..90"};
                else if (key == "x" || key == "y" || key == "z" || key == "dx" || key == "dy" || key == "dz") values = {"0", "1"};
                out.items = FilterPrefix(values, slot);
                return out;
            }
            std::vector<std::string> keys = SelectorKeys();
            if (prev == ',' || sub > bracket + 1) keys.insert(keys.begin(), "]");
            out.items = FilterPrefix(keys, slot);
            return out;
        }

        // BlockStateArgument / BlockPredicateArgument for one element: ids
        // (and #tags), then inside `[...]` the block's properties and values.
        std::vector<std::string> BlockElement(const std::string& typed, bool predicate, bool list, int& offset) {
            offset = 0;
            const size_t bracket = typed.find('[');
            if (bracket == std::string::npos) {
                std::vector<std::string> items;
                if (predicate && !typed.empty() && typed[0] == '#') {
                    const auto& tags = DataFileIds("tags/block", "#");
                    return FilterNames(tags, typed);
                }
                items = FilterResources(BlockIds(), typed);
                if (predicate && typed.empty()) {
                    const auto top = TopLevel(DataFileIds("tags/block", "#"));
                    items.insert(items.end(), top.begin(), top.end());
                }
                // A whole id typed: offer to open its state, or (in a list) the next element.
                Game::BlockID block{};
                if (BlockFromText(typed, block)) {
                    if (Game::BlockStates::PropertyCount(block) > 0) items.insert(items.begin() + 1, typed + "[");
                    if (list) items.push_back(typed + ",");
                }
                return items;
            }
            // Properties: `id[name=value,...]`.
            Game::BlockID block{};
            if (!BlockFromText(typed.substr(0, bracket), block)) return {};
            size_t sub = bracket + 1;
            for (size_t i = sub; i < typed.size(); ++i) {
                if (typed[i] == ',' || typed[i] == '=' || typed[i] == ']') sub = i + 1;
            }
            const char prev = typed[sub - 1];
            offset = static_cast<int>(sub);
            const std::string slot = typed.substr(sub);
            const uint16_t count = Game::BlockStates::PropertyCount(block);
            std::vector<std::string> items;
            if (prev == ']') {
                offset = static_cast<int>(typed.size());
                if (list) items.push_back(",");
                return items;
            }
            if (prev == '=') {
                size_t ks = sub - 1;
                while (ks > bracket + 1 && typed[ks - 1] != ',' && typed[ks - 1] != '[') --ks;
                const std::string name = typed.substr(ks, sub - 1 - ks);
                for (uint16_t slot_ = 0; slot_ < count; ++slot_) {
                    const Game::PropertyId prop = Game::BlockStates::PropertyAt(block, slot_);
                    if (Game::BlockStates::PropertyName(prop) != name) continue;
                    for (uint16_t v = 0; v < Game::BlockStates::PropertyValueCount(prop); ++v) {
                        items.emplace_back(Game::BlockStates::PropertyValueName(prop, v));
                    }
                }
                items = FilterPrefix(items, slot);
                // A complete value: close or continue.
                for (const std::string& v : items) {
                    if (v == slot) {
                        items = {slot + ",", slot + "]"};
                        break;
                    }
                }
                return items;
            }
            // Key position: the properties not named yet.
            const std::string inside = typed.substr(bracket + 1);
            for (uint16_t s = 0; s < count; ++s) {
                const std::string name(Game::BlockStates::PropertyName(Game::BlockStates::PropertyAt(block, s)));
                if (inside.find(name + "=") != std::string::npos) continue;
                items.push_back(name + "=");
            }
            std::sort(items.begin(), items.end());
            items = FilterPrefix(items, slot);
            if (slot.empty()) items.push_back("]");
            return items;
        }

        // Comma lists: complete the element after the last top-level ','.
        Provided ListElement(const Token& partial, const std::function<std::vector<std::string>(const std::string&, int&)>& element) {
            const std::string& word = partial.text;
            size_t start = 0;
            int depth = 0;
            for (size_t i = 0; i < word.size(); ++i) {
                if (word[i] == '[') ++depth;
                else if (word[i] == ']' && depth > 0) --depth;
                else if (word[i] == ',' && depth == 0) start = i + 1;
            }
            while (start < word.size() && word[start] == ' ') ++start;
            int offset = 0;
            Provided out;
            out.items = element(word.substr(start), offset);
            out.anchor = partial.start + static_cast<int>(start) + offset;
            return out;
        }

        // ── SNBT compound completion (particle options, entity NBT) ────────

        struct CompoundSlot {
            enum class Mode { None, Key, Value, AfterValue } mode = Mode::None;
            std::string key;        // for Value
            size_t      start = 0;  // where the slot begins in the text
        };

        // Where the end of `text` (which starts at a '{') is, within the
        // top-level compound: typing a key, a value, or just past a value.
        CompoundSlot LocateInCompound(const std::string& text) {
            CompoundSlot slot;
            if (text.empty() || text[0] != '{') return slot;
            int depth = 0;
            char quote = 0;
            enum class State { Key, Value, AfterValue } state = State::Key;
            size_t slotStart = 1;
            std::string key;
            size_t keyStart = 1;
            for (size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (quote) {
                    if (c == '\\') { ++i; continue; }
                    if (c == quote) {
                        quote = 0;
                        if (depth == 1 && state == State::Value) state = State::AfterValue;
                    }
                    continue;
                }
                if (c == '"' || c == '\'') { quote = c; continue; }
                if (c == '{' || c == '[') {
                    ++depth;
                    if (depth == 1) { state = State::Key; slotStart = i + 1; keyStart = i + 1; }
                    continue;
                }
                if (c == '}' || c == ']') {
                    --depth;
                    if (depth == 1 && state == State::Value) state = State::AfterValue;
                    if (depth <= 0) return slot;   // compound closed
                    continue;
                }
                if (depth != 1) continue;
                if (c == ':' && state == State::Key) {
                    key = text.substr(keyStart, i - keyStart);
                    key.erase(0, key.find_first_not_of(' '));
                    if (!key.empty() && (key.front() == '"' || key.front() == '\'')) key = key.substr(1, key.size() - 2);
                    state = State::Value;
                    slotStart = i + 1;
                } else if (c == ',') {
                    state = State::Key;
                    slotStart = i + 1;
                    keyStart = i + 1;
                }
            }
            if (depth != 1) return slot;
            while (slotStart < text.size() && text[slotStart] == ' ') ++slotStart;
            slot.start = slotStart;
            if (state == State::Key) {
                slot.mode = CompoundSlot::Mode::Key;
            } else if (state == State::Value) {
                // A plain value typed to the end (a number) reads as still being typed.
                slot.mode = CompoundSlot::Mode::Value;
                slot.key = key;
            } else {
                slot.mode = CompoundSlot::Mode::AfterValue;
                slot.start = text.size();
            }
            return slot;
        }

        struct NbtKey {
            std::string              name;
            std::vector<std::string> values;
        };

        // Complete inside `word` (which starts with '{'): keys, then values.
        Provided CompoundCompletion(const Token& partial, const std::vector<NbtKey>& keys) {
            Provided out;
            out.anchor = partial.start;
            const std::string& word = partial.text;
            if (word.empty()) {
                for (const NbtKey& k : keys) out.items.push_back("{" + k.name + ":");
                return out;
            }
            if (word[0] != '{') return out;
            const CompoundSlot slot = LocateInCompound(word);
            out.anchor = partial.start + static_cast<int>(slot.start);
            const std::string typed = word.substr(std::min(slot.start, word.size()));
            switch (slot.mode) {
                case CompoundSlot::Mode::None:
                    break;
                case CompoundSlot::Mode::Key: {
                    std::vector<std::string> names;
                    for (const NbtKey& k : keys) {
                        if (word.find(k.name + ":") == std::string::npos) names.push_back(k.name + ":");
                    }
                    out.items = FilterPrefix(names, typed);
                    if (typed.empty()) out.items.push_back("}");
                    break;
                }
                case CompoundSlot::Mode::Value: {
                    for (const NbtKey& k : keys) {
                        if (k.name != slot.key) continue;
                        std::vector<std::string> first, rest;
                        const std::string bareTyped = (!typed.empty() && (typed[0] == '"' || typed[0] == '\''))
                            ? typed.substr(1) : typed;
                        for (const std::string& v : k.values) {
                            const std::string bareValue = (!v.empty() && v[0] == '"') ? v.substr(1) : v;
                            if (StartsWithIgnoreCase(v, typed) || StartsWithIgnoreCase(bareValue, bareTyped)) {
                                first.push_back(v);
                            } else if (!bareTyped.empty() && MatchesSubStr(Lower(bareTyped), Lower(bareValue))) {
                                rest.push_back(v);
                            }
                        }
                        first.insert(first.end(), rest.begin(), rest.end());
                        out.items = std::move(first);
                        break;
                    }
                    break;
                }
                case CompoundSlot::Mode::AfterValue:
                    out.items = {",", "}"};
                    break;
            }
            return out;
        }

        // The keys every mob type shares — what `nbt=` offers before a
        // `type=` narrows it.
        const std::vector<NbtKey>& CommonNbtKeys() {
            static std::vector<NbtKey> keys;
            static bool built = false;
            if (built) return keys;
            built = true;
            std::unordered_map<std::string, int> seen;
            int types = 0;
            for (int i = 0; i < Game::kEntityTypeCount; ++i) {
                const auto type = static_cast<Game::EntityTypeId>(i);
                if (Game::GetEntityTypeInfo(type).category == Game::MobCategory::Misc) continue;
                ++types;
                for (const Game::EntityNbtHints::Key& k : Game::EntityNbtHints::KeysFor(type)) {
                    if (seen[k.name]++ == types - 1 && types == 1) keys.push_back({k.name, k.values});
                }
            }
            keys.erase(std::remove_if(keys.begin(), keys.end(),
                                      [&](const NbtKey& k) { return seen[k.name] < types; }),
                       keys.end());
            return keys;
        }

        // `@e[...,nbt={...` with the cursor inside the compound: its keys
        // (the `type=` option's, when one is given) and values.
        bool SelectorNbtCompletion(const Token& partial, size_t bracket, Provided& out) {
            const std::string& word = partial.text;
            // The option the cursor is in: the run since the last ',' or '['
            // at nesting depth zero (a compound's own commas don't count).
            size_t optionStart = bracket + 1;
            int depth = 0;
            char quote = 0;
            std::string typeValue;
            const auto noteOption = [&](size_t from, size_t to) {
                const std::string option = word.substr(from, to - from);
                const size_t eq = option.find('=');
                if (eq == std::string::npos) return;
                std::string key = option.substr(0, eq);
                key.erase(0, key.find_first_not_of(' '));
                if (Lower(key) == "type") typeValue = option.substr(eq + 1);
            };
            for (size_t i = bracket + 1; i < word.size(); ++i) {
                const char c = word[i];
                if (quote) {
                    if (c == '\\') { ++i; continue; }
                    if (c == quote) quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'') { quote = c; continue; }
                if (c == '{' || c == '[') ++depth;
                else if ((c == '}' || c == ']') && depth > 0) --depth;
                else if (c == ',' && depth == 0) { noteOption(optionStart, i); optionStart = i + 1; }
            }
            std::string option = word.substr(optionStart);
            const size_t lead = option.find_first_not_of(' ');
            if (lead == std::string::npos) return false;
            const size_t eq = option.find('=');
            if (eq == std::string::npos || Lower(option.substr(lead, eq - lead)) != "nbt") return false;
            size_t valueStart = optionStart + eq + 1;
            if (valueStart < word.size() && word[valueStart] == '!') ++valueStart;
            Token value;
            value.start = partial.start + static_cast<int>(valueStart);
            value.text = word.substr(valueStart);
            std::vector<NbtKey> keys;
            Game::EntityTypeId type{};
            if (!typeValue.empty() && typeValue[0] != '!' && EntityTypeFromText(typeValue, type)) {
                for (const Game::EntityNbtHints::Key& k : Game::EntityNbtHints::KeysFor(type)) keys.push_back({k.name, k.values});
            } else {
                keys = CommonNbtKeys();
            }
            out = CompoundCompletion(value, keys);
            return true;
        }

        // ── Item component values ──────────────────────────────────────────
        //
        // Each settable component's value shape (ItemStackNbt ReadItemStack's
        // codecs), with every leaf naming the registry its suggestions come
        // from — the same lists the other argument types offer (sounds as
        // /playsound, items as /give, enchantments, effects …), so nothing
        // here is a hand-kept example list except plain numbers and enums.

        enum class ValueKind : uint8_t {
            Text,          // a text component: "…" or {text:"…",color:…}
            Sound, Item, ItemOrTag, Effect, Potion, EntityType, Block, Enchantment,
            Instrument, BannerPattern, Painting, DyeColor, DamageTypeTag, Axolotl,
            Bool, Unit, Number, Enum, Compound, List,
            EnchantmentMap,   // {<enchantment>:<level>, ...}
            Attribute,        // the attribute registry
            BlockOrTag,       // a block id or #block tag (HolderSet<Block>)
            EntityTypeOrTag,  // an entity type or #entity_type tag
            DataFile,         // data/<ns>/<values[0]>/*.json ids (values[1]: its tag dir)
            Variant,          // an entity variant component's names (values[0]: the component)
            ComponentName,    // an item component name (tooltip_display's hidden list)
        };

        struct ValueSpec {
            ValueKind kind = ValueKind::Number;
            std::vector<std::string> values;   // Number examples / Enum names
            std::vector<std::pair<std::string, const ValueSpec*>> fields;   // Compound
            const ValueSpec* element = nullptr;   // List
        };

        const ValueSpec* ComponentSpec(const std::string& component) {
            static std::vector<std::unique_ptr<ValueSpec>> pool;
            static std::unordered_map<std::string, const ValueSpec*> specs;
            if (!specs.empty()) {
                auto it = specs.find(component);
                return it == specs.end() ? nullptr : it->second;
            }
            const auto make = [&](ValueKind kind, std::vector<std::string> values = {}) {
                pool.push_back(std::make_unique<ValueSpec>());
                pool.back()->kind = kind;
                pool.back()->values = std::move(values);
                return pool.back().get();
            };
            const auto compound = [&](std::vector<std::pair<std::string, const ValueSpec*>> fields) {
                ValueSpec* spec = make(ValueKind::Compound);
                spec->fields = std::move(fields);
                return spec;
            };
            const auto list = [&](const ValueSpec* element) {
                ValueSpec* spec = make(ValueKind::List);
                spec->element = element;
                return spec;
            };
            const ValueSpec* text    = make(ValueKind::Text);
            const ValueSpec* boolean = make(ValueKind::Bool);
            const ValueSpec* item    = make(ValueKind::Item);
            const ValueSpec* dye     = make(ValueKind::DyeColor);
            const ValueSpec* rgb     = make(ValueKind::Number, {"16711680", "65280", "255", "16777215"});
            const ValueSpec* rgbList = make(ValueKind::Number, {"[I;16711680]", "[I;255,16776960]"});
            const ValueSpec* effect  = make(ValueKind::Effect);
            const ValueSpec* ticks   = make(ValueKind::Number, {"160", "600", "3600"});
            const ValueSpec* count   = make(ValueKind::Number, {"1", "16", "64"});
            const ValueSpec* stack   = compound({{"id", item}, {"count", count}});
            const ValueSpec* firework = compound({
                {"shape", make(ValueKind::Enum, {"small_ball", "large_ball", "star", "creeper", "burst"})},
                {"colors", rgbList}, {"fade_colors", rgbList},
                {"has_trail", boolean}, {"has_twinkle", boolean}});
            const ValueSpec* mobEffect = compound({
                {"id", effect}, {"amplifier", make(ValueKind::Number, {"0", "1", "4"})}, {"duration", ticks},
                {"ambient", boolean}, {"show_particles", boolean}, {"show_icon", boolean}});
            const ValueSpec* sound      = make(ValueKind::Sound);
            const ValueSpec* unit01     = make(ValueKind::Number, {"0.0", "0.5", "1.0"});
            const ValueSpec* reach      = make(ValueKind::Number, {"0.0", "3.0", "5.0"});
            const ValueSpec* damageTag  = make(ValueKind::DamageTypeTag);
            const ValueSpec* blockOrTag = make(ValueKind::BlockOrTag);
            const ValueSpec* entityOrTag = make(ValueKind::EntityTypeOrTag);
            const ValueSpec* equipmentGroup = make(ValueKind::Enum, {"any", "mainhand", "offhand", "hand", "feet", "legs",
                                                                     "chest", "head", "armor", "body", "saddle"});
            const ValueSpec* useAnimation = make(ValueKind::Enum, {"none", "eat", "drink", "block", "bow", "spear",
                                                                   "crossbow", "spyglass", "toot_horn", "brush",
                                                                   "bundle", "trident"});
            const ValueSpec* consumeEffect = compound({
                {"type", make(ValueKind::Enum, {"apply_effects", "remove_effects", "clear_all_effects",
                                                "teleport_randomly", "play_sound"})},
                {"effects", list(mobEffect)}, {"probability", unit01},
                {"diameter", make(ValueKind::Number, {"16.0"})}, {"sound", sound}});
            const ValueSpec* blockPredicate = compound({{"blocks", blockOrTag}, {"state", compound({})},
                                                        {"nbt", compound({})}});
            const ValueSpec* kineticCondition = compound({{"max_duration_ticks", make(ValueKind::Number, {"100"})},
                                                          {"min_speed", make(ValueKind::Number, {"0.0"})},
                                                          {"min_relative_speed", make(ValueKind::Number, {"0.0"})}});
            const ValueSpec* swing = compound({{"type", make(ValueKind::Enum, {"none", "whack", "stab"})},
                                               {"duration", make(ValueKind::Number, {"6", "10"})}});
            const ValueSpec* signText = compound({{"messages", list(text)}, {"color", dye},
                                                  {"has_glowing_text", boolean}});
            specs = {
                {"banner_patterns", list(compound({{"pattern", make(ValueKind::BannerPattern)}, {"color", dye}}))},
                {"break_sound", make(ValueKind::Sound)},
                {"bucket_entity_data", compound({
                    {"NoAI", boolean}, {"Silent", boolean}, {"NoGravity", boolean}, {"Glowing", boolean},
                    {"Invulnerable", boolean}, {"PersistenceRequired", boolean},
                    {"Health", make(ValueKind::Number, {"10.0", "20.0"})}, {"Age", make(ValueKind::Number, {"0", "-24000"})},
                    {"AgeLocked", boolean}, {"HuntingCooldown", make(ValueKind::Number, {"0", "2400"})}})},
                {"charged_projectiles", list(stack)},
                {"container", list(compound({{"slot", make(ValueKind::Number, {"0", "1", "26"})}, {"item", stack}}))},
                {"custom_name", text},
                {"damage", make(ValueKind::Number, {"0", "10", "100"})},
                {"damage_resistant", compound({{"types", make(ValueKind::DamageTypeTag)}})},
                {"dyed_color", rgb},
                {"enchantable", compound({{"value", make(ValueKind::Number, {"1", "10", "15", "22"})}})},
                {"enchantments", make(ValueKind::EnchantmentMap)},
                {"firework_explosion", firework},
                {"fireworks", compound({{"flight_duration", make(ValueKind::Number, {"1", "2", "3"})}, {"explosions", list(firework)}})},
                {"instrument", make(ValueKind::Instrument)},
                {"item_name", text},
                {"map_color", rgb},
                {"map_decorations", make(ValueKind::Unit)},
                {"map_id", make(ValueKind::Number, {"0", "1"})},
                {"max_damage", make(ValueKind::Number, {"59", "250", "1561", "2031"})},
                {"ominous_bottle_amplifier", make(ValueKind::Number, {"0", "1", "2", "3", "4"})},
                {"pot_decorations", list(item)},
                {"potion_contents", compound({
                    {"potion", make(ValueKind::Potion)}, {"custom_color", rgb},
                    {"custom_effects", list(mobEffect)}, {"custom_name", make(ValueKind::Enum, {})}})},
                {"potion_duration_scale", make(ValueKind::Number, {"1.0", "0.25", "2.0"})},
                {"rarity", make(ValueKind::Enum, {"common", "uncommon", "rare", "epic"})},
                {"repair_cost", make(ValueKind::Number, {"0", "1", "3", "39"})},
                {"repairable", compound({{"items", make(ValueKind::ItemOrTag)}})},
                {"stored_enchantments", make(ValueKind::EnchantmentMap)},
                {"suspicious_stew_effects", list(compound({{"id", effect}, {"duration", ticks}}))},
                {"unbreakable", make(ValueKind::Unit)},
                {"weapon", compound({{"item_damage_per_attack", make(ValueKind::Number, {"0", "1", "2"})},
                                     {"disable_blocking_for_seconds", make(ValueKind::Number, {"0.0", "5.0"})}})},
                {"writable_book_content", compound({{"pages", list(make(ValueKind::Enum, {}))}})},
                {"written_book_content", compound({
                    {"title", make(ValueKind::Enum, {})}, {"author", make(ValueKind::Enum, {})},
                    {"generation", make(ValueKind::Number, {"0", "1", "2", "3"})},
                    {"pages", list(text)}, {"resolved", boolean}})},
                {"axolotl/variant", make(ValueKind::Axolotl)},
                {"painting/variant", make(ValueKind::Painting)},
                {"salmon/size", make(ValueKind::Enum, {"small", "medium", "large"})},
                {"tropical_fish/base_color", dye},
                {"tropical_fish/pattern", make(ValueKind::Enum, {"kob", "sunstreak", "snooper", "dasher", "brinely", "spotty",
                                                                 "flopper", "stripey", "glitter", "blockfish", "betty", "clayfish"})},
                {"tropical_fish/pattern_color", dye},

                // ── The rest of MC 26.3's persistent components ─────────────
                {"custom_data", compound({})},
                {"max_stack_size", make(ValueKind::Number, {"1", "16", "64", "99"})},
                {"use_effects", compound({{"can_sprint", boolean}, {"interact_vibrations", boolean},
                                          {"speed_multiplier", unit01}})},
                {"minimum_attack_charge", unit01},
                {"damage_type", make(ValueKind::DataFile, {"damage_type"})},
                {"item_model", item},
                {"lore", list(text)},
                {"can_place_on", blockPredicate},
                {"can_break", blockPredicate},
                {"attribute_modifiers", list(compound({
                    {"type", make(ValueKind::Attribute)},
                    {"id", make(ValueKind::Enum, {})},
                    {"amount", make(ValueKind::Number, {"1.0", "4.0", "0.5", "-0.25"})},
                    {"operation", make(ValueKind::Enum, {"add_value", "add_multiplied_base", "add_multiplied_total"})},
                    {"slot", equipmentGroup},
                    {"display", compound({{"type", make(ValueKind::Enum, {"default", "hidden", "override"})},
                                          {"value", text}})}}))},
                {"custom_model_data", compound({{"floats", list(make(ValueKind::Number, {"1.0", "2.0"}))},
                                                {"flags", list(boolean)}, {"strings", list(make(ValueKind::Enum, {}))},
                                                {"colors", list(rgb)}})},
                {"tooltip_display", compound({{"hide_tooltip", boolean},
                                              {"hidden_components", list(make(ValueKind::ComponentName))}})},
                {"enchantment_glint_override", boolean},
                {"intangible_projectile", make(ValueKind::Unit)},
                {"food", compound({{"nutrition", make(ValueKind::Number, {"1", "4", "8"})},
                                   {"saturation", make(ValueKind::Number, {"0.6", "2.4", "12.8"})},
                                   {"can_always_eat", boolean}})},
                {"consumable", compound({{"consume_seconds", make(ValueKind::Number, {"1.6", "0.8", "3.2"})},
                                         {"animation", useAnimation}, {"sound", sound},
                                         {"has_consume_particles", boolean},
                                         {"on_consume_effects", list(consumeEffect)}})},
                {"use_remainder", stack},
                {"use_cooldown", compound({{"seconds", make(ValueKind::Number, {"1.0", "5.0"})},
                                           {"cooldown_group", make(ValueKind::Enum, {})}})},
                {"tool", compound({{"rules", list(compound({{"blocks", blockOrTag},
                                                            {"speed", make(ValueKind::Number, {"2.0", "8.0", "15.0"})},
                                                            {"correct_for_drops", boolean}}))},
                                   {"default_mining_speed", make(ValueKind::Number, {"1.0"})},
                                   {"damage_per_block", make(ValueKind::Number, {"1", "2"})},
                                   {"can_destroy_blocks_in_creative", boolean}})},
                {"attack_range", compound({{"min_reach", reach}, {"max_reach", reach}, {"min_creative_reach", reach},
                                           {"max_creative_reach", reach},
                                           {"hitbox_margin", make(ValueKind::Number, {"0.3", "0.0"})},
                                           {"mob_factor", unit01}})},
                {"equippable", compound({
                    {"slot", make(ValueKind::Enum, {"head", "chest", "legs", "feet", "body", "mainhand", "offhand", "saddle"})},
                    {"equip_sound", sound},
                    {"asset_id", make(ValueKind::Enum, {"leather", "chainmail", "iron", "gold", "diamond", "netherite",
                                                        "copper", "turtle_scute", "armadillo_scute", "elytra"})},
                    {"camera_overlay", make(ValueKind::Enum, {"misc/pumpkinblur"})},
                    {"allowed_entities", entityOrTag},
                    {"dispensable", boolean}, {"swappable", boolean}, {"damage_on_hurt", boolean},
                    {"equip_on_interact", boolean}, {"can_be_sheared", boolean}, {"shearing_sound", sound}})},
                {"glider", make(ValueKind::Unit)},
                {"tooltip_style", make(ValueKind::Enum, {})},
                {"death_protection", compound({{"death_effects", list(consumeEffect)}})},
                {"blocks_attacks", compound({
                    {"block_delay_seconds", make(ValueKind::Number, {"0.0", "0.25"})},
                    {"disable_cooldown_scale", make(ValueKind::Number, {"1.0", "0.0"})},
                    {"damage_reductions", list(compound({{"type", damageTag}, {"base", make(ValueKind::Number, {"0.0"})},
                                                         {"factor", unit01},
                                                         {"horizontal_blocking_angle", make(ValueKind::Number, {"90.0"})}}))},
                    {"item_damage", compound({{"threshold", make(ValueKind::Number, {"3.0"})},
                                              {"base", make(ValueKind::Number, {"1.0"})}, {"factor", unit01}})},
                    {"bypassed_by", damageTag}, {"block_sound", sound}, {"disabled_sound", sound}})},
                {"piercing_weapon", compound({{"deals_knockback", boolean}, {"dismounts", boolean},
                                              {"sound", sound}, {"hit_sound", sound}})},
                {"kinetic_weapon", compound({
                    {"contact_cooldown_ticks", make(ValueKind::Number, {"10"})},
                    {"delay_ticks", make(ValueKind::Number, {"0", "10"})},
                    {"dismount_conditions", kineticCondition}, {"knockback_conditions", kineticCondition},
                    {"damage_conditions", kineticCondition},
                    {"forward_movement", make(ValueKind::Number, {"0.0", "0.38"})},
                    {"damage_multiplier", make(ValueKind::Number, {"1.0"})},
                    {"sound", sound}, {"hit_sound", sound}})},
                {"attack_animation", swing},
                {"interact_animation", swing},
                {"block_transformer", make(ValueKind::Enum, {"axe", "hoe", "shovel"})},
                {"villager_food", compound({{"nutrition", make(ValueKind::Number, {"1", "4"})}})},
                {"dye", dye},
                {"bundle_contents", list(stack)},
                {"trim", compound({{"material", make(ValueKind::DataFile, {"trim_material"})},
                                   {"pattern", make(ValueKind::DataFile, {"trim_pattern"})}})},
                {"debug_stick_state", compound({})},
                {"entity_data", compound({{"id", make(ValueKind::EntityType)}, {"NoAI", boolean}, {"Silent", boolean},
                                          {"Invulnerable", boolean}, {"CustomName", text}})},
                {"block_entity_data", compound({{"id", make(ValueKind::Enum, {"chest", "barrel", "furnace", "spawner",
                                                                             "beehive", "sign", "command_block"})}})},
                {"provides_trim_material", make(ValueKind::DataFile, {"trim_material"})},
                {"jukebox_playable", make(ValueKind::DataFile, {"jukebox_song"})},
                {"provides_banner_patterns", make(ValueKind::DataFile, {"banner_pattern", "tags/banner_pattern"})},
                {"recipes", list(make(ValueKind::DataFile, {"recipe"}))},
                {"lodestone_tracker", compound({{"target", compound({
                                                    {"dimension", make(ValueKind::Enum, {"overworld", "the_nether", "the_end"})},
                                                    {"pos", make(ValueKind::Number, {"[I;0,64,0]"})}})},
                                                {"tracked", boolean}})},
                {"profile", compound({{"name", make(ValueKind::Enum, {})},
                                      {"id", make(ValueKind::Number, {"[I;0,0,0,0]"})},
                                      {"properties", list(compound({{"name", make(ValueKind::Enum, {"textures"})},
                                                                    {"value", make(ValueKind::Enum, {})},
                                                                    {"signature", make(ValueKind::Enum, {})}}))}})},
                {"note_block_sound", sound},
                {"base_color", dye},
                {"block_state", compound({})},
                {"bees", list(compound({{"entity_data", compound({{"id", make(ValueKind::EntityType)}})},
                                        {"ticks_in_hive", make(ValueKind::Number, {"0"})},
                                        {"min_ticks_in_hive", make(ValueKind::Number, {"600", "2400"})}}))},
                {"sulfur_cube_content", stack},
                {"lock", compound({{"items", make(ValueKind::ItemOrTag)}, {"count", make(ValueKind::Number, {"1"})},
                                   {"components", compound({})}, {"predicates", compound({})}})},
                {"container_loot", compound({{"loot_table", make(ValueKind::DataFile, {"loot_table"})},
                                             {"seed", make(ValueKind::Number, {"0"})}})},
                {"compostable", compound({{"layers", make(ValueKind::Enum, {
                    "compostable/low", "compostable/low_medium", "compostable/medium", "compostable/medium_high",
                    "compostable/always_add_one"})}})},
                {"cooking_fuel", compound({{"burn_time", make(ValueKind::Enum, {
                                               "cooking/time_coal", "cooking/time_wood_blocks", "cooking/time_lava_bucket",
                                               "cooking/time_blaze_rod", "cooking/time_coal_block", "cooking/time_dry_plants"})},
                                           {"speed_multiplier", make(ValueKind::Enum, {
                                               "cooking/speed_default", "cooking/normal_speed_multiplier",
                                               "cooking/fast_speed_multiplier"})}})},
                {"brewing_fuel", compound({{"uses", make(ValueKind::Enum, {"brewing/uses_default"})},
                                           {"speed_multiplier", make(ValueKind::Enum, {"brewing/speed_default"})}})},
                {"mob_visibility", compound({{"targeting_entity_types", entityOrTag},
                                             {"visibility", unit01}})},
                {"provides_pottery_pattern", make(ValueKind::Enum, {
                    "angler", "archer", "arms_up", "blade", "brewer", "burn", "danger", "explorer", "flow", "friend",
                    "guster", "heart", "heartbreak", "howl", "miner", "mourner", "plenty", "prize", "scrape", "sheaf",
                    "shelter", "skull", "snort"})},
                {"sign_text_front", signText},
                {"sign_text_back", signText},
                {"waxed", make(ValueKind::Unit)},
            };
            // The entity variant components: their own registries' names.
            for (const char* name : {"villager/variant", "wolf/variant", "wolf/sound_variant", "wolf/collar",
                                     "fox/variant", "parrot/variant", "mooshroom/variant", "rabbit/variant",
                                     "pig/variant", "pig/sound_variant", "cow/variant", "cow/sound_variant",
                                     "chicken/variant", "chicken/sound_variant", "zombie_nautilus/variant",
                                     "frog/variant", "horse/variant", "llama/variant", "cat/variant",
                                     "cat/sound_variant", "cat/collar", "sheep/color", "shulker/color",
                                     "cushion/color"}) {
                specs.emplace(name, make(ValueKind::Variant, {name}));
            }
            auto it = specs.find(component);
            return it == specs.end() ? nullptr : it->second;
        }

        const std::vector<std::string>& DyeColorNames() {
            static const std::vector<std::string> names = {
                "black", "blue", "brown", "cyan", "gray", "green", "light_blue", "light_gray",
                "lime", "magenta", "orange", "pink", "purple", "red", "white", "yellow"};
            return names;
        }

        const std::vector<std::string>& PotionIds() {
            static std::vector<std::string> ids;
            if (ids.empty()) {
                for (int i = 0; i < Game::kPotionCount; ++i) {
                    if (const char* key = Game::GetPotionKey(static_cast<Game::PotionId>(i))) ids.push_back(Bare(key));
                }
                SortUnique(ids);
            }
            return ids;
        }

        // Quoted ids ("ashen" style), filtered MC's way on what was typed
        // (with or without its opening quote).
        std::vector<std::string> QuotedResources(const std::vector<std::string>& ids, const std::string& typed) {
            const bool quoted = !typed.empty() && (typed[0] == '"' || typed[0] == '\'');
            std::string body = quoted ? typed.substr(1) : typed;
            if (!body.empty() && (body.back() == '"' || body.back() == '\'')) body.pop_back();
            std::vector<std::string> out;
            for (const std::string& id : FilterResources(ids, body)) out.push_back("\"" + id + "\"");
            return out;
        }

        // The values for one spec at the position being typed.
        std::vector<std::string> SpecValues(const ValueSpec& spec, const std::string& typed) {
            switch (spec.kind) {
                case ValueKind::Text: {
                    std::vector<std::string> out = {"\"Name\"", "{text:\"Name\",color:\"gold\"}",
                                                    "{text:\"Name\",italic:false}"};
                    if (!typed.empty() && typed != "\"" && typed != "{") out.clear();
                    return out;
                }
                case ValueKind::Sound:         return QuotedResources(SoundIds(), typed);
                case ValueKind::Item:          return QuotedResources(ItemIds(), typed);
                case ValueKind::ItemOrTag: {
                    std::vector<std::string> out = QuotedResources(ItemIds(), typed);
                    const auto tags = QuotedResources(DataFileIds("tags/item", "#"), typed);
                    out.insert(out.end(), tags.begin(), tags.end());
                    return out;
                }
                case ValueKind::Effect:        return QuotedResources(EffectIds(), typed);
                case ValueKind::Potion:        return QuotedResources(PotionIds(), typed);
                case ValueKind::EntityType:    return QuotedResources(EntitySlugs(false), typed);
                case ValueKind::Block:         return QuotedResources(BlockIds(), typed);
                case ValueKind::Enchantment:   return QuotedResources(EnchantmentIds(), typed);
                case ValueKind::Instrument:    return QuotedResources(DataFileIds("instrument"), typed);
                case ValueKind::BannerPattern: return QuotedResources(DataFileIds("banner_pattern"), typed);
                case ValueKind::Painting:      return QuotedResources(DataFileIds("painting_variant"), typed);
                case ValueKind::DyeColor:      return QuotedResources(DyeColorNames(), typed);
                case ValueKind::DamageTypeTag: return QuotedResources(DataFileIds("tags/damage_type", "#"), typed);
                case ValueKind::Axolotl:
                    return QuotedResources({"blue", "cyan", "gold", "lucy", "wild"}, typed);
                case ValueKind::Bool:          return FilterPrefix({"false", "true"}, typed);
                case ValueKind::Unit:          return FilterPrefix({"{}"}, typed);
                case ValueKind::Number:        return FilterPrefix(spec.values, typed);
                case ValueKind::Enum:          return spec.values.empty() ? std::vector<std::string>{}
                                                                          : QuotedResources(spec.values, typed);
                case ValueKind::Attribute: {
                    static std::vector<std::string> ids;
                    if (ids.empty()) {
                        for (int i = 0; i < static_cast<int>(Game::Attribute::Count); ++i) {
                            ids.emplace_back(Game::AttributeName(static_cast<Game::Attribute>(i)));
                        }
                        SortUnique(ids);
                    }
                    return QuotedResources(ids, typed);
                }
                case ValueKind::BlockOrTag: {
                    std::vector<std::string> out = QuotedResources(BlockIds(), typed);
                    const auto tags = QuotedResources(DataFileIds("tags/block", "#"), typed);
                    out.insert(out.end(), tags.begin(), tags.end());
                    return out;
                }
                case ValueKind::EntityTypeOrTag: {
                    std::vector<std::string> out = QuotedResources(EntitySlugs(false), typed);
                    const auto tags = QuotedResources(DataFileIds("tags/entity_type", "#"), typed);
                    out.insert(out.end(), tags.begin(), tags.end());
                    return out;
                }
                case ValueKind::DataFile: {
                    if (spec.values.empty()) return {};
                    std::vector<std::string> out = QuotedResources(DataFileIds(spec.values[0]), typed);
                    if (spec.values.size() > 1) {
                        const auto tags = QuotedResources(DataFileIds(spec.values[1], "#"), typed);
                        out.insert(out.end(), tags.begin(), tags.end());
                    }
                    return out;
                }
                case ValueKind::Variant: {
                    if (spec.values.empty()) return {};
                    const Game::DataComponentTypeBase* type = Game::DataComponents::ByName(spec.values[0]);
                    const auto* info = type ? Game::EntityVariantComponents::Find(*type) : nullptr;
                    if (!info) return {};
                    if (info->kind == Game::EntityVariantComponents::Kind::Dye) return QuotedResources(DyeColorNames(), typed);
                    std::vector<std::string> names;
                    for (int i = 0; i < info->count; ++i) {
                        if (info->names && info->names[i] && *info->names[i]) names.emplace_back(info->names[i]);
                    }
                    SortUnique(names);
                    return QuotedResources(names, typed);
                }
                case ValueKind::ComponentName: {
                    static std::vector<std::string> names;
                    if (names.empty()) {
                        for (const auto& c : Game::ItemComponentHints::kComponents) names.emplace_back(c.name);
                        SortUnique(names);
                    }
                    return QuotedResources(names, typed);
                }
                case ValueKind::Compound:
                case ValueKind::EnchantmentMap: return FilterPrefix({"{"}, typed);
                case ValueKind::List:           return FilterPrefix({"["}, typed);
            }
            return {};
        }

        // Complete inside one component's SNBT value (`text` starts right
        // after the `=`): walk the nesting to the innermost compound or list,
        // follow the spec down the same path, and offer that position's keys
        // or values. `offset` returns where the entries replace from.
        std::vector<std::string> ComponentValueCompletion(const ValueSpec& root, const std::string& text, size_t& offset) {
            struct Frame {
                bool compound = true;
                const ValueSpec* spec = nullptr;   // the container's own spec
                bool inValue = false;              // compound: after the ':'
                std::string key;
                size_t slot = 0;                   // where the current key/value starts
                bool enchantMap = false;
            };
            std::vector<Frame> frames;
            char quote = 0;
            const auto childSpec = [&](const Frame& f) -> const ValueSpec* {
                if (!f.spec) return nullptr;
                if (!f.compound) return f.spec->element;
                if (f.enchantMap) return nullptr;
                for (const auto& [name, spec] : f.spec->fields) if (name == f.key) return spec;
                return nullptr;
            };
            const auto currentSpec = [&]() -> const ValueSpec* {
                return frames.empty() ? &root : childSpec(frames.back());
            };
            for (size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (quote) {
                    if (c == '\\') { ++i; continue; }
                    if (c == quote) quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'') { quote = c; continue; }
                if (c == '{' || c == '[') {
                    const ValueSpec* spec = currentSpec();
                    Frame f;
                    f.compound = c == '{';
                    f.spec = spec;
                    f.enchantMap = spec && spec->kind == ValueKind::EnchantmentMap;
                    f.slot = i + 1;
                    // A typed array ([I;…]) is a value, not a list of specs.
                    if (!f.compound && i + 2 < text.size() && text[i + 2] == ';') f.spec = nullptr;
                    frames.push_back(std::move(f));
                    continue;
                }
                if (c == '}' || c == ']') {
                    if (!frames.empty()) frames.pop_back();
                    continue;
                }
                if (frames.empty()) continue;
                Frame& f = frames.back();
                if (f.compound && c == ':' && !f.inValue) {
                    f.key = text.substr(f.slot, i - f.slot);
                    f.key.erase(0, f.key.find_first_not_of(' '));
                    if (!f.key.empty() && (f.key.front() == '"' || f.key.front() == '\'')) f.key = f.key.substr(1, f.key.size() - 2);
                    f.inValue = true;
                    f.slot = i + 1;
                } else if (c == ',') {
                    f.inValue = false;
                    f.slot = i + 1;
                }
            }
            if (frames.empty()) {
                offset = 0;
                return SpecValues(root, text);
            }
            Frame& f = frames.back();
            size_t slot = f.slot;
            while (slot < text.size() && text[slot] == ' ') ++slot;
            offset = slot;
            const std::string typed = text.substr(slot);
            if (f.compound && !f.inValue) {
                // Keys: the spec's fields, or (an enchantment map) every
                // enchantment — each once.
                std::vector<std::string> keys;
                if (f.enchantMap) {
                    for (const std::string& e : EnchantmentIds()) keys.push_back(e + ":");
                } else if (f.spec) {
                    for (const auto& [name, spec] : f.spec->fields) { (void)spec; keys.push_back(name + ":"); }
                }
                std::vector<std::string> out = FilterResources(keys, typed);
                if (typed.empty()) out.push_back("}");
                return out;
            }
            if (f.compound && f.enchantMap) {
                // A level, 1 up to the enchantment's max.
                std::vector<std::string> levels;
                const auto id = Game::EnchantmentRegistry::ByName(Bare(f.key));
                const int max = id ? Game::EnchantmentRegistry::Get(*id).maxLevel : 5;
                for (int l = 1; l <= std::max(1, max); ++l) levels.push_back(std::to_string(l));
                return FilterPrefix(levels, typed);
            }
            const ValueSpec* spec = childSpec(f);
            if (!spec) return {};
            std::vector<std::string> values = SpecValues(*spec, typed);
            if (!f.compound && typed.empty()) values.push_back("]");
            return values;
        }

        // ItemArgument: `id[component=value,...]` — ids, then (after `[`)
        // the components this engine applies (ItemComponentHints) and their
        // example values.
        Provided ItemWithComponents(const Token& partial) {
            Provided out;
            out.anchor = partial.start;
            const std::string& word = partial.text;
            const size_t bracket = word.find('[');
            if (bracket == std::string::npos) {
                out.items = FilterResources(ItemIds(), word);
                if (!word.empty() && std::binary_search(ItemIds().begin(), ItemIds().end(), Bare(Lower(word)))) {
                    out.items.insert(out.items.begin() + std::min<size_t>(1, out.items.size()), word + "[");
                }
                return out;
            }
            // Where the cursor is inside the brackets: a key or a value.
            int depth = 0;
            char quote = 0;
            bool inValue = false;
            size_t slot = bracket + 1, keyStart = bracket + 1;
            std::string key;
            for (size_t i = bracket + 1; i < word.size(); ++i) {
                const char c = word[i];
                if (quote) {
                    if (c == '\\') { ++i; continue; }
                    if (c == quote) quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'') { quote = c; continue; }
                if (c == '{' || c == '[') { ++depth; continue; }
                if (c == '}' || c == ']') {
                    if (depth == 0) return out;   // closed: nothing to add
                    --depth;
                    continue;
                }
                if (depth != 0) continue;
                if (c == '=' && !inValue) {
                    key = Lower(word.substr(keyStart, i - keyStart));
                    key.erase(0, key.find_first_not_of(' '));
                    if (key.rfind("minecraft:", 0) == 0) key.erase(0, 10);
                    inValue = true;
                    slot = i + 1;
                } else if (c == ',') {
                    inValue = false;
                    slot = keyStart = i + 1;
                }
            }
            while (slot < word.size() && word[slot] == ' ') ++slot;
            const std::string typed = word.substr(slot);
            out.anchor = partial.start + static_cast<int>(slot);
            if (!inValue) {
                // `name=` to set, `!name` to take a default away (any
                // registered component the item may carry by default).
                static const char* const kRemovable[] = {
                    "food", "consumable", "tool", "equippable", "blocks_attacks", "use_remainder",
                    "lore", "enchantment_glint_override", "bundle_contents", "painting_variant"};
                const std::string inside = word.substr(bracket + 1);
                const auto named = [&](const std::string& name) {
                    return inside.find(name + "=") != std::string::npos || inside.find("!" + name) != std::string::npos;
                };
                std::vector<std::string> sets, removes;
                for (const auto& c : Game::ItemComponentHints::kComponents) {
                    const std::string name(c.name);
                    if (named(name)) continue;
                    sets.push_back(name + "=");
                    if (Game::DataComponents::ByName(name)) removes.push_back("!" + name);
                }
                for (const char* name : kRemovable) {
                    if (!named(name) && Game::DataComponents::ByName(name)) removes.push_back(std::string("!") + name);
                }
                std::sort(removes.begin(), removes.end());
                // A whole `!name` typed: close or continue.
                if (!typed.empty() && typed[0] == '!' && Game::DataComponents::ByName(typed.substr(1))) {
                    out.items = {typed + ",", typed + "]"};
                    return out;
                }
                out.items = FilterPrefix(sets, typed);
                const auto rem = FilterPrefix(removes, typed);
                out.items.insert(out.items.end(), rem.begin(), rem.end());
                if (typed.empty()) out.items.push_back("]");
                return out;
            }
            // The value: from the component's own registries (ComponentSpec).
            const ValueSpec* spec = ComponentSpec(key);
            if (!spec) return out;
            size_t offset = 0;
            std::vector<std::string> values = ComponentValueCompletion(*spec, typed, offset);
            out.anchor = partial.start + static_cast<int>(slot + offset);
            // A whole value typed at the top level: close or continue.
            if (offset == 0 && !typed.empty()) {
                for (const std::string& v : values) {
                    if (v == typed) { values = {typed + ",", typed + "]"}; break; }
                }
            }
            out.items = std::move(values);
            return out;
        }

        std::vector<std::string> Quoted(const std::vector<std::string>& ids) {
            std::vector<std::string> out;
            out.reserve(ids.size());
            for (const std::string& id : ids) out.push_back("\"" + id + "\"");
            return out;
        }

        // ParticleArgument: the option record each shape takes, keyed as
        // ParticleCommand's SNBT reader reads it, with a working example.
        std::vector<NbtKey> ParticleKeys(Game::ParticleTypes::OptionsShape shape) {
            using S = Game::ParticleTypes::OptionsShape;
            switch (shape) {
                case S::BlockState:     return {{"block_state", Quoted(BlockIds())}};
                case S::Item:           return {{"item", Quoted(ItemIds())}};
                case S::Dust:           return {{"color", {"[1.0,0.0,0.0]", "[0.0,1.0,0.0]", "[0.0,0.0,1.0]", "16711680"}},
                                                {"scale", {"1.0", "0.5", "2.0"}}};
                case S::DustTransition: return {{"from_color", {"[1.0,0.0,0.0]"}}, {"scale", {"1.0"}},
                                                {"to_color", {"[0.0,0.0,1.0]"}}};
                case S::Color:          return {{"color", {"[1.0,1.0,1.0,1.0]", "-1"}}};
                case S::Spell:          return {{"color", {"[1.0,1.0,1.0]"}}, {"power", {"1.0"}}};
                case S::Power:          return {{"power", {"1.0"}}};
                case S::Vibration:      return {{"arrival_in_ticks", {"20"}},
                                                {"destination", {"{type:\"block\",pos:[0,64,0]}",
                                                                 "{type:\"entity\",source_entity:0,y_offset:0.0}"}}};
                case S::Trail:          return {{"color", {"[1.0,0.0,0.0]"}}, {"duration", {"20"}},
                                                {"target", {"[0.0,64.0,0.0]"}}};
                case S::Shriek:         return {{"delay", {"0"}}};
                case S::SculkCharge:    return {{"roll", {"0.0"}}};
                case S::Geyser:         return {{"water_blocks", {"3"}}};
                case S::GeyserBase:     return {{"burst_impulse_base", {"1.0"}}, {"water_blocks", {"3"}}};
                case S::Simple:
                case S::FireworkStarter:
                    break;
            }
            return {};
        }

        // The whole option block with each key's first example — what a
        // bare `dust` expands to.
        std::string ParticleTemplate(Game::ParticleTypes::OptionsShape shape) {
            using S = Game::ParticleTypes::OptionsShape;
            switch (shape) {
                case S::BlockState:     return "{block_state:\"stone\"}";
                case S::Item:           return "{item:\"diamond\"}";
                case S::Dust:           return "{color:[1.0,0.0,0.0],scale:1.0}";
                case S::DustTransition: return "{from_color:[1.0,0.0,0.0],to_color:[0.0,0.0,1.0],scale:1.0}";
                case S::Color:          return "{color:[1.0,1.0,1.0,1.0]}";
                case S::Spell:          return "{color:[1.0,1.0,1.0],power:1.0}";
                case S::Power:          return "{power:1.0}";
                case S::Vibration:      return "{destination:{type:\"block\",pos:[0,64,0]},arrival_in_ticks:20}";
                case S::Trail:          return "{target:[0.0,64.0,0.0],color:[1.0,0.0,0.0],duration:20}";
                case S::Shriek:         return "{delay:0}";
                case S::SculkCharge:    return "{roll:0.0}";
                case S::Geyser:         return "{water_blocks:3}";
                case S::GeyserBase:     return "{water_blocks:3,burst_impulse_base:1.0}";
                case S::Simple:
                case S::FireworkStarter:
                    break;
            }
            return {};
        }

        Provided Particle(const Token& partial) {
            Provided out;
            out.anchor = partial.start;
            const std::string& word = partial.text;
            const size_t brace = word.find('{');
            if (brace == std::string::npos) {
                out.items = FilterResources(ParticleIds(), word);
                // A whole id typed that takes options: its option block.
                if (const auto kind = Game::ParticleTypes::FromName(word)) {
                    const std::string tmpl = ParticleTemplate(Game::ParticleTypes::Get(*kind).shape);
                    if (!tmpl.empty()) out.items.insert(out.items.begin(), word + tmpl);
                }
                return out;
            }
            const auto kind = Game::ParticleTypes::FromName(word.substr(0, brace));
            if (!kind) return out;
            std::vector<NbtKey> keys = ParticleKeys(Game::ParticleTypes::Get(*kind).shape);
            Token options;
            options.start = partial.start + static_cast<int>(brace);
            options.text = word.substr(brace);
            return CompoundCompletion(options, keys);
        }

        Provided EntityNbt(const Token& partial, const std::vector<Step>& path) {
            std::vector<NbtKey> keys;
            Game::EntityTypeId type{};
            const std::string* entity = ValueOf(path, "entity");
            if (entity && EntityTypeFromText(*entity, type)) {
                for (const Game::EntityNbtHints::Key& k : Game::EntityNbtHints::KeysFor(type)) {
                    keys.push_back({k.name, k.values});
                }
            }
            return CompoundCompletion(partial, keys);
        }

        // Coordinates: "~ ~ ~" for what is left of the group (MC
        // suggestCoordinates' default), "^ ^ ^" once a '^' is typed.
        std::vector<std::string> Coordinates(Arg type, int component, const std::string& typed) {
            const int remaining = Game::Cmd::TokenArity(type) - component;
            if (remaining <= 0) return {};
            const bool local = type == Arg::Vec3 || type == Arg::BlockPos || type == Arg::Vec2;
            auto repeat = [&](const char* c) {
                std::string s;
                for (int i = 0; i < remaining; ++i) s += (i ? " " : "") + std::string(c);
                return s;
            };
            if (typed.empty()) {
                std::vector<std::string> out = {repeat("~")};
                if (local && component == 0) out.push_back(repeat("^"));
                return out;
            }
            if (typed == "~") return {repeat("~")};
            if (typed == "^" && local) return {repeat("^")};
            return {};
        }

        // TimeArgument.listSuggestions: a typed number with each unit.
        std::vector<std::string> TimeValues(const Node& node, const std::string& typed) {
            size_t n = 0;
            while (n < typed.size() && (std::isdigit(static_cast<unsigned char>(typed[n])) || typed[n] == '.' || typed[n] == '-')) ++n;
            if (n > 0 && n == typed.size()) return {typed + "d", typed + "s", typed + "t"};
            return FilterPrefix(node.suggestions, typed);
        }

        // `/data modify … compute … float|integer <provider>` (MC
        // ResourceOrIdArgument over the context number providers): the
        // built-in registry ids, or an inline `{type:"…", …}` whose keys and
        // `type` values complete from the provider types' codecs.
        Provided NumberProviderCompletion(const Token& partial, bool isInt) {
            namespace N = Game::ContextNumberProviderNames;
            Provided out;
            out.anchor = partial.start;
            const std::string& word = partial.text;
            if (word.empty() || word[0] != '{') {
                std::vector<std::string> ids;
                if (isInt) for (const auto id : N::kIntIds) ids.emplace_back(id);
                else       for (const auto id : N::kFloatIds) ids.emplace_back(id);
                out.items = FilterResources(ids, word);
                if (word.empty()) out.items.push_back("{type:");
                return out;
            }
            // Which type the compound names (its top-level `type:` value).
            std::string type;
            const size_t t = word.find("type:");
            if (t != std::string::npos) {
                size_t v = t + 5;
                while (v < word.size() && (word[v] == '"' || word[v] == '\'' || word[v] == ' ')) ++v;
                size_t e = v;
                while (e < word.size() && (std::isalnum(static_cast<unsigned char>(word[e])) || word[e] == '_' || word[e] == ':')) ++e;
                type = Bare(word.substr(v, e - v));
            }
            std::vector<NbtKey> keys;
            std::vector<std::string> typeNames;
            const auto addType = [&](const N::TypeFields& tf) {
                typeNames.push_back("\"" + std::string(tf.type) + "\"");
                if (tf.type != type) return;
                for (const auto field : tf.fields) {
                    if (field.empty()) continue;
                    std::vector<std::string> values;
                    if (field == "condition") {
                        values = {"\"block/fast_cooking\"", "{condition:\"random_chance\",chance:0.5}"};
                    } else if (field == "storage") {
                        values = {"\"minecraft:\""};
                    } else if (field == "path") {
                        values = {"\"\""};
                    } else if (field == "inputs") {
                        values = {"[1,2]"};
                    } else if (field == "distribution") {
                        values = {"[{data:1,weight:1}]"};
                    } else if (field == "cases") {
                        values = {"[{condition:\"block/fast_cooking\",value:1}]"};
                    } else if (field == "amount") {
                        values = {"1", "{type:\"linear\",base:1,per_level_above_first:1}"};
                    } else if (field == "target") {
                        values = {"\"this\""};
                    } else if (field == "score") {
                        values = {"\"\""};
                    } else {
                        values = {"0", "1", "{type:"};
                    }
                    keys.push_back({std::string(field), std::move(values)});
                }
            };
            if (isInt) for (const auto& tf : N::kIntTypes) addType(tf);
            else       for (const auto& tf : N::kFloatTypes) addType(tf);
            keys.insert(keys.begin(), NbtKey{"type", typeNames});
            return CompoundCompletion(partial, keys);
        }

        // /data's NBT paths: the top-level keys of the target (or source)
        // kind — a player's file, a mob type's own keys (from a `type=`), the
        // block-entity keys — so the first step completes. Deeper steps are
        // typed (the server's live data is not on the client).
        std::vector<std::string> NbtPathKeys(const Node& node, const std::vector<Step>& path, const std::string& typed) {
            if (typed.find_first_of(".[{") != std::string::npos) return {};
            const bool source = node.name.rfind("source", 0) == 0;
            const char* who = source ? "source" : "target";
            const std::string* entity = nullptr;
            bool block = false;
            for (auto it = path.rbegin(); it != path.rend(); ++it) {
                if (!it->node || it->node->IsLiteral()) continue;
                if (it->node->name == who) { entity = it->node->type == Arg::Entity ? &it->value : nullptr; break; }
                if (it->node->name == std::string(who) + "Pos") { block = true; break; }
            }
            std::vector<std::string> keys;
            if (block) {
                keys = {"Book", "BurnTime", "CookTime", "CookTimeTotal", "CustomName", "Delay", "Items", "Lock",
                        "LootTable", "LootTableSeed", "MaxNearbyEntities", "MaxSpawnDelay", "MinSpawnDelay",
                        "Page", "RecordItem", "RequiredPlayerRange", "SpawnCount", "SpawnData", "SpawnPotentials",
                        "SpawnRange", "back_text", "bees", "components", "flower_pos", "front_text", "id",
                        "is_waxed", "item", "patterns", "sherds", "ticks_since_song_started", "x", "y", "z"};
            } else if (entity) {
                const std::string& sel = *entity;
                const bool player = sel.empty() || sel[0] != '@' || sel.rfind("@s", 0) == 0 || sel.rfind("@p", 0) == 0 ||
                                    sel.rfind("@a", 0) == 0 || sel.rfind("@r", 0) == 0;
                keys = {"Air", "CustomName", "CustomNameVisible", "Fire", "Glowing", "Invulnerable", "Motion",
                        "NoGravity", "OnGround", "PortalCooldown", "Pos", "Rotation", "Silent", "UUID", "fall_distance"};
                Game::EntityTypeId type{};
                const size_t typePos = sel.find("type=");
                if (typePos != std::string::npos) {
                    std::string slug = sel.substr(typePos + 5);
                    slug = slug.substr(0, slug.find_first_of(",]"));
                    if (EntityTypeFromText(slug, type)) {
                        for (const auto& k : Game::EntityNbtHints::KeysFor(type)) keys.push_back(k.name);
                    }
                } else if (player) {
                    for (const char* k : {"Health", "Inventory", "SelectedItem", "SelectedItem.components",
                                          "SelectedItem.count", "SelectedItem.id", "SelectedItemSlot", "XpLevel",
                                          "XpP", "XpTotal", "abilities", "active_effects", "equipment", "foodLevel",
                                          "foodSaturationLevel", "playerGameType", "Dimension"}) {
                        keys.emplace_back(k);
                    }
                } else {
                    for (const char* k : {"Age", "Health", "NoAI", "PersistenceRequired", "active_effects",
                                          "attributes", "equipment", "Item"}) {
                        keys.emplace_back(k);
                    }
                }
            }
            SortUnique(keys);
            return FilterPrefix(keys, typed);
        }

        // ComponentArgument (SNBT text components): how a component starts,
        // then the component keys and their values inside a '{…}' (the
        // ComponentSerialization codec's field names; colours by name).
        Provided TextComponentCompletion(const Token& partial) {
            const std::string& word = partial.text;
            Provided out;
            out.anchor = partial.start;
            if (word.empty()) {
                out.items = {"{text:\"", "\"", "[", "{translate:\"", "{selector:\"", "{keybind:\""};
                return out;
            }
            if (word[0] != '{') return out;
            std::vector<std::string> colors;
            for (int i = 0; i < Game::Text::kFormattingColorCount; ++i) {
                if (const char* name = Game::Text::FormattingName(i)) colors.emplace_back(name);
            }
            colors.emplace_back("\"#");
            const std::vector<std::string> flags = {"true", "false"};
            const std::vector<NbtKey> keys = {
                {"text", {"\""}},
                {"color", colors},
                {"bold", flags},
                {"italic", flags},
                {"underlined", flags},
                {"strikethrough", flags},
                {"obfuscated", flags},
                {"translate", {"\""}},
                {"fallback", {"\""}},
                {"with", {"["}},
                {"selector", {"\"@p\"", "\"@a\"", "\"@s\"", "\"@e\""}},
                {"separator", {"\"", "{"}},
                {"keybind", {"\"key.jump\"", "\"key.attack\"", "\"key.use\"", "\"key.inventory\""}},
                {"score", {"{name:"}},
                {"nbt", {"\""}},
                {"entity", {"\"@s\"", "\"@p\""}},
                {"block", {"\"~ ~ ~\""}},
                {"storage", {"\""}},
                {"interpret", flags},
                {"plain", flags},
                {"extra", {"["}},
                {"click_event", {"{action:"}},
                {"hover_event", {"{action:show_text,value:"}},
                {"insertion", {"\""}},
                {"font", {"\"minecraft:default\"", "\"minecraft:uniform\"", "\"minecraft:alt\""}},
                {"shadow_color", {"0"}},
                {"action", {"run_command", "suggest_command", "copy_to_clipboard", "open_url", "show_text"}},
                {"command", {"\"/"}},
                {"value", {"\""}},
            };
            return CompoundCompletion(partial, keys);
        }

        Provided Provide(const Candidate& c, const Token& partial) {
            const Node& node = *c.node;
            const std::string& typed = partial.text;
            Provided out;
            out.anchor = partial.start;
            if (node.IsLiteral()) {
                if (StartsWithIgnoreCase(node.name, typed)) out.items.push_back(node.name);
                return out;
            }
            // Fixed entries first (number examples, extra words) — for the
            // argument's first token only ("0 0 0" is a whole delta).
            std::vector<std::string> fixed;
            const bool greedy = node.type == Arg::Greedy || node.type == Arg::Message || node.type == Arg::TextComponent;
            if (c.component == 0 || greedy) fixed = FilterPrefix(node.suggestions, typed);
            switch (node.type) {
                case Arg::Word:
                case Arg::Integer:
                case Arg::Float:
                case Arg::Greedy:
                case Arg::Nbt:
                    out.items = std::move(fixed);
                    return out;
                case Arg::Bool:
                    out.items = FilterPrefix({"false", "true"}, typed);
                    break;
                case Arg::Player:
                case Arg::Players:
                case Arg::Entity:
                case Arg::Entities: {
                    out = Entities(node.type, partial);
                    // "/tp Alice <Tab>": a name already typed for an earlier
                    // player/entity argument is not offered again, so Tab
                    // lands on the other player (the old chat completion's
                    // rule).
                    for (const Step& s : c.path) {
                        const Arg t = s.node->type;
                        if (s.node->IsLiteral() || s.value.empty() || s.value[0] == '@') continue;
                        if (t != Arg::Player && t != Arg::Players && t != Arg::Entity && t != Arg::Entities) continue;
                        const std::string typedName = Lower(s.value);
                        out.items.erase(std::remove_if(out.items.begin(), out.items.end(),
                                                       [&](const std::string& n) { return Lower(n) == typedName; }),
                                        out.items.end());
                    }
                    break;
                }
                case Arg::PlayerName:
                    out.items = FilterPrefix(PlayerNames(), typed);
                    break;
                case Arg::EntityType:
                    out.items = FilterResources(EntitySlugs(true), typed);
                    break;
                case Arg::Block:
                case Arg::BlockPredicate: {
                    int offset = 0;
                    out.items = BlockElement(typed, node.type == Arg::BlockPredicate, false, offset);
                    out.anchor = partial.start + offset;
                    break;
                }
                case Arg::BlockList:
                    out = ListElement(partial, [](const std::string& element, int& offset) {
                        return BlockElement(element, true, true, offset);
                    });
                    break;
                case Arg::Item:
                    out = ItemWithComponents(partial);
                    break;
                case Arg::ItemList:
                    out = ListElement(partial, [](const std::string& element, int& offset) {
                        offset = 0;
                        std::vector<std::string> items = FilterResources(ItemIds(), element);
                        if (std::binary_search(ItemIds().begin(), ItemIds().end(), Bare(Lower(element)))) {
                            items.push_back(element + ",");
                        }
                        return items;
                    });
                    break;
                case Arg::Effect:        out.items = FilterResources(EffectIds(), typed); break;
                case Arg::Enchantment:   out.items = FilterResources(EnchantmentIds(), typed); break;
                case Arg::Sound:         out.items = FilterResources(SoundIds(), typed); break;
                case Arg::Particle:      out = Particle(partial); break;
                case Arg::GameRule:      out.items = FilterResources(GameRuleIds(), typed); break;
                case Arg::GameRuleValue: {
                    const std::string* rule = ValueOf(c.path, "rule");
                    const Game::Rules::Def* def = rule ? Game::Rules::Find(*rule) : nullptr;
                    if (def && def->type == Game::Rules::Type::Bool) {
                        out.items = FilterPrefix({"false", "true"}, typed);
                    } else if (def) {
                        std::vector<std::string> values = {std::to_string(def->defaultValue)};
                        if (def->minValue != def->defaultValue && def->minValue > -1000000) values.push_back(std::to_string(def->minValue));
                        out.items = FilterPrefix(values, typed);
                    }
                    break;
                }
                case Arg::Dimension:     out.items = FilterPrefix(DimensionIds(), typed); break;
                case Arg::Biome:         out.items = FilterResources(BiomeIds(), typed); break;
                case Arg::BiomeOrTag: {
                    if (!typed.empty() && typed[0] == '#') {
                        out.items = FilterNames(DataFileIds("tags/worldgen/biome", "#"), typed);
                    } else {
                        out.items = FilterResources(BiomeIds(), typed);
                        if (typed.empty()) {
                            const auto top = TopLevel(DataFileIds("tags/worldgen/biome", "#"));
                            out.items.insert(out.items.end(), top.begin(), top.end());
                        }
                    }
                    break;
                }
                case Arg::LocateStructure:
                case Arg::LocateBiome: {
                    const bool biome = node.type == Arg::LocateBiome;
                    const bool tag = !typed.empty() && typed[0] == '#';
                    if (s_dimension.received) {
                        out.items = FilterNames(tag ? (biome ? s_dimension.biomeTags : s_dimension.structureTags)
                                                    : (biome ? s_dimension.biomes : s_dimension.structures),
                                                typed);
                    } else {
                        const std::string dir = biome ? "biome" : "structure";
                        if (tag) {
                            out.items = FilterNames(DataFileIds("tags/worldgen/" + dir, "#"), typed);
                        } else {
                            out.items = FilterResources(biome ? BiomeIds() : DataFileIds("worldgen/structure"), typed);
                        }
                    }
                    break;
                }
                case Arg::LocatePoi: {
                    // The portal kinds that can exist where the player stands:
                    // nether portals in the Overworld and the Nether, hush
                    // portals in the Overworld and the Hush.
                    const Game::DimensionId dim = Client::ClientLevels::ActiveDimension();
                    std::vector<std::string> portals;
                    if (dim == Game::DimensionId::Overworld || dim == Game::DimensionId::Hush) portals.push_back("hush_portal");
                    if (dim == Game::DimensionId::Overworld || dim == Game::DimensionId::Nether) portals.push_back("nether_portal");
                    out.items = FilterNames(portals, typed);
                    break;
                }
                case Arg::LootTable:     out.items = FilterResources(DataFileIds("loot_table"), typed); break;
                case Arg::Advancement: {
                    // The advancements the server loads (the recipe-unlock
                    // ones are skipped there, so not offered here either).
                    static const std::vector<std::string> ids = [] {
                        std::vector<std::string> v;
                        for (const std::string& id : Game::Advancements::SuggestionRegistry().Ids()) v.push_back(Bare(id));
                        SortUnique(v);
                        return v;
                    }();
                    out.items = FilterResources(ids, typed);
                    break;
                }
                case Arg::AdvancementCriterion: {
                    // MC AdvancementCommands: the criteria of the advancement
                    // named before it.
                    const std::string* advancement = ValueOf(c.path, "advancement");
                    const Game::Advancements::Definition* def =
                        advancement ? Game::Advancements::SuggestionRegistry().Get(*advancement) : nullptr;
                    if (def) {
                        std::vector<std::string> names;
                        for (const auto& criterion : def->criteria) names.push_back(criterion.name);
                        SortUnique(names);
                        out.items = FilterPrefix(names, typed);
                    }
                    break;
                }
                case Arg::GameMode: {
                    // The four names; a short form or number completes to its name.
                    static const std::vector<std::pair<const char*, const char*>> aliases = {
                        {"0", "survival"}, {"1", "creative"}, {"2", "adventure"}, {"3", "spectator"}, {"sp", "spectator"}};
                    const std::string t = Lower(typed);
                    for (const auto& [alias, name] : aliases) {
                        if (t == alias) { out.items = {name}; return out; }
                    }
                    out.items = FilterPrefix({"adventure", "creative", "spectator", "survival"}, typed);
                    break;
                }
                case Arg::Vec3:
                case Arg::BlockPos:
                case Arg::Vec2:
                case Arg::ColumnPos:
                case Arg::Rotation:
                    out.items = Coordinates(node.type, c.component, typed);
                    break;
                case Arg::Time:          out.items = TimeValues(node, typed); return out;
                case Arg::EntityNbt:     out = EntityNbt(partial, c.path); break;
                case Arg::SoundSource: {
                    std::vector<std::string> names;
                    for (int i = 0; i < Game::kSoundSourceCount; ++i) {
                        names.emplace_back(Game::SoundSourceName(static_cast<Game::SoundSource>(i)));
                    }
                    out.items = FilterPrefix(names, typed);
                    break;
                }
                case Arg::NbtPath:       out.items = NbtPathKeys(node, c.path, typed); break;
                case Arg::NbtTag:        out.items = FilterPrefix({"{", "[", "\""}, typed); break;
                case Arg::StorageId:     break;   // the server's storage ids are not sent
                case Arg::FloatProvider: out = NumberProviderCompletion(partial, false); break;
                case Arg::IntProvider:   out = NumberProviderCompletion(partial, true); break;
                case Arg::Attribute:     out.items = FilterResources(AttributeIds(), typed); break;
                case Arg::AttributeModifierId: {
                    // MC suggests the TARGET's modifiers on the attribute
                    // (getAttributeModifiers); the client knows its own —
                    // the server's sync names them.
                    const std::string* attribute = ValueOf(c.path, "attribute");
                    std::vector<std::string> ids;
                    if (const OwnAttribute* row = attribute ? OwnAttributeRow(*attribute) : nullptr) {
                        for (const std::string& id : row->modifierIds) ids.push_back(Bare(id));
                        SortUnique(ids);
                    }
                    out.items = FilterResources(ids, typed);
                    break;
                }
                case Arg::AttributeValue: {
                    // The attribute's default (a player's, then the registry
                    // row's) and the local player's current base.
                    const std::string* attribute = ValueOf(c.path, "attribute");
                    Game::Attribute a{};
                    std::vector<std::string> values;
                    if (attribute && Game::AttributeFromName(Lower(*attribute), a)) {
                        values.push_back(AttributeNumber(Game::PlayerBaseAttributeValue(a)));
                        values.push_back(AttributeNumber(Game::GetAttributeDef(a).defaultValue));
                        if (const OwnAttribute* row = OwnAttributeRow(*attribute)) values.push_back(AttributeNumber(row->base));
                    }
                    std::vector<std::string> unique;
                    for (std::string& v : values) {
                        if (std::find(unique.begin(), unique.end(), v) == unique.end()) unique.push_back(std::move(v));
                    }
                    out.items = FilterPrefix(unique, typed);
                    break;
                }
                case Arg::ItemPredicate: {
                    // ItemPredicateArgument: `*`, an item id or `#tag`, then
                    // the `[...]` component tests (the item argument's own
                    // key / value completion).
                    const std::string& w = partial.text;
                    if (w.find('[') != std::string::npos) { out = ItemWithComponents(partial); break; }
                    const auto& tags = DataFileIds("tags/item", "#");
                    if (!w.empty() && w[0] == '#') { out.items = FilterNames(tags, w); break; }
                    out.items = FilterPrefix({"*"}, w);
                    const std::vector<std::string> ids = FilterResources(ItemIds(), w);
                    out.items.insert(out.items.end(), ids.begin(), ids.end());
                    if (w.empty()) {
                        const auto top = TopLevel(tags);
                        out.items.insert(out.items.end(), top.begin(), top.end());
                    }
                    break;
                }
                case Arg::DamageType:    out.items = FilterResources(DataFileIds("damage_type"), typed); break;
                case Arg::SlotRange: {
                    // SlotSourceArgument.listSuggestions: SlotRanges.allNames.
                    std::vector<std::string> names;
                    for (const Game::SlotRanges::Range& r : Game::SlotRanges::All()) names.push_back(r.name);
                    out.items = FilterPrefix(names, typed);
                    break;
                }
                case Arg::ItemModifier: {
                    // ResourceOrIdArgument<item_modifier>: the data pack's
                    // ids, or an inline function compound.
                    out.items = FilterResources(DataFileIds("item_modifier"), typed);
                    if (typed.empty()) out.items.push_back("{function:\"");
                    break;
                }
                case Arg::Message: {
                    // MessageArgument: free text; a word starting with '@' is
                    // an entity selector (EntitySelectorParser's
                    // suggestions), any other word may be a player's name
                    // (the chat's name completion).
                    if (!typed.empty() && typed[0] == '@') out = Entities(Arg::Entities, partial);
                    else if (!typed.empty()) out.items = FilterPrefix(PlayerNames(), typed);
                    break;
                }
                case Arg::TextComponent: out = TextComponentCompletion(partial); break;
                case Arg::ConfiguredFeature: out.items = FilterResources(ConfiguredFeatureIds(), typed); break;
                case Arg::Literal:
                case Arg::Command:
                case Arg::Count:
                    break;
            }
            if (!fixed.empty() && out.anchor == partial.start) out.items.insert(out.items.begin(), fixed.begin(), fixed.end());
            return out;
        }

        // Brigadier Suggestions.merge: entries replacing from different
        // points are widened to the earliest one.
        void Merge(const std::string& text, std::vector<Provided>& results, int& anchor, std::vector<std::string>& items) {
            anchor = -1;
            for (const Provided& p : results) {
                if (p.items.empty()) continue;
                if (anchor < 0 || p.anchor < anchor) anchor = p.anchor;
            }
            if (anchor < 0) return;
            std::unordered_set<std::string> seen;
            for (const Provided& p : results) {
                const std::string lead = text.substr(static_cast<size_t>(anchor), static_cast<size_t>(p.anchor - anchor));
                for (const std::string& item : p.items) {
                    std::string full = lead + item;
                    if (seen.insert(full).second) items.push_back(std::move(full));
                }
            }
        }

        std::optional<Completion> CompleteLine(const std::string& text, int begin, int cursor, int depth);

        // Walk `text` [begin, cursor) — a command line without its '/'.
        // nullopt when the command has no tree.
        std::optional<Completion> CompleteLine(const std::string& text, int begin, int cursor, int depth) {
            std::vector<Token> toks;
            Token partial;
            Tokenize(text, begin, cursor, toks, partial);
            Completion result;
            if (toks.empty()) {
                // The command's name.
                result.anchor = partial.start;
                result.items = FilterPrefix(CommandNames(), partial.text);
                return result;
            }
            const Node* root = FindTree(Lower(toks.front().text));
            if (!root) return std::nullopt;
            std::vector<Token> args(toks.begin() + 1, toks.end());
            Walk w{args, partial, *root, {}, {}};
            std::vector<Step> path;
            WalkFrom(w, *root, 0, path);

            std::vector<Provided> provided;
            // Literals first, sorted; then each argument's own list.
            Provided literals;
            literals.anchor = partial.start;
            for (const Candidate& c : w.candidates) {
                if (c.node->IsLiteral() && StartsWithIgnoreCase(c.node->name, partial.text)) literals.items.push_back(c.node->name);
            }
            SortUnique(literals.items);
            provided.push_back(std::move(literals));
            for (const Candidate& c : w.candidates) {
                if (!c.node->IsLiteral()) provided.push_back(Provide(c, partial));
            }
            if (depth < 8) {
                for (int start : w.subCommands) {
                    if (auto sub = CompleteLine(text, start, cursor, depth + 1)) {
                        provided.push_back(Provided{sub->anchor, std::move(sub->items)});
                    }
                }
            }
            Merge(text, provided, result.anchor, result.items);
            if (result.anchor < 0) result.anchor = partial.start;
            return result;
        }

        // ── Usage lines ────────────────────────────────────────────────────

        std::vector<HintLine> HintForLine(const std::string& text, int begin, int cursor, int depth, bool& known);

        std::vector<HintLine> WholeUsage(const std::string& name, const Node& root) {
            std::vector<HintLine> lines;
            for (std::string& l : Game::Cmd::UsageLines(name, root)) lines.push_back({std::move(l), -1, -1});
            return lines;
        }

        std::vector<HintLine> HintForLine(const std::string& text, int begin, int cursor, int depth, bool& known) {
            known = false;
            std::vector<Token> toks;
            Token partial;
            Tokenize(text, begin, cursor, toks, partial);
            if (toks.empty()) {
                // Typing the name: the usage once it names a command.
                const Node* root = FindTree(Lower(partial.text));
                if (!root) {
                    known = !partial.text.empty() && s_serverTreesReceived;
                    return {};
                }
                known = true;
                return WholeUsage(Lower(partial.text), *root);
            }
            const std::string name = Lower(toks.front().text);
            const Node* root = FindTree(name);
            if (!root) return {};
            known = true;
            std::vector<Token> args(toks.begin() + 1, toks.end());
            Walk w{args, partial, *root, {}, {}};
            std::vector<Step> path;
            WalkFrom(w, *root, 0, path);

            // /execute ... run <command>: the inner command's hint.
            if (depth < 8) {
                for (int start : w.subCommands) {
                    bool subKnown = false;
                    std::vector<HintLine> sub = HintForLine(text, start, cursor, depth + 1, subKnown);
                    if (!sub.empty()) return sub;
                }
            }
            if (w.candidates.empty()) return WholeUsage(name, *root);

            // Branches the typed token could still be.
            std::vector<const Candidate*> live;
            for (const Candidate& c : w.candidates) {
                if (c.component > 0 || CouldAccept(*c.node, partial.text)) live.push_back(&c);
            }
            if (live.empty()) for (const Candidate& c : w.candidates) live.push_back(&c);

            // One line per branch: "/cmd <consumed labels> <usage...>", the
            // current argument highlighted; branches that continue alike
            // share a line ("[peaceful|easy|normal|hard]").
            struct Group {
                std::string prefix, labels, tail;
                bool optional = false;
            };
            std::vector<Group> groups;
            for (const Candidate* c : live) {
                std::string prefix = "/" + name;
                for (const Step& s : c->path) prefix += " " + Game::Cmd::Label(*s.node);
                prefix += " ";
                const std::string label = Game::Cmd::Label(*c->node);
                const std::string usage = Game::Cmd::Usage(*c->node, false, *root);
                const std::string tail = usage.size() > label.size() ? usage.substr(label.size() + 1) : std::string();
                bool merged = false;
                for (Group& g : groups) {
                    if (g.prefix == prefix && g.tail == tail && g.optional == c->optional) {
                        g.labels += "|" + label;
                        merged = true;
                        break;
                    }
                }
                if (!merged) groups.push_back({prefix, label, tail, c->optional});
            }
            constexpr size_t kMaxLines = 10;
            std::vector<HintLine> lines;
            for (const Group& g : groups) {
                if (lines.size() + 1 == kMaxLines && groups.size() > kMaxLines) {
                    lines.push_back({"... " + std::to_string(groups.size() - lines.size()) + " more", -1, -1});
                    break;
                }
                const bool several = g.labels.find('|') != std::string::npos;
                const std::string head = g.optional ? "[" + g.labels + "]" : (several ? "(" + g.labels + ")" : g.labels);
                HintLine line;
                line.text = g.prefix + head + (g.tail.empty() ? "" : " " + g.tail);
                line.highlightBegin = static_cast<int>(g.prefix.size());
                line.highlightEnd = static_cast<int>(g.prefix.size() + head.size());
                lines.push_back(std::move(line));
            }
            return lines;
        }

    } // namespace

    void SetServerSyntax(std::vector<std::pair<std::string, Game::Cmd::Node>> trees) {
        s_serverTrees.clear();
        for (auto& [name, root] : trees) s_serverTrees[Lower(name)] = std::move(root);
        s_serverTreesReceived = true;
        ++s_treeGeneration;
    }

    void SetKnownEntityNames(std::vector<std::string> names) {
        s_entityNames = std::move(names);
    }

    void SetOwnAttributes(std::vector<OwnAttribute> rows) {
        s_ownAttributes = std::move(rows);
    }

    void SetDimensionWorldgenIds(const std::vector<std::string>& biomes,
                                 const std::vector<std::string>& structures,
                                 const std::vector<std::string>& biomeTags,
                                 const std::vector<std::string>& structureTags) {
        s_dimension.received      = true;
        s_dimension.biomes        = DisplayNames(biomes, false);
        s_dimension.structures    = DisplayNames(structures, false);
        s_dimension.biomeTags     = DisplayNames(biomeTags, true);
        s_dimension.structureTags = DisplayNames(structureTags, true);
    }

    std::optional<Completion> Complete(const std::string& text, int cursor) {
        if (text.empty() || text[0] != '/') return std::nullopt;
        cursor = std::clamp(cursor, 1, static_cast<int>(text.size()));
        return CompleteLine(text, 1, cursor, 0);
    }

    std::optional<std::vector<HintLine>> Hint(const std::string& text, int cursor) {
        if (text.size() < 2 || text[0] != '/') return std::nullopt;
        cursor = std::clamp(cursor, 1, static_cast<int>(text.size()));
        // Drawn every frame: reuse the last answer while nothing changed.
        static std::string lastText;
        static int lastCursor = -1;
        static uint64_t lastGeneration = 0;
        static std::optional<std::vector<HintLine>> lastResult;
        if (text == lastText && cursor == lastCursor && s_treeGeneration == lastGeneration && lastResult) {
            return lastResult;
        }
        bool known = false;
        std::vector<HintLine> lines = HintForLine(text, 1, cursor, 0, known);
        std::optional<std::vector<HintLine>> result;
        if (known) result = std::move(lines);
        lastText = text;
        lastCursor = cursor;
        lastGeneration = s_treeGeneration;
        lastResult = result;
        return result;
    }

} // namespace Render::CommandSuggestions
