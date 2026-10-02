// File: src/server/commands/StopwatchCommand.hpp
//
// MC StopwatchCommand (26.x):
//
//   /stopwatch create <id>
//   /stopwatch query <id> [<scale>]
//   /stopwatch restart <id>
//   /stopwatch remove <id>
//
// Wall-clock stopwatches kept in the world's data/stopwatches.dat
// (CommandSavedData.hpp). <id> is an Identifier ("minecraft:" by default);
// the existing ids are the tree's suggestions, refreshed whenever one is
// created or removed (MC SUGGEST_STOPWATCHES asks the server).
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class StopwatchCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source, const std::vector<std::string>& args,
                            ServerConnection& connection, PlayerSessionManager& sessionManager);

        static Game::Cmd::Node Syntax();
    };

} // namespace Server
