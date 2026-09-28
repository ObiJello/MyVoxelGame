// File: src/client/renderer/gui/CommandSuggestions.cpp
#include "CommandSuggestions.hpp"
#include "ChatScreen.hpp"   // GetServerCommandNames

#include "../../network/NetworkClient.hpp"
#include "../../entity/RemotePlayerManager.hpp"
#include "client/sound/SoundManager.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/entity/EntityNbtHints.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/Morph.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundSource.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/level/GameRules.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
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
                cur.text += c;
                if (c == '"' || c == '\'') quote = c;
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
                case Arg::Float:   return IsNumber(tokens[0]->text, false);
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
                if (kid.type == Arg::Greedy) {
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
                "distance=", "dx=", "dy=", "dz=", "gamemode=", "limit=", "name=",
                "sort=", "type=", "x=", "x_rotation=", "y=", "y_rotation=", "z="};
            return keys;
        }

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
                if (key == "type")          return Provided{out.anchor, FilterResources(EntitySlugs(false), slot)};
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
            if (c.component == 0 || node.type == Arg::Greedy) fixed = FilterPrefix(node.suggestions, typed);
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
                    out.items = FilterResources(ItemIds(), typed);
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
