#pragma once

#include "core/BlockPos.h"
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — world/components/feature/BlockSpikeFeature.java
// (startSpike / makeSpike) and world/components/speleothem/Stalactite.java.
// Shared by the outside_stalagmite feature (TwilightDecorFeatures) and the
// hollow hills' speleothems (levelgen/structure/twilight/TwilightHollowHill).
// Defined in src/data/worldgen/features/TwilightSpikes.cpp.

namespace minecraft {
namespace world { namespace level { namespace block { namespace state { class BlockState; }}}}
using BlockState = world::level::block::state::BlockState;
namespace levelgen {
class WorldGenLevel;
class WorldgenRandom;
}

namespace data {
namespace worldgen {
namespace features {
namespace twilight {

/**
 * Stalactite record: `ores` is Either<List<Pair<Block, weight>>, Block> —
 * a single block when `weightedOres` is empty (then `singleOre` is used),
 * else a WeightedList drawn once per placed block. Blocks are resolved to
 * default states when the config is built (TwilightBlocks for TF names);
 * a null entry falls back to stone, as WeightedList.getRandom().orElse(STONE).
 */
struct Stalactite {
    BlockState* singleOre = nullptr;
    std::vector<std::pair<BlockState*, int32_t>> weightedOres;
    float sizeVariation = 0.25f;
    int32_t maxLength = 11;
    int32_t weight = 1;
};

/** BlockSpikeFeature.STONE_STALACTITE: stone, 0.25, 11, 1. */
const Stalactite& stoneStalactite();

/**
 * startSpike(level, startPos, config, random, hanging, forcedMaxHeight):
 * length = Mth.randomBetweenInclusive(random, (int)(max * variation), max),
 * then the air/solid scan and makeSpike. Returns whether a spike was made.
 */
bool startSpike(levelgen::WorldGenLevel& level, const core::BlockPos& startPos,
                const Stalactite& config, levelgen::WorldgenRandom& random, bool hanging,
                int32_t forcedMaxHeight = std::numeric_limits<int32_t>::max());

/** FeatureLogic.worldGenReplaceable(state). */
bool worldGenReplaceable(BlockState* state);

} // namespace twilight
} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
