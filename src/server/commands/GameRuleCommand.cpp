// File: src/server/commands/GameRuleCommand.cpp
#include "GameRuleCommand.hpp"
#include "../network/ServerConnection.hpp"
#include "../IntegratedServer.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include <cctype>
#include <functional>
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

        struct BoolRule {
            const char* name; // canonical (display) casing
            std::function<bool(Game::World&)> get;
            std::function<void(Game::World&, bool)> set;
        };

        // Rule table — add a row per future gamerule.
        const BoolRule kBoolRules[] = {
            {"doDaylightCycle",
             [](Game::World& w) { return w.GetDoDaylightCycle(); },
             [](Game::World& w, bool v) { w.SetDoDaylightCycle(v); }},
        };

        const BoolRule* FindRule(const std::string& arg) {
            const std::string lower = ToLower(arg);
            for (const auto& rule : kBoolRules) {
                if (ToLower(rule.name) == lower) return &rule;
            }
            return nullptr;
        }

        std::optional<bool> ParseBool(const std::string& raw) {
            const std::string arg = ToLower(raw);
            if (arg == "true") return true;
            if (arg == "false") return false;
            return std::nullopt;
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
            connection.SendChatMessage("Usage: /gamerule <rule> [true|false]", 1);
            return;
        }

        const BoolRule* rule = FindRule(args[0]);
        if (!rule) {
            connection.SendChatMessage("Unknown gamerule: " + args[0], 1);
            return;
        }

        // Query form: /gamerule <rule>
        if (args.size() < 2) {
            connection.SendChatMessage(
                std::string(rule->name) + " = " + (rule->get(*world) ? "true" : "false"), 1);
            return;
        }

        auto value = ParseBool(args[1]);
        if (!value) {
            connection.SendChatMessage("Value must be true or false", 1);
            return;
        }

        rule->set(*world, *value);
        // Time-related rules affect client prediction — resync immediately.
        g_integratedServer->ForceTimeSync();
        connection.SendChatMessage(
            std::string("Gamerule ") + rule->name + " is now " + (*value ? "true" : "false"), 1);
    }

} // namespace Server
