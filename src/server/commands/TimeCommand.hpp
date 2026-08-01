// File: src/server/commands/TimeCommand.hpp
// /time command — MC's TimeCommand.java pattern.
// Usage: /time set <day|noon|night|midnight|N>
//        /time add <N>
//        /time query <daytime|gametime|day>
//   • set is ABSOLUTE dayTime (day=1000, noon=6000, night=13000, midnight=18000),
//     so it also resets the day count, exactly like vanilla.
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
