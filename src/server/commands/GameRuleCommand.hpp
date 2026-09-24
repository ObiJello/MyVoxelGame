// File: src/server/commands/GameRuleCommand.hpp
// /gamerule command — MC GameRuleCommand.java over the game-rule registry
// (common/world/level/GameRules).
// Usage: /gamerule <rule>            → prints the current value
//        /gamerule <rule> <value>
// Every vanilla 26.3 rule is accepted (new id, `minecraft:` id or the old
// camelCase name) plus this engine's own (immersive_portals, portal_gun,
// redstone_plus, redstone_chunks, vein_mine_max_blocks).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class GameRuleCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);

        // What the in-world game-rules screen (World Options → Edit Game
        // Rules) lists: MC GameRules.visitGameRuleTypes — every vanilla rule
        // in registration order, then the engine's own.
        struct RuleInfo {
            std::string id;          // "random_tick_speed"
            std::string label;       // MC lang gamerule.<id>
            std::string category;    // MC GameRuleCategory display name
            std::string description;  // MC lang gamerule.<id>.description, or empty
            std::string defaultValue; // serialized default ("3", "true")
            bool        isInt;
            int         minValue, maxValue;
            // False when the engine lacks the system the rule gates: the
            // screen shows "Not implemented yet" instead of a control, and
            // the value is stored but does nothing.
            bool        implemented;
        };
        static std::vector<RuleInfo> Rules();
        // The live value ("true" / "3"), or empty when no integrated server is
        // running (a remote client cannot read rules).
        static std::string ReadValue(const std::string& id);
    };

} // namespace Server
