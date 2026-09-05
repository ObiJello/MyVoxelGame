// File: src/server/commands/LocateCommand.hpp
// /locate — a port of MC's LocateCommand.java.
//   /locate structure <id|#tag>   findNearestMapStructure, radius 100 chunk
//                                 cells; distance is horizontal, Y shown as ~
//   /locate biome <id|#tag>       findClosestBiome3d, 6400 blocks, sampled
//                                 every 32 blocks / 64 vertically; 3D
//                                 distance. Deliberate difference: the Y
//                                 shown is the column's terrain surface, not
//                                 the sample's (MC's is often underground)
//   /locate poi nether_portal     the one point of interest this engine
//                                 indexes (NetherPortalIndex), 256 blocks;
//                                 MC's POI manager knows many more
// Ids take MC's forms — village_plains, minecraft:village_plains,
// #minecraft:village — and, beyond MC, a bare tag name (is_jungle) when a
// tag file of that name exists. Messages are commands.locate.* verbatim;
// the coordinates are a green click-to-teleport run (MC's click suggests
// the /tp; ours runs it).
// The searches live in level/LocateFinder.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class LocateCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(ServerPlayer& sender,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
