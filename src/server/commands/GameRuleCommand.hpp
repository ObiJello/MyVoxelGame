// File: src/server/commands/GameRuleCommand.hpp
// /gamerule command — minimal MC GameRuleCommand.java equivalent.
// Usage: /gamerule <rule>            → prints the current value
//        /gamerule <rule> <true|false>
// Rules are kept in a small name→get/set table so future rules are one row.
// Currently: doDaylightCycle (default FALSE here — deliberate deviation from
// vanilla's true; worlds stay frozen at noon until enabled).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class GameRuleCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
