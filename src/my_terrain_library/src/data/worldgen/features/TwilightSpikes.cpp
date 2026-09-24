#include "data/worldgen/features/TwilightSpikes.h"
#include "data/worldgen/features/TwilightFeatures.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/Heightmap.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — world/components/feature/BlockSpikeFeature.java
// (startSpike / makeSpike), world/components/speleothem/Stalactite.java and
// util/features/FeatureLogic.java (worldGenReplaceable, isBlockNotOk).
//
// Tags expanded by hand (the TF block tags are not loaded):
//   #twilightforest:supports_stalagmites = #twilightforest:deadrock
//       (deadrock, cracked_deadrock, weathered_deadrock) + minecraft:packed_ice
//   #twilightforest:clouds = fluffy_cloud, wispy_cloud, rainy_cloud, snowy_cloud
// A TF member only counts while its REAL block is registered: a stand-in
// (deadrock -> stone) must not turn every stone block into a stalagmite
// support, which would change where the mod's spikes are allowed.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {
namespace twilight {

namespace {

constexpr int kUpdateAll = 3;   // Block.UPDATE_ALL

struct SpikeBlocks {
    std::vector<std::string> supportsStalagmites;   // block ids
    std::vector<std::string> notOkBlocks;           // bedrock, giant blocks, clouds, hardened dark leaves
    BlockState* stone = nullptr;
};

// Resolve a TF tag member only when the engine has the mod's own block.
void addRealTwilight(std::vector<std::string>& out, const char* name) {
    if (levelgen::twilight_blocks::isStandIn(name)) return;
    const std::string id = levelgen::twilight_blocks::resolveName(name);
    if (!id.empty()) out.push_back(id);
}

const SpikeBlocks& spikeBlocks() {
    static std::once_flag s_once;
    static SpikeBlocks s_blocks;
    std::call_once(s_once, [] {
        // #twilightforest:supports_stalagmites
        addRealTwilight(s_blocks.supportsStalagmites, "twilightforest:deadrock");
        addRealTwilight(s_blocks.supportsStalagmites, "twilightforest:cracked_deadrock");
        addRealTwilight(s_blocks.supportsStalagmites, "twilightforest:weathered_deadrock");
        s_blocks.supportsStalagmites.push_back("minecraft:packed_ice");

        // FeatureLogic.isBlockNotOk: bedrock, GiantBlock instances, #clouds,
        // hardened_dark_leaves (liquids are tested on the state).
        s_blocks.notOkBlocks.push_back("minecraft:bedrock");
        addRealTwilight(s_blocks.notOkBlocks, "twilightforest:giant_cobblestone");
        addRealTwilight(s_blocks.notOkBlocks, "twilightforest:giant_log");
        addRealTwilight(s_blocks.notOkBlocks, "twilightforest:giant_leaves");
        addRealTwilight(s_blocks.notOkBlocks, "twilightforest:giant_obsidian");
        addRealTwilight(s_blocks.notOkBlocks, "twilightforest:fluffy_cloud");
        addRealTwilight(s_blocks.notOkBlocks, "twilightforest:wispy_cloud");
        addRealTwilight(s_blocks.notOkBlocks, "twilightforest:rainy_cloud");
        addRealTwilight(s_blocks.notOkBlocks, "twilightforest:snowy_cloud");
        addRealTwilight(s_blocks.notOkBlocks, "twilightforest:hardened_dark_leaves");

        s_blocks.stone = world::level::block::Blocks::getDefaultState("minecraft:stone");
    });
    return s_blocks;
}

bool isIn(const std::vector<std::string>& ids, BlockState* state) {
    if (state == nullptr) return false;
    const std::string& id = state->getIdentifier();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// FeatureLogic.isBlockNotOk
bool isBlockNotOk(BlockState* state) {
    if (state == nullptr) return false;
    return state->isFluid() || isIn(spikeBlocks().notOkBlocks, state);
}

// WeightedList.of(ores).getRandom(random).orElse(Blocks.STONE): one
// nextInt(totalWeight) walked in list order; an empty (zero-weight) list
// draws nothing and falls back to stone.
BlockState* pickOre(const Stalactite& config, levelgen::WorldgenRandom& random) {
    BlockState* stone = spikeBlocks().stone;
    int32_t total = 0;
    for (const auto& entry : config.weightedOres) total += std::max<int32_t>(entry.second, 0);
    if (total <= 0) return stone;
    int32_t selection = random.nextInt(total);
    for (const auto& entry : config.weightedOres) {
        selection -= std::max<int32_t>(entry.second, 0);
        if (selection < 0) return entry.first != nullptr ? entry.first : stone;
    }
    return stone;
}

// BlockSpikeFeature.makeSpike
bool makeSpike(levelgen::WorldGenLevel& level, const core::BlockPos& startPos, const Stalactite& config,
               int32_t length, int32_t dY, levelgen::WorldgenRandom& random, bool hang) {
    const int32_t diameter = static_cast<int32_t>(static_cast<float>(length) / 4.5f);   // diameter of the base

    // Only place spikes on solid ground, not on the tops of trees.
    if (!hang) {
        const core::BlockPos below = startPos.below(2);
        BlockState* belowState = level.getBlockState(below);
        if (!isIn(spikeBlocks().supportsStalagmites, belowState)
            && (!worldGenReplaceable(belowState)
                || belowState == nullptr
                || !belowState->isFaceSturdy(level, below, core::Direction::UP)
                || isBlockNotOk(belowState))) {
            return false;
        }
    }

    const bool single = config.weightedOres.empty();
    for (int32_t dx = -diameter; dx <= diameter; ++dx) {
        for (int32_t dz = -diameter; dz <= diameter; ++dz) {
            // Determine how long this spike will be.
            const int32_t absx = std::abs(dx);
            const int32_t absz = std::abs(dz);
            const int32_t dist = static_cast<int32_t>(static_cast<float>(std::max(absx, absz))
                                                      + static_cast<float>(std::min(absx, absz)) * 0.5f);
            int32_t spikeLength;
            if (dist <= 0) {
                spikeLength = length;
            } else {
                const int32_t bound = static_cast<int32_t>(static_cast<float>(length)
                                                           / (static_cast<float>(dist) + 0.25f));
                // Java's nextInt throws for a non-positive bound; the base
                // diameter (length / 4.5) keeps the bound >= 2, so this guard
                // never changes a draw.
                spikeLength = bound > 0 ? random.nextInt(bound) : 0;
            }

            for (int32_t i = -1; i < spikeLength; ++i) {
                const core::BlockPos placement = startPos.offset(dx, i * dY, dz);
                if (worldGenReplaceable(level.getBlockState(placement))
                    && (dY > 0 || placement.getY() < level.getHeight(levelgen::Heightmap::Types::MOTION_BLOCKING_NO_LEAVES,
                                                                      placement.getX(), placement.getZ()) - 2)) {
                    BlockState* ore = single ? (config.singleOre != nullptr ? config.singleOre : spikeBlocks().stone)
                                             : pickOre(config, random);
                    if (ore != nullptr) level.setBlock(placement, ore, kUpdateAll);
                }
            }
        }
    }
    return true;
}

// BlockSpikeFeature.startSpike(level, startPos, ore, length, lengthMinimum,
// lengthMaximum, clearance, hang, random)
bool startSpikeScan(levelgen::WorldGenLevel& level, const core::BlockPos& startPos, const Stalactite& config,
                    int32_t length, int32_t lengthMinimum, int32_t lengthMaximum, int32_t clearance,
                    bool hang, levelgen::WorldgenRandom& random) {
    if (lengthMaximum < std::max(lengthMinimum, 1)) return false;

    core::BlockPos::MutableBlockPos movingPos(startPos.getX(), startPos.getY(), startPos.getZ());
    int32_t clearedLength = 0;
    const int32_t dY = hang ? -1 : 1;

    // First find an air block.
    for (int32_t i = 0; i < length; ++i) {
        clearedLength = i;
        if (worldGenReplaceable(level.getBlockState(movingPos))) break;
        movingPos.move(0, dY, 0);
    }

    // Since this gets skipped from the previous line, we invoke it manually.
    movingPos.move(0, dY, 0);

    // Then find a solid block.
    const int32_t remainingScanLength = length - clearedLength + clearance;
    int32_t finalLength = clearedLength - clearance;
    for (int32_t i = 0; i < remainingScanLength; ++i) {
        finalLength = clearedLength + i;
        if (!worldGenReplaceable(level.getBlockState(movingPos))) break;
        movingPos.move(0, dY, 0);
    }

    finalLength = std::min(length, finalLength);
    if (finalLength < lengthMinimum || finalLength > lengthMaximum) return false;

    return makeSpike(level, startPos, config, finalLength, dY, random, hang);
}

} // namespace

const Stalactite& stoneStalactite() {
    // BlockSpikeFeature.STONE_STALACTITE = new Stalactite(Either.right(STONE), 0.25F, 11, 1)
    static const Stalactite s_stone = [] {
        Stalactite s;
        s.singleOre = world::level::block::Blocks::getDefaultState("minecraft:stone");
        s.sizeVariation = 0.25f;
        s.maxLength = 11;
        s.weight = 1;
        return s;
    }();
    return s_stone;
}

bool startSpike(levelgen::WorldGenLevel& level, const core::BlockPos& startPos,
                const Stalactite& config, levelgen::WorldgenRandom& random, bool hanging,
                int32_t forcedMaxHeight) {
    const int32_t maxInclusive = config.maxLength;
    const int32_t minInclusive = static_cast<int32_t>(static_cast<float>(maxInclusive) * config.sizeVariation);

    // Mth.randomBetweenInclusive(random, min, max) = nextInt(max - min + 1) + min
    const int32_t length = random.nextInt(maxInclusive - minInclusive + 1) + minInclusive;

    return startSpikeScan(level, startPos, config, length, minInclusive,
                          std::min(maxInclusive, forcedMaxHeight), 4, hanging, random);
}

bool worldGenReplaceable(BlockState* state) {
    // FeatureLogic.worldGenReplaceable = isReplaceable(state, false)
    return isReplaceable(state, false);
}

} // namespace twilight
} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
