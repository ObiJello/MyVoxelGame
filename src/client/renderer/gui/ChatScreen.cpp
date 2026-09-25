// File: src/client/renderer/gui/ChatScreen.cpp
#include "ChatScreen.hpp"
#include "GuiGraphics.hpp"
#include "../../network/NetworkClient.hpp"
#include "../../entity/RemotePlayerManager.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/entity/Morph.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include "client/world/ClientLevel.hpp"
#include <cstdlib>
#include <filesystem>
#include <unordered_map>

namespace {
    // /locate's registries, as MC's ResourceOrTagArgument / ResourceOrTag-
    // KeyArgument suggest them (SharedSuggestionProvider.suggestResource):
    // every element as a full id ("minecraft:plains", "twilightforest:
    // lich_tower") and every tag as "#<namespace>:<path>".
    //   biome     — the engine biome table (BiomeRegistry: vanilla, the
    //               Hush, the Twilight Forest, the Aether), the same list the
    //               server's biome sources draw from
    //   structure — data/<ns>/worldgen/structure/*.json for every data
    //               namespace, which is exactly what the terrain library's
    //               StructureSets registry loads (minecraft, aether,
    //               twilightforest), so structures added or removed there
    //               need no change here
    //   tags      — data/<ns>/tags/worldgen/<kind>/**.json
    // Read once per kind. The data root is the server's (MC_DATA_ROOT, else
    // ./data); client and server ship together.
    struct WorldgenRegistry {
        std::vector<std::string> elements;   // sorted full ids
        std::vector<std::string> tags;       // sorted "#ns:path"
        std::vector<std::string> topTags;    // the tags without a '/' in their path
    };

    std::filesystem::path WorldgenDataRoot() {
        const char* env = std::getenv("MC_DATA_ROOT");
        return std::filesystem::path(env ? env : "data");
    }

    const WorldgenRegistry& WorldgenIds(const std::string& kind) {
        static std::unordered_map<std::string, WorldgenRegistry> cache;
        auto it = cache.find(kind);
        if (it != cache.end()) return it->second;
        WorldgenRegistry reg;
        std::error_code ec;
        const std::filesystem::path root = WorldgenDataRoot();
        for (const auto& nsDir : std::filesystem::directory_iterator(root, ec)) {
            if (!nsDir.is_directory(ec)) continue;
            const std::string ns = nsDir.path().filename().string();
            if (kind == "structure") {
                for (const auto& e : std::filesystem::directory_iterator(nsDir.path() / "worldgen" / kind, ec)) {
                    if (e.is_regular_file(ec) && e.path().extension() == ".json") {
                        reg.elements.push_back(ns + ":" + e.path().stem().string());
                    }
                }
            }
            const std::filesystem::path tagDir = nsDir.path() / "tags" / "worldgen" / kind;
            for (auto e = std::filesystem::recursive_directory_iterator(tagDir, ec);
                 !ec && e != std::filesystem::recursive_directory_iterator(); e.increment(ec)) {
                if (!e->is_regular_file(ec) || e->path().extension() != ".json") continue;
                std::string path = std::filesystem::relative(e->path(), tagDir, ec).replace_extension().generic_string();
                reg.tags.push_back("#" + ns + ":" + path);
                if (path.find('/') == std::string::npos) reg.topTags.push_back(reg.tags.back());
            }
            ec.clear();
        }
        if (kind == "biome") {
            for (Game::BiomeId id = 0; id < Game::BiomeRegistry::Count(); ++id) {
                std::string name(Game::BiomeRegistry::Get(id).name);
                reg.elements.push_back(name.find(':') == std::string::npos ? "minecraft:" + name : name);
            }
        }
        for (auto* v : {&reg.elements, &reg.tags, &reg.topTags}) {
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }
        return cache.emplace(kind, std::move(reg)).first->second;
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

    // SharedSuggestionProvider.filterResources: with a ':' typed the whole id
    // must match, else the namespace or the path may — so "meadow" offers
    // minecraft:hush_meadows and minecraft:meadow, "twilight" every
    // twilightforest: id, "#is" the is_* tags.
    void SuggestResources(const std::vector<std::string>& ids, const std::string& typedRaw,
                          const std::string& prefix, std::vector<std::string>& out) {
        std::string typed = typedRaw;
        for (char& c : typed) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!typed.empty()) {
            if (!prefix.empty()) {
                if (typed.compare(0, prefix.size(), prefix) != 0) return;
                typed.erase(0, prefix.size());
            }
        }
        const bool hasNamespace = typed.find(':') != std::string::npos;
        for (const std::string& full : ids) {
            const std::string id = full.substr(prefix.size());
            const size_t colon = id.find(':');
            const bool match = typed.empty() ||
                (hasNamespace ? MatchesSubStr(typed, id)
                              : (MatchesSubStr(typed, id.substr(0, colon)) || MatchesSubStr(typed, id.substr(colon + 1))));
            if (match) out.push_back(full);
        }
    }

    // ── The current dimension's ids (WorldgenIdsS2C) ───────────────────
    // What /locate offers once the server has said where the player is:
    // only this dimension's biomes and structures (and the tags with a member
    // here), shown WITHOUT their namespace — "hush_meadows", not
    // "minecraft:hush_meadows". The server resolves a bare name against the
    // player's dimension first (LocateFinder CanonicalWorldgenId), so what is
    // offered is exactly what runs. Before the packet lands (or from a server
    // that never sends it) the global registries above are the fallback.
    struct DimensionIds {
        bool received = false;
        std::vector<std::string> biomes, structures;         // display names
        std::vector<std::string> biomeTags, structureTags;   // "#name"
    };
    DimensionIds s_dimensionIds;

    // The bare path of each full id; the full id only where two namespaces
    // share a path within the one dimension (so the choice stays exact).
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
            out.push_back(uses[p] > 1 ? id : (tags ? "#" + p : p));
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // MC's substring rule (MatchesSubStr) against the shown name: "mead"
    // offers hush_meadows, "#in" the in_* tags.
    void SuggestNames(const std::vector<std::string>& names, const std::string& typedRaw,
                      std::vector<std::string>& out) {
        std::string typed = typedRaw;
        for (char& c : typed) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const std::string& name : names) {
            if (typed.empty() || MatchesSubStr(typed, name) ||
                (typed[0] == '#' && name[0] == '#' && MatchesSubStr(typed.substr(1), name.substr(1)))) {
                out.push_back(name);
            }
        }
    }
}

