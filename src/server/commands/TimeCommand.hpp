// File: src/server/commands/TimeCommand.hpp
// /time command — MC's TimeCommand.java + TimeArgument.java.
// Usage: /time set <day|noon|night|midnight|<time>>
//        /time add <time>
//        /time query <daytime|gametime|day>
//   • <time> is MC's TimeArgument: a float followed by an optional unit —
//     t = ticks (default), s = seconds (20 ticks), d = days (24000 ticks);
//     "0.5d" = 12000, "30s" = 600, "1000" = "1000t". The tick count is
//     rounded and must not be negative ("Tick count must not be less than
//     0, found -20"); any other suffix is "Invalid unit".
//   • set is ABSOLUTE dayTime (day=1000, noon=6000, night=13000,
//     midnight=18000) on EVERY level, so it also resets the day count,
//     exactly like vanilla. The named points exist only for set.
//   • After set/add the new time is force-broadcast to every client
//     (MC forceTimeSynchronization) instead of waiting for the 20-tick sync.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class TimeCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
