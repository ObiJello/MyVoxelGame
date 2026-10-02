// File: src/server/commands/CloneCommand.hpp
//
// MC CloneCommands:
//
//   /clone [from <sourceDimension>] <begin> <end> [to <targetDimension>] <destination>
//          [strict] [replace|masked|filtered <filter>] [force|move|normal]
//
//   replace   every block of the source box (the default)
//   masked    every block but air
//   filtered  only blocks matching <filter>
//   normal    refuse overlapping boxes (the default)
//   force     allow the boxes to overlap
//   move      also allow overlap, and clear the source to air afterwards
//   strict    placed without shape updates, drops or onPlace, and no
//             neighbour updates afterwards
//
// Block entities travel with their data; scheduled ticks in the box are
// copied with the blocks (LevelTicks.copyAreaFrom). Capped by the
// max_block_modifications game rule.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class CloneCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
