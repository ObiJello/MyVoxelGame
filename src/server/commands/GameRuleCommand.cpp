// File: src/server/commands/GameRuleCommand.cpp
//
// Port of MC GameRuleCommand.java over the world/level/gamerules registry
// (common/world/level/GameRules, baked from GameRules.java).
//
// Vanilla builds a Brigadier node per rule, with the rule's own ArgumentType
// carrying its bounds, and prints two messages:
//
//     commands.gamerule.query = "Gamerule %s is currently set to: %s"
//     commands.gamerule.set   = "Gamerule %s is now set to: %s"
//
// both taking (short id, serialized value). Those exact strings and that exact
// argument order are reproduced below. What is NOT reproduced is the tree: this
// engine's CommandDispatcher is a flat name -> handler map, so the rule and its
// value arrive as plain argv strings and the bounds are checked by hand at the
// point where Brigadier would have rejected the parse (Rules::Parse).
//
// Rule ids follow the vendored decompile, which is snake_case
// (`random_tick_speed`, not the pre-1.21.9 `randomTickSpeed`). The old camelCase
// spellings are accepted as aliases (Rules::Find), so anything already typing
// them keeps working.
//
// Nine rules also live as fields on every Game::World (they predate the
// registry and are read per level in hot paths); setting one writes the World
// of every level AND the registry, so both reads agree.
#include "GameRuleCommand.hpp"
#include "common/core/Features.hpp"
#include "../network/ServerConnection.hpp"
#include "common/network/PacketTypes.hpp"   // ChatMessageS2CPacket for the red failure text
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/World.hpp"
#include "common/world/portal/PortalState.hpp"
#include "common/core/Log.hpp"
#include <cctype>
#include <functional>
#include <cstdlib>   // strtol — integer engine rules
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace Server {

    void GameRuleCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("gamerule", GameRuleCommand::Execute);
    }

    namespace {

        using Game::Rules::Id;

        // The rules that are ALSO World fields: how to read and write them
        // there. Everything not listed lives only in the registry.
        struct WorldField {
            Id id;
            std::function<int(const Game::World&)> get;
            std::function<void(Game::World&, int)> set;
        };
        const WorldField* FindWorldField(Id id) {
            static const WorldField kFields[] = {
                { Id::RandomTickSpeed,
                  [](const Game::World& w) { return w.GetRandomTickSpeed(); },
                  [](Game::World& w, int v) { w.SetRandomTickSpeed(v); } },
                { Id::AdvanceTime,
                  [](const Game::World& w) { return w.GetDoDaylightCycle() ? 1 : 0; },
                  [](Game::World& w, int v) { w.SetDoDaylightCycle(v != 0); } },
                { Id::MobGriefing,
                  [](const Game::World& w) { return w.GetDoMobGriefing() ? 1 : 0; },
                  [](Game::World& w, int v) { w.SetDoMobGriefing(v != 0); } },
                { Id::SpawnMobs,
                  [](const Game::World& w) { return w.GetDoMobSpawning() ? 1 : 0; },
                  [](Game::World& w, int v) { w.SetDoMobSpawning(v != 0); } },
                { Id::TntExplodes,
                  [](const Game::World& w) { return w.GetTntExplodes() ? 1 : 0; },
                  [](Game::World& w, int v) { w.SetTntExplodes(v != 0); } },
                { Id::EntityDrops,
                  [](const Game::World& w) { return w.GetDoEntityDrops() ? 1 : 0; },
                  [](Game::World& w, int v) { w.SetDoEntityDrops(v != 0); } },
                { Id::TntExplosionDropDecay,
                  [](const Game::World& w) { return w.GetTntExplosionDropDecay() ? 1 : 0; },
                  [](Game::World& w, int v) { w.SetTntExplosionDropDecay(v != 0); } },
                { Id::BlockExplosionDropDecay,
                  [](const Game::World& w) { return w.GetBlockExplosionDropDecay() ? 1 : 0; },
                  [](Game::World& w, int v) { w.SetBlockExplosionDropDecay(v != 0); } },
                { Id::MobExplosionDropDecay,
                  [](const Game::World& w) { return w.GetMobExplosionDropDecay() ? 1 : 0; },
                  [](Game::World& w, int v) { w.SetMobExplosionDropDecay(v != 0); } },
            };
            for (const WorldField& f : kFields) if (f.id == id) return &f;
            return nullptr;
        }

        // This engine's own rules — not in GameRules.java, so not in the
        // registry: their whole state is server flags.
        // Values travel as int (booleans 0/1), the way the registry's do,
        // so an integer rule and a boolean one share one path; `isInt`
        // and the bounds pick the parse and the screen's control.
        struct EngineRule {
            const char* id;
            const char* alias;
            const char* label;
            const char* description;
            std::function<int()> get;
            std::function<void(int)> set;
            bool isInt        = false;
            int  minValue     = 0;
            int  maxValue     = 1;
            int  defaultValue = 1;
        };
        const std::vector<EngineRule>& EngineRules() {
            static const std::vector<EngineRule> kRules = {
#if ENABLE_IMMERSIVE_PORTALS
                EngineRule{
                    // Immersive (see-through, walk-through) nether portals vs
                    // vanilla purple blocks. Server-wide. Nether only: the
                    // Hush, Aether and Twilight portals are always blocks
                    // (Portals::FamilyIsImmersive).
                    "immersive_portals", "immersivePortals", "Immersive nether portals",
                    "Lit obsidian frames become see-through, walk-through surfaces instead of purple portal blocks. Turning this off turns standing nether portals into purple blocks. Only nether portals: Hush, Aether and Twilight Forest portals are always normal portal blocks.",
                    [] { return g_integratedServer && g_integratedServer->ImmersivePortalsEnabled(); },
                    [](bool v) { if (g_integratedServer) g_integratedServer->SetImmersivePortals(v); },
                },
#endif
                EngineRule{
                    // Redstone without its physical limits (RedstonePlus.hpp).
                    // Per world (level.dat obeycraft.redstone_plus).
                    "redstone_plus", "redstonePlus", "Redstone Plus",
                    "Redstone dust carries its full signal any distance (no 15-block limit, no repeaters needed to go further) and torches never burn out. Circuits pick the change up as they next update.",
                    [] { return g_integratedServer && g_integratedServer->RedstonePlusEnabled(); },
                    [](bool v) { if (g_integratedServer) g_integratedServer->SetRedstonePlus(v); },
                },
                EngineRule{
                    // Every saved chunk holding a redstone component stays loaded
                    // and ticking, like a force-loaded chunk (ChunkKeeper.hpp).
                    "redstone_chunks", "redstoneChunks", "Keep redstone chunks loaded",
                    "Chunks that contain redstone components stay loaded and ticking at any distance, like /forceload, without loading the land between. Existing worlds are scanned once when this is turned on.",
                    [] { return g_integratedServer && g_integratedServer->RedstoneChunksEnabled(); },
                    [](bool v) { if (g_integratedServer) g_integratedServer->SetRedstoneChunks(v); },
                },
                EngineRule{
                    // How far one vein mine reaches (PlayerSession::
                    // VeinMineFrom). Per world (level.dat obeycraft).
                    "vein_mine_max_blocks", "veinMineMaxBlocks", "Vein mine block limit",
                    "How many extra blocks one vein mine (hold Sneak and the Vein Mine key while a block breaks) takes with the block that was dug. 0 turns vein mining off.",
                    [] { return g_integratedServer ? g_integratedServer->VeinMineMaxBlocks()
                                                   : IntegratedServer::kDefaultVeinMineMaxBlocks; },
                    [](int v) { if (g_integratedServer) g_integratedServer->SetVeinMineMaxBlocks(v); },
                    /*isInt*/ true, /*min*/ 0, /*max*/ IntegratedServer::kMaxVeinMineMaxBlocks,
                    /*default*/ IntegratedServer::kDefaultVeinMineMaxBlocks,
                },
                EngineRule{
                    // One health and one hunger for everyone in the world
                    // (PlayerSessionManager::ShareVitals). Per world (level.dat).
                    "shared_vitals", "sharedVitals", "Shared health and hunger",
                    "Every player in the world has the same health and hunger: damage, healing, eating and exhaustion taken by one happen to all, and when the shared health runs out everyone dies together.",
                    [] { return g_integratedServer && g_integratedServer->SharedVitalsEnabled(); },
                    [](bool v) { if (g_integratedServer) g_integratedServer->SetSharedVitals(v); },
                },
                EngineRule{
                    // The Twilight Forest port (docs/mod-ports.md). Off: the
                    // pool will not light and no portal sends anyone in.
                    "twilight_forest", "twilightForest", "Twilight Forest",
                    "Whether the Twilight Forest can be entered. Turned off, throwing a diamond into a portal pool does nothing, existing Twilight portals stop sending anyone in, and /dimension refuses it. Leaving it always works.",
                    [] { return !g_integratedServer || g_integratedServer->ModDimensionEnabled(Game::DimensionId::TwilightForest); },
                    [](bool v) { if (g_integratedServer) g_integratedServer->SetModDimensionEnabled(Game::DimensionId::TwilightForest, v); },
                },
                EngineRule{
                    // The Aether port. Off: the glowstone frame will not
                    // light and no Aether portal sends anyone in.
                    "aether", "aether", "The Aether",
                    "Whether the Aether can be entered. Turned off, a water bucket no longer lights a glowstone frame, existing Aether portals stop sending anyone in, and /dimension refuses it. Leaving it (or falling out) always works.",
                    [] { return !g_integratedServer || g_integratedServer->ModDimensionEnabled(Game::DimensionId::Aether); },
                    [](bool v) { if (g_integratedServer) g_integratedServer->SetModDimensionEnabled(Game::DimensionId::Aether, v); },
                },
#if ENABLE_PORTAL_GUN
                EngineRule{
                    // Whether the portal gun fires; off closes every placed
                    // pair. Per world (sidecar).
                    "portal_gun", "portalGun", "Portal gun",
                    "Whether the portal gun can place portals. Turning this off closes every placed pair.",
                    [] { return Game::Portals::PortalGunAllowed(); },
                    [](bool v) { if (g_integratedServer) g_integratedServer->SetPortalGunAllowed(v); },
                },
#endif
            };
            return kRules;
        }

        bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
            }
            return true;
        }

        const EngineRule* FindEngineRule(std::string_view name) {
            constexpr std::string_view kNamespace = "minecraft:";
            if (name.size() > kNamespace.size() && EqualsIgnoreCase(name.substr(0, kNamespace.size()), kNamespace)) {
                name.remove_prefix(kNamespace.size());
            }
            for (const EngineRule& rule : EngineRules()) {
                if (EqualsIgnoreCase(name, rule.id) || (rule.alias && EqualsIgnoreCase(name, rule.alias))) return &rule;
            }
            return nullptr;
        }

        std::string BoolText(bool v) { return v ? "true" : "false"; }
        // MC GameRule.serialize for an engine rule: the decimal for an
        // integer, "true"/"false" otherwise.
        std::string EngineText(const EngineRule& rule, int v) {
            return rule.isInt ? std::to_string(v) : BoolText(v != 0);
        }

    } // namespace

    std::vector<GameRuleCommand::RuleInfo> GameRuleCommand::Rules() {
        std::vector<RuleInfo> out;
        out.reserve(Game::Rules::kCount + EngineRules().size());
        for (size_t i = 0; i < Game::Rules::kCount; ++i) {
            const Game::Rules::Def& def = Game::Rules::AllDefs()[i];
            RuleInfo info;
            info.id           = def.key;
            info.label        = def.label;
            info.category     = Game::Rules::CategoryName(def.category);
            info.description  = def.description;
            info.defaultValue = Game::Rules::Serialize(def.id, def.defaultValue);
            info.isInt        = def.type == Game::Rules::Type::Int;
            info.minValue     = def.minValue;
            info.maxValue     = def.maxValue;
            info.implemented  = Game::Rules::IsImplemented(def.id);
            out.push_back(std::move(info));
        }
        for (const EngineRule& rule : EngineRules()) {
            RuleInfo info;
            info.id           = rule.id;
            info.label        = rule.label;
            info.category     = Game::Rules::CategoryName(Game::Rules::Category::Misc);
            info.description  = rule.description;
            info.defaultValue = EngineText(rule, rule.defaultValue);
            info.isInt        = rule.isInt;
            info.minValue     = rule.minValue;
            info.maxValue     = rule.maxValue;
            info.implemented  = true;
            out.push_back(std::move(info));
        }
        return out;
    }

    std::string GameRuleCommand::ReadValue(const std::string& id) {
        if (!g_integratedServer) return {};
        if (const EngineRule* engine = FindEngineRule(id)) return EngineText(*engine, engine->get());
        const Game::Rules::Def* def = Game::Rules::Find(id);
        if (!def) return {};
        // A World-field rule reads from the world, the authoritative copy.
        if (const WorldField* field = FindWorldField(def->id)) {
            if (Game::World* world = g_integratedServer->Overworld().World()) {
                return Game::Rules::Serialize(def->id, field->get(*world));
            }
        }
        return Game::Rules::Serialize(def->id);
    }

    namespace {
        // MC CommandSourceStack.sendFailure: the message in ChatFormatting.RED
        // (0xFF5555), to the sender only.
        void SendFailure(ServerConnection& connection, const std::string& text) {
            Network::ChatMessageS2CPacket packet;
            packet.senderId = 0;
            packet.position = 1;
            packet.segments.push_back(Network::ChatSegmentData{
                text, 0xFFFF5555u, Network::ChatClickAction::None, "", ""});
            connection.SendChatMessage(packet);
        }
    } // namespace

    void GameRuleCommand::Execute(const CommandSourceStack& source,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& /*sessionManager*/) {
        (void)source;
        Game::World* world = g_integratedServer ? g_integratedServer->GetWorld() : nullptr;
        if (!world) {
            connection.SendChatMessage("Gamerules are unavailable (no world)", 1);
            return;
        }

        if (args.empty()) {
            // MC's bare `/gamerule` is not executable — Brigadier reports an
            // incomplete command. With no usage machinery here, listing the
            // rules is the closest useful equivalent.
            std::string list;
            for (size_t i = 0; i < Game::Rules::kCount; ++i) {
                if (!list.empty()) list += ", ";
                list += Game::Rules::AllDefs()[i].key;
            }
            for (const EngineRule& rule : EngineRules()) list += std::string(", ") + rule.id;
            connection.SendChatMessage("Usage: /gamerule <rule> [value]", 1);
            connection.SendChatMessage("Available rules: " + list, 1);
            return;
        }

        // ── the engine's own rules ──────────────────────────────────────────
        if (const EngineRule* engine = FindEngineRule(args[0])) {
            if (args.size() < 2) {
                connection.SendChatMessage(
                    "Game rule " + std::string(engine->id) + " is currently set to " +
                    EngineText(*engine, engine->get()), 1);
                return;
            }
            int value = 0;
            if (engine->isInt) {
                // Brigadier IntegerArgumentType: a whole number, within
                // the rule's bounds, nothing trailing.
                const std::string& text = args[1];
                char* end = nullptr;
                const long parsed = std::strtol(text.c_str(), &end, 10);
                const bool whole = !text.empty() && end != text.c_str() && *end == '\0';
                if (!whole || parsed < engine->minValue || parsed > engine->maxValue) {
                    SendFailure(connection,
                        "Value must be a whole number between " + std::to_string(engine->minValue) +
                        " and " + std::to_string(engine->maxValue));
                    return;
                }
                value = static_cast<int>(parsed);
            } else {
                if (EqualsIgnoreCase(args[1], "true")) value = 1;
                else if (EqualsIgnoreCase(args[1], "false")) value = 0;
                else { SendFailure(connection, "Value must be true or false"); return; }
            }
            // MC GameRuleCommand.setRule: ERROR_GAME_RULE_NOT_SET when the
            // value is what it already is.
            if (engine->get() == value) {
                SendFailure(connection, "Game rule " + std::string(engine->id) +
                                        " is already set to " + EngineText(*engine, value));
                return;
            }
            engine->set(value);
            g_integratedServer->WriteLevelDat();
            connection.SendChatMessage(
                "Game rule " + std::string(engine->id) + " is now set to " + EngineText(*engine, value), 1);
            return;
        }

        const Game::Rules::Def* def = Game::Rules::Find(args[0]);
        if (!def) {
            SendFailure(connection, "Unknown gamerule: " + args[0]);
            return;
        }
        // The three pre-26 names whose meaning flipped (disableRaids ->
        // raids) are not accepted for SETTING: "disableRaids true" would
        // have to mean raids=false, which nobody typing it expects to see
        // echoed back. Vanilla has no such name any more at all.
        if (def->legacyName && def->legacyInverted && args.size() >= 2) {
            std::string_view typed = args[0];
            constexpr std::string_view kNamespace = "minecraft:";
            if (typed.size() > kNamespace.size() && EqualsIgnoreCase(typed.substr(0, kNamespace.size()), kNamespace)) {
                typed.remove_prefix(kNamespace.size());
            }
            if (EqualsIgnoreCase(typed, def->legacyName)) {
                connection.SendChatMessage(
                    std::string(def->legacyName) + " is now " + def->key + " with the opposite meaning: use /gamerule " +
                    def->key + " <true|false>", 1);
                return;
            }
        }

        // Query form: /gamerule <rule>
        // MC: Component.translatable("commands.gamerule.query", id, serialize(value))
        if (args.size() < 2) {
            connection.SendChatMessage(
                "Game rule " + std::string(def->key) + " is currently set to " + ReadValue(def->key), 1);
            return;
        }

        // Set form: /gamerule <rule> <value>. The value is validated the way
        // the rule's ArgumentType would have parsed it.
        const std::optional<int> value = Game::Rules::Parse(def->id, args[1]);
        if (!value) {
            if (def->type == Game::Rules::Type::Bool) {
                SendFailure(connection, "Value must be true or false");
            } else if (def->maxValue == std::numeric_limits<int>::max()) {
                // Brigadier's own out-of-range parse error, restated.
                SendFailure(connection,
                    "Value must be a whole number, at least " + std::to_string(def->minValue));
            } else {
                SendFailure(connection,
                    "Value must be a whole number between " + std::to_string(def->minValue) +
                    " and " + std::to_string(def->maxValue));
            }
            return;
        }

        // MC GameRuleCommand.setRule: `if (gameRules.get(gameRule).equals(value))
        // throw ERROR_GAME_RULE_NOT_SET` — a red "already set to", and nothing
        // is written or broadcast.
        if (Game::Rules::GetInt(def->id) == *value) {
            SendFailure(connection, "Game rule " + std::string(def->key) + " is already set to " +
                                    Game::Rules::Serialize(def->id, *value));
            return;
        }

        Game::Rules::Set(def->id, *value);
        // Gamerules are server-wide in MC (one GameRules on the server that
        // every level reads); a World-field rule keeps its own copy per
        // level here, so all of them are written.
        if (const WorldField* field = FindWorldField(def->id)) {
            g_integratedServer->ForEachLevel([&](ServerLevel& level) {
                if (level.World()) field->set(*level.World(), *value);
            });
        }
        // level.dat is what carries the rule to the next session. MC rewrites
        // it on the autosave; doing it now as well means a crash between
        // autosaves cannot lose the change.
        g_integratedServer->WriteLevelDat();
        // MC MinecraftServer.onGameRuleChanged: the rules a client mirrors
        // (reduced_debug_info, immediate_respawn) go out at once.
        g_integratedServer->BroadcastWorldRules();

        // Time-related rules affect client prediction — resync immediately.
        if (def->id == Id::AdvanceTime) {
            g_integratedServer->ForceTimeSync();
        }

        std::string message = "Game rule " + std::string(def->key) + " is now set to " +
                              Game::Rules::Serialize(def->id, *value);
        if (!Game::Rules::IsImplemented(def->id)) {
            message += " (not implemented yet: stored, no effect)";
        }
        connection.SendChatMessage(message, 1);
    }

} // namespace Server
