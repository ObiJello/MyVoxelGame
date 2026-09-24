#pragma once
#include "MonsterRoomDecoration.h"
#include <functional>

class Level;

namespace console {
// A bounded first pass through RandomLevelSource::postProcess and
// BiomeDecorator::decorate: consume the original pre-decoration coordinate
// draws, then place lakes, dungeons, ores, shore deposits, trees and surface plants. The caller
// provides generated chunks with generation brightness. The optional hook
// applies the source chunk's rule schematics after biome decoration and before
// snow/ice. Structures and live liquid spread remain separate stages.
struct DecorationStats {
    int oreAttempts=0;
    int shoreAttempts=0;
    int treeAttempts=0;
    int treesPlaced=0;
    int flowerAttempts=0;
    int grassAttempts=0;
    int reedAttempts=0;
    int cactusAttempts=0;
    int springAttempts=0;
    int frozenCells=0;
    int snowLayers=0;
    int desertWells=0;
    int emeraldOre=0;
    int dungeonAttempts=0;
    int dungeonsPlaced=0;
};

// Returns false without writing if the source chunk's 4x4 neighborhood is not
// ready. The two-chunk reach covers ore veins and large tree branches.
bool decorateGeneratedChunk(Level& level,int chunkX,int chunkZ,DecorationStats* stats=nullptr,
                            const std::function<void(int,int)>& afterBiome={},
                            std::vector<DungeonTile>* generatedTiles=nullptr);
}
