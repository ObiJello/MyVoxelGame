// File: src/server/commands/LocateCommand.hpp
// /locate — a port of MC's LocateCommand.java.
//   /locate structure <id|#tag>   findNearestMapStructure, radius 100
//                                 placement cells (Twilight Forest landmarks:
//                                 100 landmark regions, the mod's
//                                 findNearestMapLandmark); distance is
//                                 horizontal
//   /locate biome <id|#tag>       findClosestBiome3d, 6400 blocks, sampled
//                                 every 32 blocks / 64 vertically; 3D
//                                 distance
//   /locate poi <nether_portal|hush_portal>
//                                 the frame portals this engine indexes
//                                 (NetherPortalIndex), 256 blocks; MC's POI
//                                 manager knows many more
// Everything searches the command source's dimension (the player's, or
// `/execute in …`): its own biome source and structure sets, so Hush,
// Twilight Forest, Aether, Nether and End biomes and structures are found in
// their own dimension. Ids take MC's forms — minecraft:village_plains,
// #minecraft:village — plus, beyond MC, a bare name in any namespace
// (hush_meadows, lich_tower, skyroot_meadow, is_aether; a bare name the
// current dimension generates wins, so `forest` in the Twilight Forest is
// twilightforest:forest). Messages are commands.locate.* verbatim; a miss
// for something that generates only in another dimension adds a gray line
// naming it. Deliberate differences: the Y shown is the terrain surface of
// the found column (a cave biome: the open floor nearest the sample inside
// it) so the green coordinates, which RUN "/tp @s x y z" when clicked (MC's
// only suggest it), land the player on the ground.
// The searches live in level/LocateFinder; chat's tab completion
// (ChatScreen) lists the same registries.
#pragma once

#include "CommandDispatcher.hpp"

namespace Server {

    class LocateCommand {
    public:
        static void Register(CommandDispatcher& dispatcher);

        static void Execute(const CommandSourceStack& source,
                            const std::vector<std::string>& args,
                            ServerConnection& connection,
                            PlayerSessionManager& sessionManager);
    };

} // namespace Server