namespace {
    // Wall-clock millis since some fixed epoch, mirroring Java's System.currentTimeMillis() /
    // Util.getMillis() that MC uses in EditBox.renderWidget for cursor blinking.
    long long NowMillis() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // ── Tab-completion provider (MC's ClientSuggestionProvider) ────────
    //
    // Returns the suggestions for the word AT the cursor in `text`, plus
    // the index where that word starts (anchor). Anchor + the existing
    // cursor position bracket the substring to REPLACE when applying.

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

    // List every known player name (self + remotes). Sorted alphabetically
    // case-insensitive (MC's getOnlinePlayerNames() returns players in the
    // order they're listed in the tab list, but vanilla also alphabetises
    // for the suggestion popup so the output is deterministic).
    std::vector<std::string> CollectPlayerNames() {
        std::vector<std::string> names;
        if (Client::g_networkClient) {
            const std::string& self = Client::g_networkClient->GetPlayerName();
            if (!self.empty()) names.push_back(self);
        }
        if (Client::g_remotePlayerManager) {
            for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                if (!rp.name.empty()) names.push_back(rp.name);
            }
        }
        std::sort(names.begin(), names.end(),
                  [](const std::string& a, const std::string& b) {
                      for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
                          int ca = std::tolower(static_cast<unsigned char>(a[i]));
                          int cb = std::tolower(static_cast<unsigned char>(b[i]));
                          if (ca != cb) return ca < cb;
                      }
                      return a.size() < b.size();
                  });
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    // Find the start of the word containing `cursor`. A word boundary is
    // a space (matches MC's CommandSuggestions which splits on whitespace).
    int FindWordStart(const std::string& text, int cursor) {
        int i = cursor;
        while (i > 0 && text[i - 1] != ' ') --i;
        return i;
    }

    // Tokenise the text from start up to `cursor` (so we know which arg
    // we're completing). The first token (after the leading '/') is the
    // command name; everything else is its args.
    std::vector<std::string> TokenizeBeforeCursor(const std::string& text, int cursor) {
        std::vector<std::string> out;
        std::string cur;
        for (int i = 0; i < cursor && i < (int)text.size(); ++i) {
            char c = text[i];
            if (c == ' ') {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    std::string ToLowerCopy(std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    }

    // Every entity slug the shared registry knows — the same table /summon
    // resolves against, so the popup can never drift from what works.
    std::vector<std::string> CollectEntitySlugs() {
        std::vector<std::string> out;
        out.reserve(Game::kEntityTypeCount);
        for (int i = 0; i < Game::kEntityTypeCount; ++i) {
            const auto& info = Game::kEntityTypeTable[i];
            if (!info.slug.empty()) out.emplace_back(info.slug);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // Block names as /shape accepts them: lower-case, spaces as underscores
    // ("Infested Stone" -> "infested_stone").
    std::vector<std::string> CollectBlockNames() {
        std::vector<std::string> out;
        out.reserve(static_cast<size_t>(Game::BlockID::Count));
        out.emplace_back("air");
        for (size_t i = 1; i < static_cast<size_t>(Game::BlockID::Count); ++i) {
            std::string n = Game::BlockRegistry::Get(static_cast<Game::BlockID>(i)).name;
            if (n.empty()) continue;
            for (char& c : n) {
                c = (c == ' ') ? '_'
                              : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            out.push_back(std::move(n));
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // The gamerule table ChatScreen suggests — mirrors GameRuleCommand's
    // snake_case ids (the camelCase aliases still parse but are not offered).
    struct RuleSuggestion { const char* name; bool isBool; };
    constexpr RuleSuggestion kRuleSuggestions[] = {
        {"advance_time", true},   {"block_explosion_drop_decay", true},
        {"do_mob_spawning", true}, {"entity_drops", true},
        {"mob_explosion_drop_decay", true}, {"mob_griefing", true},
        {"random_tick_speed", false}, {"tnt_explodes", true},
        {"tnt_explosion_drop_decay", true},
    };

    // ── @selector completion ────────────────────────────────────────────
    //
    // `word` is the whole @-token (spaces never appear inside selector
    // brackets in this engine's parser), `wordStart` its index in the input,
    // `posInWord` the cursor relative to it. Inside brackets the completion
    // slot is the run since the last '[', ',' or '=' — which is what lets
    // "@e[type=zom<TAB>" complete just the value, and a further ",limit="
    // start a fresh key.
    std::vector<std::string> SelectorSuggestions(const std::string& word, int wordStart,
                                                 int posInWord, int& anchorOut) {
        const size_t br = word.find('[');
        if (br == std::string::npos || posInWord <= static_cast<int>(br)) {
            anchorOut = wordStart;
            // "@e[" as its own entry so a second TAB dives straight into the
            // bracket; the bare forms are what /kill @a wants.
            return {"@a", "@e", "@e[", "@p", "@r", "@s"};
        }
        int sub = static_cast<int>(br) + 1;
        for (int i = sub; i < posInWord && i < static_cast<int>(word.size()); ++i) {
            const char c = word[i];
            if (c == '[' || c == ',' || c == '=') sub = i + 1;
        }
        anchorOut = wordStart + sub;
        const char prev = sub > 0 ? word[sub - 1] : '[';
        if (prev == '=') {
            // Value position: which key is this?
            int ke = sub - 1, ks = ke;
            while (ks > static_cast<int>(br) && word[ks - 1] != '[' && word[ks - 1] != ',') --ks;
            std::string key = ToLowerCopy(word.substr(ks, ke - ks));
            // A '!'-negated value keeps its '!' and completes the rest.
            if (posInWord > sub && sub < static_cast<int>(word.size()) && word[sub] == '!') {
                ++anchorOut;
            }
            if (key == "type")     return CollectEntitySlugs();
            if (key == "sort")     return {"arbitrary", "furthest", "nearest", "random"};
            if (key == "gamemode") return {"adventure", "creative", "spectator", "survival"};
            if (key == "limit")    return {"1", "10", "100"};
            if (key == "name")     return CollectPlayerNames();
            return {};
        }
        // Key position. "]" closes the selector once at least one pair is in.
        std::vector<std::string> keys = {
            "distance=", "dx=", "dy=", "dz=", "gamemode=", "limit=", "name=",
            "sort=", "type=", "x=", "x_rotation=", "y=", "y_rotation=", "z="};
        if (sub > static_cast<int>(br) + 1) keys.insert(keys.begin(), "]");
        return keys;
    }

    // One-line usage template per command, drawn greyed above the input as
    // soon as the command name is recognisable.
    std::string UsageHintFor(const std::string& text) {
        if (text.size() < 2 || text[0] != '/') return {};
        std::string cmd = text.substr(1, text.find(' ') == std::string::npos
                                             ? std::string::npos : text.find(' ') - 1);
        cmd = ToLowerCopy(cmd);
        static const std::pair<const char*, const char*> kUsages[] = {
            {"tp",          "/tp <target|x y z> [<dest>|<x y z>] [yaw pitch | facing x y z]"},
            {"teleport",    "/teleport <target|x y z> [<dest>|<x y z>] [yaw pitch | facing x y z]"},
            {"kick",        "/kick <player> [reason]"},
            {"gamemode",    "/gamemode <survival|creative|adventure|spectator> [player]"},
            {"difficulty",  "/difficulty [peaceful|easy|normal|hard]"},
            {"kill",        "/kill [<player>|@e[type=...]|@a|@p|@r|@s]"},
            {"heal",        "/heal [<player>|@e[type=...]|@a|@p|@r|@s]"},
            {"effect",      "/effect give <targets> <effect> [<seconds>|infinite] [<amplifier>] [<hideParticles>] | "
                            "/effect clear [<targets>] [<effect>]"},
            {"summon",      "/summon <entity> [count] [<x> <y> <z>] [fuse=n] [delay=n]"},
            {"morph",       "/morph [baby] <entity> | item <item> | block <block> | xp | herobrine | off"},
            {"control",     "/control <player> | off"},
            {"sheepeat",    "/sheepeat [radius]"},
            {"seed",        "/seed"},
            {"time",        "/time <set|add|query> <time>  (time: <n>[t|s|d])"},
            {"dimension",   "/dimension <overworld|nether|end|hush|twilight|aether>  (travels as the dimension's portal would from where you stand)"},
            {"locate",      "/locate <structure|biome|poi> <id|#tag>"},
            {"fillbiome",   "/fillbiome <x1> <y1> <z1> <x2> <y2> <z2> <biome> [replace <biome|#tag>]"},
            {"gamerule",    "/gamerule <rule> [value]"},
            {"portal",      "/portal <make|make_biway|make_full> <w> <h> <dim> <x> <y> <z> | "
                            "make_loop <w> <h> <dx> <dy> <dz> [turn] | make_mirror <w> <h> | "
                            "set_rotation <ax> <ay> <az> <deg> | set_scale <s> | "
                            "list | info | remove [id] | remove_all"},
            {"scale",       "/scale [0.1-32] [player|radius] (no value resets you; a name scales that player, a radius scales mobs and items around you)"},
            {"entitystats", "/entitystats [tnt|all]"},
            {"tick",        "/tick <query|rate|freeze|unfreeze|step|sprint> [...]"},
            {"clearchat",   "/clearchat"},
            {"shape",       "/shape <block> <cube s|box sx sy sz|wall w [h]|sphere r|dome r|"
                            "cylinder r [h]|pyramid base> [hollow|frame|checker|spaced=N] [at x y z]"},
        };
        for (const auto& [name, usage] : kUsages) {
            if (cmd == name) return usage;
        }
        return {};
    }

    // The full suggestion-provider entry point. Returns the candidate
    // list filtered by the partial word at the cursor, AND sets `anchor`
    // to the index of where to start the replacement.
    std::vector<std::string> ComputeSuggestionsFor(const std::string& text,
                                                   int cursor,
                                                   int& anchorOut) {
        anchorOut = FindWordStart(text, cursor);

        if (text.empty() || text[0] != '/') return {};

        auto tokens = TokenizeBeforeCursor(text, cursor);

        const bool completingCommandName =
            (tokens.size() <= 1) && (cursor <= (int)(tokens.empty() ? 0 : tokens[0].size()));

        int argIndex;
        const int wordStart = anchorOut;
        const std::string word = text.substr(wordStart, cursor - wordStart);

        if (completingCommandName) {
            anchorOut = std::min(cursor, 1);
            argIndex = 0;
        } else {
            // argIndex: 0 = command name, 1+ = positional args after it.
            if (!word.empty() && !tokens.empty()) {
                argIndex = static_cast<int>(tokens.size()) - 1;
            } else {
                argIndex = static_cast<int>(tokens.size());
            }
        }

        std::vector<std::string> candidates;
        // Set by a branch that already filtered MC's resource way.
        bool resourceFiltered = false;
        if (argIndex == 0) {
            // BARE command names (no '/'), from the server's CommandsS2C so
            // the popup can never drift from what is actually registered.
            candidates = Render::GetServerCommandNames();
            candidates.push_back("clearchat");
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()),
                             candidates.end());
        } else {
            std::string cmd = tokens.empty() ? "" : tokens[0];
            if (!cmd.empty() && cmd[0] == '/') cmd.erase(0, 1);
            cmd = ToLowerCopy(cmd);

            const bool takesSelectors =
                cmd == "kill" || cmd == "heal" || cmd == "tp" || cmd == "teleport" ||
                cmd == "effect";

            // An @-token completes as a selector wherever selectors parse,
            // whatever the argument position — this is the "@e[type=<TAB>"
            // path, and it manages its own anchor inside the brackets.
            if (takesSelectors && !word.empty() && word[0] == '@') {
                candidates = SelectorSuggestions(word, wordStart, cursor - wordStart, anchorOut);
            } else if (cmd == "tp" || cmd == "teleport") {
                if (argIndex == 1 || argIndex == 2) {
                    candidates = CollectPlayerNames();
                    // "/tp Alice <TAB>": the destination is someone else —
                    // the name already typed as the target is not offered.
                    if (argIndex == 2 && tokens.size() >= 2) {
                        const std::string target = ToLowerCopy(tokens[1]);
                        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                                        [&](const std::string& n) { return ToLowerCopy(n) == target; }),
                                         candidates.end());
                    }
                    candidates.push_back("@a"); candidates.push_back("@e");
                    candidates.push_back("@p"); candidates.push_back("@r");
                    candidates.push_back("@s");
                    candidates.push_back("~");
                } else if (argIndex == 3) {
                    candidates = {"~"};
                } else if (argIndex >= 4) {
                    candidates = {"~", "facing"};
                }
            } else if (cmd == "effect") {
                // MC EffectCommands' tree: give|clear, EntityArgument
                // targets, a ResourceArgument over the mob_effect registry,
                // then `infinite` (the only literal among the numbers) and
                // hideParticles' bool.
                const std::string sub = tokens.size() >= 2 ? ToLowerCopy(tokens[1]) : "";
                if (argIndex == 1) {
                    candidates = {"clear", "give"};
                } else if (argIndex == 2) {
                    candidates = CollectPlayerNames();
                    candidates.push_back("@a"); candidates.push_back("@e");
                    candidates.push_back("@n"); candidates.push_back("@p");
                    candidates.push_back("@r"); candidates.push_back("@s");
                } else if (argIndex == 3) {
                    // SharedSuggestionProvider.suggestResource matches the
                    // full id or its path: bare paths unless a namespace is
                    // being typed.
                    const bool namespaced = word.rfind("minecraft:", 0) == 0 ||
                                            (!word.empty() && std::string("minecraft:").rfind(word, 0) == 0 &&
                                             word.size() > 1);
                    for (int i = 0; i < Game::kMobEffectCount; ++i) {
                        const std::string name = Game::GetEffectName(static_cast<Game::MobEffectId>(i));
                        candidates.push_back(namespaced ? "minecraft:" + name : name);
                    }
                    std::sort(candidates.begin(), candidates.end());
                } else if (sub == "give" && argIndex == 4) {
                    candidates = {"infinite"};
                } else if (sub == "give" && argIndex == 6) {
                    candidates = {"false", "true"};
                }
            } else if (cmd == "kick") {
                if (argIndex == 1) candidates = CollectPlayerNames();
            } else if (cmd == "control") {
                if (argIndex == 1) {
                    candidates = CollectPlayerNames();
                    candidates.push_back("off");
                }
            } else if (cmd == "difficulty") {
                if (argIndex == 1) candidates = {"easy", "hard", "normal", "peaceful"};
            } else if (cmd == "gamemode") {
                if (argIndex == 1) {
                    candidates = {"adventure", "creative", "spectator", "survival"};
                } else if (argIndex == 2) {
                    candidates = CollectPlayerNames();
                }
            } else if (cmd == "kill") {
                if (argIndex == 1) {
                    candidates = CollectPlayerNames();
                    candidates.push_back("@a"); candidates.push_back("@e");
                    candidates.push_back("@p"); candidates.push_back("@r");
                    candidates.push_back("@s");
                }
            } else if (cmd == "summon") {
                if (argIndex == 1) {
                    candidates = CollectEntitySlugs();
                } else if (argIndex == 2) {
                    candidates = {"1", "10", "100", "1000", "10000", "100000"};
                } else {
                    candidates = {"~", "fuse=80", "delay=1"};
                }
            } else if (cmd == "morph") {
                // /morph [baby] <entity|item <item>|block <block>|xp|off>
                const std::string sub = tokens.size() >= 2 ? ToLowerCopy(tokens[1]) : "";
                if (argIndex == 1) {
                    candidates = CollectEntitySlugs();
                    candidates.push_back("baby");
                    candidates.push_back("block");
                    candidates.push_back("herobrine");
                    candidates.push_back("item");
                    candidates.push_back("off");
                    candidates.push_back("xp");
                } else if (argIndex == 2 && sub == "item") {
                    candidates = Game::Morph::ItemSlugs();
                } else if (argIndex == 2 && sub == "block") {
                    candidates = CollectBlockNames();
                } else if (argIndex == 2 && sub == "baby") {
                    candidates = CollectEntitySlugs();
                } else if (argIndex == 2) {
                    candidates = {"baby"};
                }
            } else if (cmd == "setblock") {
                // /setblock <x> <y> <z> <block>[state] [destroy|keep|replace]
                if (argIndex >= 1 && argIndex <= 3) {
                    candidates = {"~"};
                } else if (argIndex == 4) {
                    candidates = CollectBlockNames();
                } else if (argIndex == 5) {
                    candidates = {"destroy", "keep", "replace"};
                }
            } else if (cmd == "fillbiome") {
                // MC FillBiomeCommand's tree: two BlockPosArguments, a
                // ResourceArgument over the whole biome registry (any biome
                // may be placed in any dimension, so not just this one's),
                // then `replace` and a ResourceOrTagArgument filter.
                if (argIndex >= 1 && argIndex <= 6) {
                    candidates = {"~"};
                } else if (argIndex == 7 || argIndex == 9) {
                    const WorldgenRegistry& reg = WorldgenIds("biome");
                    SuggestResources(reg.elements, word, "", candidates);
                    if (argIndex == 9) SuggestResources(word.empty() ? reg.topTags : reg.tags, word, "#", candidates);
                    resourceFiltered = true;
                } else if (argIndex == 8) {
                    candidates = {"replace"};
                }
            } else if (cmd == "replaceall") {
                // /replaceall <block> <radius> <newblock>
                if (argIndex == 1 || argIndex == 3) {
                    candidates = CollectBlockNames();
                } else if (argIndex == 2) {
                    candidates = {"4", "8", "16", "32", "64"};
                }
            } else if (cmd == "execute") {
                // The subcommand chain: what can follow depends on the word
                // before the one being completed.
                const std::string prev = tokens.size() >= 2
                    ? ToLowerCopy(tokens[tokens.size() - (word.empty() ? 1 : 2)]) : "";
                if (prev == "as" || prev == "at") {
                    candidates = CollectPlayerNames();
                    candidates.push_back("@a"); candidates.push_back("@e");
                    candidates.push_back("@p"); candidates.push_back("@r");
                    candidates.push_back("@s");
                } else if (prev == "run") {
                    candidates = Render::GetServerCommandNames();
                    std::sort(candidates.begin(), candidates.end());
                } else if (prev == "in") {
                    candidates = {"aether", "end", "hush", "nether", "overworld", "twilight"};
                } else if (prev == "positioned" || prev == "rotated") {
                    candidates = {"~", "as", "over"};
                } else if (prev == "facing") {
                    candidates = {"~", "entity"};
                } else if (prev == "anchored") {
                    candidates = {"eyes", "feet"};
                } else if (prev == "if" || prev == "unless") {
                    candidates = {"block", "blocks", "entity"};
                } else if (prev == "block") {
                    candidates = {"~"};
                } else {
                    candidates = {"align", "anchored", "as", "at", "facing", "if", "in",
                                  "positioned", "rotated", "run", "unless"};
                }
            } else if (cmd == "defaultgamemode") {
                if (argIndex == 1) candidates = {"adventure", "creative", "spectator", "survival"};
            } else if (cmd == "spawnall") {
                if (argIndex == 1) candidates = {"adults", "babies", "both"};
                else if (argIndex == 2) candidates = {"2", "4", "8"};
            } else if (cmd == "worldoptions") {
                if (argIndex == 1) {
                    candidates = {"allow_commands", "force_game_mode", "guest_command_access", "joinable"};
                } else if (argIndex == 2) {
                    candidates = {"off", "on"};
                }
            } else if (cmd == "shape") {
                // /shape <block> <form> <sizes...> [quirks] [at x y z]
                if (argIndex == 1) {
                    candidates = CollectBlockNames();
                } else if (argIndex == 2) {
                    candidates = {"box", "cube", "cylinder", "dome",
                                  "pyramid", "sphere", "wall"};
                } else {
                    // Past the sizes: quirks, the `at` anchor, and `~` for its
                    // coordinates. (Sizes themselves are numbers — nothing
                    // useful to suggest.)
                    const std::string prev1 = tokens.size() >= 2 ? tokens[tokens.size() - (word.empty() ? 1 : 2)] : "";
                    bool afterAt = false;
                    for (size_t t = 3; t < tokens.size(); ++t) {
                        if (ToLowerCopy(tokens[t]) == "at") { afterAt = true; break; }
                    }
                    if (afterAt || ToLowerCopy(prev1) == "at") {
                        candidates = {"~"};
                    } else {
                        candidates = {"at", "checker", "frame", "hollow", "spaced=2"};
                    }
                }
            } else if (cmd == "entitystats") {
                if (argIndex == 1) candidates = {"all", "tnt"};
            } else if (cmd == "sheepeat") {
                if (argIndex == 1) candidates = {"8", "16"};
            } else if (cmd == "dimension" || cmd == "dim") {
                if (argIndex == 1) candidates = {"aether", "end", "hush", "nether", "overworld", "twilight"};
            } else if (cmd == "locate") {
                if (argIndex == 1) {
                    candidates = {"biome", "poi", "structure"};
                } else if (argIndex == 2) {
                    // MC's registry suggestions (WorldgenIds): full ids, then
                    // tags. With nothing typed the list is every element plus
                    // the top-level tags (MC also lists the ~100 nested
                    // has_structure/* biome tags there; they appear once a
                    // '#' is typed). Pre-filtered MC's way, not by prefix.
                    const std::string sub = tokens.size() > 1 ? ToLowerCopy(tokens[1]) : "";
                    if ((sub == "structure" || sub == "biome") && s_dimensionIds.received) {
                        // Only what this dimension generates, bare names; tags
                        // once a '#' is typed.
                        const bool biome = sub == "biome";
                        const bool tag = !word.empty() && word[0] == '#';
                        SuggestNames(tag ? (biome ? s_dimensionIds.biomeTags : s_dimensionIds.structureTags)
                                         : (biome ? s_dimensionIds.biomes : s_dimensionIds.structures),
                                     word, candidates);
                    } else if (sub == "structure" || sub == "biome") {
                        const WorldgenRegistry& reg = WorldgenIds(sub);
                        SuggestResources(reg.elements, word, "", candidates);
                        SuggestResources(word.empty() ? reg.topTags : reg.tags, word, "#", candidates);
                    } else if (sub == "poi") {
                        // The portal kinds that can exist where the player
                        // stands: nether portals in the Overworld and the
                        // Nether, hush portals in the Overworld and the Hush.
                        const Game::DimensionId dim = Client::ClientLevels::ActiveDimension();
                        std::vector<std::string> portals;
                        if (dim == Game::DimensionId::Overworld || dim == Game::DimensionId::Hush) {
                            portals.push_back("hush_portal");
                        }
                        if (dim == Game::DimensionId::Overworld || dim == Game::DimensionId::Nether) {
                            portals.push_back("nether_portal");
                        }
                        SuggestNames(portals, word, candidates);
                    }
                    resourceFiltered = true;
                }
            } else if (cmd == "time") {
                if (argIndex == 1) {
                    candidates = {"add", "query", "set"};
                } else if (argIndex == 2) {
                    const std::string sub = tokens.size() > 1 ? ToLowerCopy(tokens[1]) : "";
                    if (sub == "query") {
                        candidates = {"day", "daytime", "gametime"};
                    } else if (sub == "set" || sub == "add") {
                        // MC TimeArgument.listSuggestions: once a number is
                        // typed, offer it with each unit (d, s, t) appended.
                        size_t n = 0;
                        while (n < word.size() && ((word[n] >= '0' && word[n] <= '9') ||
                                                   word[n] == '.' || word[n] == '-')) ++n;
                        if (n > 0) {
                            const std::string number = word.substr(0, n);
                            candidates = {number + "d", number + "s", number + "t"};
                        } else if (sub == "set") {
                            candidates = {"day", "midnight", "night", "noon"};
                        }
                    }
                }
            } else if (cmd == "gamerule") {
                if (argIndex == 1) {
                    for (const auto& r : kRuleSuggestions) candidates.push_back(r.name);
                } else if (argIndex == 2) {
                    const std::string rule = tokens.size() > 1 ? ToLowerCopy(tokens[1]) : "";
                    for (const auto& r : kRuleSuggestions) {
                        if (rule == r.name) {
                            candidates = r.isBool
                                ? std::vector<std::string>{"false", "true"}
                                : std::vector<std::string>{"0", "3", "300"};
                            break;
                        }
                    }
                }
            } else if (cmd == "portal") {
                const std::string sub = tokens.size() > 1 ? ToLowerCopy(tokens[1]) : "";
                if (argIndex == 1) {
                    candidates = {"info", "list", "make", "make_biway", "make_full", "make_loop",
                                  "make_mirror", "remove", "remove_all", "set_rotation", "set_scale"};
                } else if (sub == "make" || sub == "make_biway" || sub == "make_full") {
                    // <w> <h> <dim> <x> <y> <z>
                    if (argIndex == 2 || argIndex == 3) candidates = {"1", "2", "3", "4"};
                    else if (argIndex == 4)             candidates = {"aether", "end", "hush", "nether", "overworld", "twilight"};
                    else if (argIndex <= 7)             candidates = {"~"};
                } else if (sub == "make_loop") {
                    // <w> <h> <dx> <dy> <dz> [turn]
                    if (argIndex == 2 || argIndex == 3) candidates = {"1", "2", "3", "4"};
                    else if (argIndex <= 6)             candidates = {"0", "-8", "8"};
                    else if (argIndex == 7)             candidates = {"0", "90", "180", "-90"};
                } else if (sub == "make_mirror") {
                    if (argIndex == 2 || argIndex == 3) candidates = {"1", "2", "3", "4"};
                } else if (sub == "set_rotation") {
                    // <ax> <ay> <az> <degrees>
                    if (argIndex <= 4)       candidates = {"0", "1"};
                    else if (argIndex == 5)  candidates = {"180", "45", "90"};
                } else if (sub == "set_scale") {
                    if (argIndex == 2) candidates = {"0.5", "2", "4"};
                }
            } else if (cmd == "scale") {
                if (argIndex == 1)      candidates = {"0.5", "1", "2", "4"};
                else if (argIndex == 2) {
                    candidates = CollectPlayerNames();
                    candidates.push_back("@s");
                    candidates.push_back("8"); candidates.push_back("16"); candidates.push_back("32");
                }
            } else if (cmd == "tick") {
                if (argIndex == 1) {
                    candidates = {"freeze", "query", "rate", "sprint", "step", "unfreeze"};
                } else if (argIndex == 2) {
                    const std::string sub = tokens.size() > 1 ? ToLowerCopy(tokens[1]) : "";
                    if (sub == "rate")        candidates = {"20"};
                    else if (sub == "step")   candidates = {"1s", "1t", "stop"};
                    else if (sub == "sprint") candidates = {"1d", "3d", "60s", "stop"};
                }
            }
        }

        // Filter by the partial at the FINAL anchor (a selector path may have
        // moved it inside the token), case-insensitive.
        if (resourceFiltered) return candidates;
        const std::string partial = text.substr(anchorOut, cursor - anchorOut);
        std::vector<std::string> filtered;
        for (const auto& c : candidates) {
            if (partial.empty() || StartsWithIgnoreCase(c, partial)) {
                filtered.push_back(c);
            }
        }
        return filtered;
    }
}

namespace Render {

    namespace {
        // Populated by CommandsS2C on join. The fallback below is what a
        // pre-CommandsS2C server (or a session where the packet hasn't landed
        // yet) gets: the commands that existed when tab-completion was written.
        // It is deliberately NOT kept up to date — the whole point of the
        // packet is that this list stops mattering.
        std::vector<std::string> s_serverCommandNames = {
            "difficulty", "dimension", "entitystats", "gamemode", "gamerule", "kick", "kill", "locate", "portal", "scale", "seed",
            "shape", "sheepeat", "summon", "teleport", "tick", "time", "tp",
        };
    } // namespace

    void SetServerCommandNames(std::vector<std::string> names) {
        s_serverCommandNames = std::move(names);
    }

    const std::vector<std::string>& GetServerCommandNames() {
        return s_serverCommandNames;
    }

    void SetDimensionWorldgenIds(const std::vector<std::string>& biomes,
                                 const std::vector<std::string>& structures,
                                 const std::vector<std::string>& biomeTags,
                                 const std::vector<std::string>& structureTags) {
        s_dimensionIds.received      = true;
        s_dimensionIds.biomes        = DisplayNames(biomes, false);
        s_dimensionIds.structures    = DisplayNames(structures, false);
        s_dimensionIds.biomeTags     = DisplayNames(biomeTags, true);
        s_dimensionIds.structureTags = DisplayNames(structureTags, true);
    }

    void ChatScreen::Open(bool withSlash) {
        m_open = true;
        m_inputText = withSlash ? "/" : "";
        m_submittedMessage.clear();
        m_cursorPos = static_cast<int>(m_inputText.size()); // Caret at end (after '/' if present)
        ResetCursorBlink();
        m_historyIndex = -1;
    }

    void ChatScreen::Close() {
        m_open = false;
        m_inputText.clear();
        m_cursorPos = 0;
        CloseSuggestions();
    }

    void ChatScreen::ResetCursorBlink() {
        // MC's behavior: focusedTime is updated when the cursor moves or the field gains focus,
        // so the cursor immediately reappears in the "visible" half of its blink cycle.
        m_focusedAtMillis = NowMillis();
    }

    bool ChatScreen::ShouldShowCursor() const {
        // MC EditBox.java line 408 (verbatim formula):
        //   showCursor = (Util.getMillis() - focusedTime) / 300L % 2L == 0L
        // → 300ms visible / 300ms hidden, total 600ms cycle, driven by wall-clock millis.
        long long elapsed = NowMillis() - m_focusedAtMillis;
        if (elapsed < 0) elapsed = 0;
        return ((elapsed / 300LL) % 2LL) == 0LL;
    }

    void ChatScreen::SetCursorPosition(int pos) {
        // MC: Mth.clamp(pos, 0, value.length())
        const int n = static_cast<int>(m_inputText.size());
        m_cursorPos = std::clamp(pos, 0, n);
        ResetCursorBlink();
    }

    void ChatScreen::MoveCursor(int dir) { SetCursorPosition(m_cursorPos + dir); }
    void ChatScreen::MoveCursorToStart() { SetCursorPosition(0); }
    void ChatScreen::MoveCursorToEnd()   { SetCursorPosition(static_cast<int>(m_inputText.size())); }

    void ChatScreen::InsertText(const std::string& text) {
        if (!m_open) return;
        for (const char c : text) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u == '\n' || u == '\r' || u == '\t') continue;
            OnCharInput(u);
        }
    }

    void ChatScreen::OnCharInput(unsigned int codepoint) {
        if (!m_open) return;
        if (static_cast<int>(m_inputText.size()) >= MAX_MESSAGE_LENGTH) return;

        // Only accept printable ASCII for now
        if (codepoint >= 32 && codepoint < 127) {
            // MC: EditBox.insertText splices at cursor and advances by length inserted.
            m_inputText.insert(m_inputText.begin() + m_cursorPos, static_cast<char>(codepoint));
            SetCursorPosition(m_cursorPos + 1);
            // MC: typing closes the suggestion popup. The user types TAB
            // again to re-open with the new word.
            CloseSuggestions();
        }
    }

    // ── Cycle-in-place completion ───────────────────────────────────────────
    //
    // Deliberately NOT MC's model. Vanilla opens a popup, leaves the field
    // alone, and only splices the highlighted entry in when you press TAB a
    // second time. Here the first TAB both opens the list AND types the first
    // entry into the field; every further TAB overwrites it with the next one,
    // wrapping at the end. The list stays visible the whole time so you can see
    // what you are cycling through, and typing anything — a space, most
    // usefully — closes the popup and keeps whatever word is showing.
    //
    // The suggestions are computed ONCE, from the partial word as originally
    // typed, and never recomputed while cycling. Recomputing would be wrong:
    // after the first TAB the field holds a complete command, which matches
    // only itself, and the list would collapse to one entry after a single
    // press.
    void ChatScreen::OpenSuggestions() {
        int anchor = 0;
        auto sugg = ComputeSuggestionsFor(m_inputText, m_cursorPos, anchor);
        if (sugg.empty()) {
            CloseSuggestions();
            return;
        }
        m_suggestions      = std::move(sugg);
        m_suggestionAnchor = anchor;
        m_suggestionIndex  = 0;
        m_suggestionsOpen  = true;
        // The slot currently holds the partial word the player typed, so the
        // first apply replaces exactly that.
        m_suggestionCurrentLen = std::max(0, m_cursorPos - anchor);
        ApplySuggestionInPlace();
    }

    void ChatScreen::CloseSuggestions() {
        m_suggestionsOpen = false;
        m_suggestions.clear();
        m_suggestionIndex = 0;
        m_suggestionCurrentLen = 0;
    }

    void ChatScreen::CycleSuggestion(int delta) {
        if (!m_suggestionsOpen || m_suggestions.empty()) return;
        const int n = static_cast<int>(m_suggestions.size());
        m_suggestionIndex = ((m_suggestionIndex + delta) % n + n) % n;
        ApplySuggestionInPlace();
    }

    void ChatScreen::ApplySuggestionInPlace() {
        if (!m_suggestionsOpen || m_suggestions.empty()) return;
        if (m_suggestionIndex < 0 || m_suggestionIndex >= (int)m_suggestions.size()) return;

        const int textLen = static_cast<int>(m_inputText.size());
        // The anchor can go stale if the text changed under us (history recall,
        // a paste). Bail rather than splice at a nonsense offset.
        if (m_suggestionAnchor < 0 || m_suggestionAnchor > textLen) {
            CloseSuggestions();
            return;
        }

        const std::string& sel = m_suggestions[m_suggestionIndex];
        const int replaceLen = std::clamp(m_suggestionCurrentLen, 0, textLen - m_suggestionAnchor);

        // Refuse a replacement that would overflow the field — MC clamps the
        // same limit on insertText. Leaving the previous entry showing is
        // better than silently producing a truncated command.
        if (textLen - replaceLen + static_cast<int>(sel.size()) > MAX_MESSAGE_LENGTH) {
            return;
        }

        m_inputText.replace(m_suggestionAnchor, replaceLen, sel);
        m_suggestionCurrentLen = static_cast<int>(sel.size());
        SetCursorPosition(m_suggestionAnchor + static_cast<int>(sel.size()));
    }

    bool ChatScreen::OnKeyDown(int glfwKey) {
        if (!m_open) return false;

        // TAB — first press opens the list AND fills in its first entry; each
        // further press walks down the list, replacing the filled-in word and
        // wrapping around at the end. Whatever is showing when you type a space
        // (or anything else) is the one you keep. See OpenSuggestions for why
        // this is not MC's two-step model.
        if (glfwKey == GLFW_KEY_TAB) {
            if (m_suggestionsOpen) CycleSuggestion(+1);
            else                   OpenSuggestions();
            return true;
        }

        if (glfwKey == GLFW_KEY_ENTER || glfwKey == GLFW_KEY_KP_ENTER) {
            // Submit message
            if (!m_inputText.empty()) {
                m_submittedMessage = m_inputText;
                // Add to history
                m_history.push_back(m_inputText);
                if (static_cast<int>(m_history.size()) > 50) {
                    m_history.erase(m_history.begin());
                }
            }
            if (m_closeOnSubmit) {
                Close();
            } else {
                // MC ChatScreen.keyPressed with closeOnSubmit = false: the
                // line is sent, the box is emptied and stays up (the bed).
                m_inputText.clear();
                m_cursorPos = 0;
                CloseSuggestions();
            }
            return true;
        }

        if (glfwKey == GLFW_KEY_ESCAPE) {
            // MC: ESC first closes the suggestion popup, second press
            // closes the whole chat.
            if (m_suggestionsOpen) { CloseSuggestions(); return true; }
            Close();
            return true;
        }

        if (glfwKey == GLFW_KEY_BACKSPACE) {
            // MC: EditBox.deleteText(-1) — remove the char before the cursor and step back.
            if (m_cursorPos > 0) {
                m_inputText.erase(m_inputText.begin() + (m_cursorPos - 1));
                SetCursorPosition(m_cursorPos - 1);
            }
            CloseSuggestions();
            return true;
        }

        if (glfwKey == GLFW_KEY_DELETE) {
            // MC: EditBox.deleteText(+1) — remove the char at the cursor.
            if (m_cursorPos < static_cast<int>(m_inputText.size())) {
                m_inputText.erase(m_inputText.begin() + m_cursorPos);
                ResetCursorBlink();
            }
            CloseSuggestions();
            return true;
        }

        if (glfwKey == GLFW_KEY_LEFT)  { CloseSuggestions(); MoveCursor(-1); return true; }
        if (glfwKey == GLFW_KEY_RIGHT) { CloseSuggestions(); MoveCursor(+1); return true; }
        if (glfwKey == GLFW_KEY_HOME)  { CloseSuggestions(); MoveCursorToStart(); return true; }
        if (glfwKey == GLFW_KEY_END)   { CloseSuggestions(); MoveCursorToEnd();   return true; }

        if (glfwKey == GLFW_KEY_UP) {
            // MC: UP cycles suggestion selection when popup is open, else
            // walks chat history.
            if (m_suggestionsOpen) { CycleSuggestion(-1); return true; }
            if (!m_history.empty()) {
                if (m_historyIndex < 0) {
                    m_historyIndex = static_cast<int>(m_history.size()) - 1;
                } else if (m_historyIndex > 0) {
                    m_historyIndex--;
                }
                m_inputText = m_history[m_historyIndex];
                MoveCursorToEnd();
            }
            return true;
        }

        if (glfwKey == GLFW_KEY_DOWN) {
            if (m_suggestionsOpen) { CycleSuggestion(+1); return true; }
            if (m_historyIndex >= 0) {
                m_historyIndex++;
                if (m_historyIndex >= static_cast<int>(m_history.size())) {
                    m_historyIndex = -1;
                    m_inputText.clear();
                } else {
                    m_inputText = m_history[m_historyIndex];
                }
                MoveCursorToEnd();
            }
            return true;
        }

        return true; // Consume all keys when chat is open
    }

    void ChatScreen::Update(float /*deltaTime*/) {
        // No-op: blink is driven by wall-clock millis in ShouldShowCursor() so the rate is
        // independent of frame rate. MC does the same — see EditBox.renderWidget.
    }

    void ChatScreen::Render(GuiGraphics& graphics) {
        if (!m_open) return;

        // Own stratum, above the chat history. Within one stratum the GUI
        // batcher orders all fills under all text (GuiRenderer::BuildBatches
        // pushes fills first and the sort is stable), so the suggestion
        // popup's background could never cover ChatComponent's lines — a
        // broadcast rendered above the popup box but below its entries. MC
        // solves this the same way: CommandSuggestions renders in a later
        // stratum than the chat.
        graphics.NextStratum();

        int guiWidth = graphics.GuiWidth();
        int guiHeight = graphics.GuiHeight();

        // Dark background bar at bottom (MC: full width, 12px tall)
        int inputY = guiHeight - INPUT_HEIGHT - 2;
        const int inputX = 4;
        graphics.Fill(0, inputY, guiWidth, guiHeight, 0x80000000);

        // Render the full input text (no trailing underscore — cursor is drawn separately)
        const int textY = inputY + 2;
        graphics.DrawString(m_inputText, inputX, textY, 0xFFFFFFFF, true);

        // Cursor — matches MC's EditBox.renderWidget (lines 411-458):
        //   MC line 415:   drawX += font.width(text_before) + 1;       // +1 = inter-char gap
        //   MC line 422-4: if insert (mid-text):  cursorX = drawX - 1; // bar drawn between glyphs
        //                  else (at end of text): cursorX = drawX;     // underscore one gap past last char
        //
        // Effectively:
        //   - At end of text: underscore at  text_start + width(before) + 1
        //   - Mid-text:       vertical bar at text_start + width(before) + 1 - 1 = + 0 above
        //                     (which is what GetStringWidth(before) already gives us)
        if (ShouldShowCursor()) {
            std::string before = m_inputText.substr(0, m_cursorPos);
            int beforeWidth = graphics.GetStringWidth(before);
            const bool atEnd = (m_cursorPos >= static_cast<int>(m_inputText.size()));
            if (atEnd) {
                int underscoreX = inputX + beforeWidth + 1;
                graphics.DrawString("_", underscoreX, textY, 0xFFFFFFFF, true);
            } else {
                int barX = inputX + beforeWidth;
                graphics.Fill(barX, textY - 1, barX + 1, textY + 1 + 9, 0xFFFFFFFF);
            }
        }

        // Suggestion popup is drawn LAST so it sits on top of everything.
        // Usage template for the command being typed — MC shows this as the
        // grey inline hint; a line above the input is the closest fit here.
        if (!m_suggestionsOpen) {
            const std::string hint = UsageHintFor(m_inputText);
            if (!hint.empty()) {
                graphics.DrawString(hint, inputX, inputY - 11, 0xFFA0A0A0, true);
            }
        }

        RenderSuggestions(graphics, inputX, inputY);
    }

    void ChatScreen::RenderSuggestions(GuiGraphics& graphics, int inputX, int inputY) {
        if (!m_suggestionsOpen || m_suggestions.empty()) return;

        // X anchor: align the popup's left edge to the start of the word
        // being completed (MC's CommandSuggestions.render). That's:
        //   inputX + width(prefix-before-anchor)
        const std::string before = m_inputText.substr(0, m_suggestionAnchor);
        const int anchorX = inputX + graphics.GetStringWidth(before);

        // Width: max suggestion width + 2px horizontal padding (MC uses 1
        // px padding either side, total 2 in interior).
        int maxW = 0;
        for (const auto& s : m_suggestions) {
            maxW = std::max(maxW, graphics.GetStringWidth(s));
        }
        const int boxX0 = anchorX - 1;            // 1px left padding
        const int boxX1 = anchorX + maxW + 1;     // 1px right padding

        // Height + scroll window: up to MAX_VISIBLE_SUGGESTIONS rows of
        // 12px each. If there are more entries than fit, scroll so the
        // selected index stays in view (MC: SuggestionsList.scroll).
        const int total = static_cast<int>(m_suggestions.size());
        const int visible = std::min(total, MAX_VISIBLE_SUGGESTIONS);
        int scrollStart = 0;
        if (total > MAX_VISIBLE_SUGGESTIONS) {
            // Keep selected centred-ish in the visible window.
            scrollStart = m_suggestionIndex - MAX_VISIBLE_SUGGESTIONS / 2;
            if (scrollStart < 0) scrollStart = 0;
            if (scrollStart + MAX_VISIBLE_SUGGESTIONS > total) {
                scrollStart = total - MAX_VISIBLE_SUGGESTIONS;
            }
        }
        const int rowH = 12;
        const int boxH = visible * rowH;
        // MC positions the popup ABOVE the input. Bottom of popup sits
        // at inputY - 1 (1px gap), top at boxY0.
        const int boxY1 = inputY - 1;
        const int boxY0 = boxY1 - boxH;

        // Box background — MC uses 0xD0000000 (~82% black). Drawn under
        // every row at once.
        graphics.Fill(boxX0, boxY0, boxX1, boxY1, 0xD0000000);

        {
            const std::string hint = UsageHintFor(m_inputText);
            if (!hint.empty()) {
                graphics.DrawString(hint, inputX, boxY0 - 11, 0xFFA0A0A0, true);
            }
        }

        // Each visible row, top-down.
        for (int i = 0; i < visible; ++i) {
            const int idx = scrollStart + i;
            const std::string& text = m_suggestions[idx];
            const int rowY = boxY0 + i * rowH;
            // Text color: selected = MC yellow (0xFFFFFF55), others gray (MC 0xFFAAAAAA).
            const uint32_t color = (idx == m_suggestionIndex)
                ? 0xFFFFFF55u
                : 0xFFAAAAAAu;
            // 1px gutter from box edge; +2 to match MC's CommandSuggestions
            // text indent inside the box.
            graphics.DrawString(text, anchorX, rowY + 2, color, /*dropShadow=*/true);
        }
    }

    std::string ChatScreen::ConsumeSubmittedMessage() {
        std::string msg = m_submittedMessage;
        m_submittedMessage.clear();
        return msg;
    }

} // namespace Render
