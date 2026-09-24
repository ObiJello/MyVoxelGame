#pragma once
#include "ChunkStorage.h"
#include "GenerationRegion.h"
#include "MonsterRoomDecoration.h"
#include <cstdint>
#include <functional>

namespace console {
// Decorate one generated column against detached copies of its neighborhood.
// The caller owns the source chunks and may safely keep saved/player-edited
// neighbors unchanged. The returned column has no prepared light.
std::unique_ptr<ChunkStorage> decorateNaturalColumn(
    std::int64_t seed,int worldChunks,BiomeScale scale,int chunkX,int chunkZ,
    const std::function<const ChunkStorage&(int,int)>& source,
    const std::function<bool(ChunkStorage&,int,int)>& afterBiome={},
    std::vector<DungeonTile>* generatedTiles=nullptr);
}
