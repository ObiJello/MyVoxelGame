// File: src/server/commands/GameRuleCommand.cpp
//
// Port of MC GameRuleCommand.java + the world/level/gamerules package.
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
// point where Brigadier would have rejected the parse.
//
// Rule ids follow the vendored decompile, which is snake_case
// (`random_tick_speed`, not the pre-1.21.9 `randomTickSpeed`). The old camelCase
// spellings are kept as aliases so anything already typing them keeps working.
#include "GameRuleCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../IntegratedServer.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include <cctype>
#include <functional>
#include <limits>
#include <optional>
#include <string>

namespace Server {

    void GameRuleCommand::Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("gamerule", GameRuleCommand::Execute);
    }

    namespace {

        std::string ToLower(std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // MC GameRuleType — the registry knows only these two kinds.
        enum class RuleType { Bool, Int };

        struct Rule {
            const char* id;        // canonical, snake_case (MC GameRule.id())
            const char* alias;     // legacy camelCase spelling, or nullptr
            RuleType    type;
            // Bounds, ints only. MC carries these in the rule's ArgumentType
            // (`IntegerArgumentType.integer(min, max)`), so they are per-rule
            // rather than global.
            int         minValue;
            int         maxValue;
            std::function<std::string(Game::World&)> get;   // already serialized
            std::function<void(Game::World&, const std::string&)> set;
        };

        // MC GameRules.java's registration list, restricted to the rules this
        // engine actually implements. Adding a row here is all a new rule needs.
        //
        //   random_tick_speed — GameRules.java:214, category UPDATES,
        //                       default 3, min 0, max Integer.MAX_VALUE.
        //   advance_time      — the modern id for what older MC called
        //                       doDaylightCycle. This engine defaults it OFF
        //                       (vanilla defaults it on); see World.hpp.
        const std::vector<Rule>& RuleTable() {
            static const std::vector<Rule> kRules = {
                Rule{
                    "random_tick_speed", "randomTickSpeed", RuleType::Int,
                    0, std::numeric_limits<int>::max(),
                    [](Game::World& w) { return std::to_string(w.GetRandomTickSpeed()); },
                    [](Game::World& w, const std::string& v) { w.SetRandomTickSpeed(std::stoi(v)); },
                },
                Rule{
                    "advance_time", "doDaylightCycle", RuleType::Bool,
                    0, 0,
                    [](Game::World& w) { return std::string(w.GetDoDaylightCycle() ? "true" : "false"); },
                    [](Game::World& w, const std::string& v) { w.SetDoDaylightCycle(v == "true"); },
                },
                Rule{
                    // GameRules.java, category MOBS, default true. Gates world
                    // edits made by mobs — a sheep turning grass to dirt.
                    "mob_griefing", "mobGriefing", RuleType::Bool,
                    0, 0,
                    [](Game::World& w) { return std::string(w.GetDoMobGriefing() ? "true" : "false"); },
                    [](Game::World& w, const std::string& v) { w.SetDoMobGriefing(v == "true"); },
                },
                Rule{
                    // GameRules.java:196, category MOBS, default true. Gates
                    // NaturalSpawner only — /summon and despawning still work.
                    "do_mob_spawning", "doMobSpawning", RuleType::Bool,
                    0, 0,
                    [](Game::World& w) { return std::string(w.GetDoMobSpawning() ? "true" : "false"); },
                    [](Game::World& w, const std::string& v) { w.SetDoMobSpawning(v == "true"); },
                },
            };
            return kRules;
        }

        const Rule* FindRule(const std::string& arg) {
            std::string lower = ToLower(arg);
            // MC registers each rule under both its short id and its
            // fully-qualified `minecraft:` identifier, so accept the prefix.
            const std::string kNamespace = "minecraft:";
            if (lower.rfind(kNamespace, 0) == 0) lower = lower.substr(kNamespace.size());

            for (const auto& rule : RuleTable()) {
                if (ToLower(rule.id) == lower) return &rule;
                if (rule.alias && ToLower(rule.alias) == lower) return &rule;
            }
            return nullptr;
        }

        std::optional<bool> ParseBool(const std::string& raw) {
            const std::string arg = ToLower(raw);
            if (arg == "true")  return true;
            if (arg == "false") return false;
            return std::nullopt;
        }

        std::optional<int> ParseInt(const std::string& raw) {
            if (raw.empty()) return std::nullopt;
            size_t consumed = 0;
            int value = 0;
            try {
                value = std::stoi(raw, &consumed);
            } catch (...) {
                return std::nullopt;
            }
            // Reject trailing junk — "3x" must not read as 3.
            if (consumed != raw.size()) return std::nullopt;
            return value;
        }

    } // namespace

    void GameRuleCommand::Execute(ServerPlayer& /*sender*/,
                                  const std::vector<std::string>& args,
                                  ServerConnection& connection,
                                  PlayerSessionManager& /*sessionManager*/) {
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
            for (const auto& rule : RuleTable()) {
                if (!list.empty()) list += ", ";
                list += rule.id;
            }
            connection.SendChatMessage("Usage: /gamerule <rule> [value]", 1);
            connection.SendChatMessage("Available rules: " + list, 1);
            return;
        }

        const Rule* rule = FindRule(args[0]);
        if (!rule) {
            connection.SendChatMessage("Unknown gamerule: " + args[0], 1);
            return;
        }

        // Query form: /gamerule <rule>
        // MC: Component.translatable("commands.gamerule.query", id, serialize(value))
        if (args.size() < 2) {
            connection.SendChatMessage(
                "Gamerule " + std::string(rule->id) + " is currently set to: " +
                rule->get(*world), 1);
            return;
        }

        // Set form: /gamerule <rule> <value>. The value is validated the way
        // the rule's ArgumentType would have parsed it.
        std::string serialized;
        switch (rule->type) {
            case RuleType::Bool: {
                auto value = ParseBool(args[1]);
                if (!value) {
                    connection.SendChatMessage("Value must be true or false", 1);
                    return;
                }
                serialized = *value ? "true" : "false";
                break;
            }
            case RuleType::Int: {
                auto value = ParseInt(args[1]);
                if (!value) {
                    connection.SendChatMessage("Value must be a whole number", 1);
                    return;
                }
                if (*value < rule->minValue || *value > rule->maxValue) {
                    // Brigadier's own out-of-range parse error, restated.
                    connection.SendChatMessage(
                        "Value must be between " + std::to_string(rule->minValue) +
                        " and " + std::to_string(rule->maxValue), 1);
                    return;
                }
                serialized = std::to_string(*value);
                break;
            }
        }

        rule->set(*world, serialized);

        // Time-related rules affect client prediction — resync immediately.
        if (std::string(rule->id) == "advance_time") {
            g_integratedServer->ForceTimeSync();
        }

        connection.SendChatMessage(
            "Gamerule " + std::string(rule->id) + " is now set to: " + serialized, 1);
    }

} // namespace Server
