#pragma once

#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/carver/CarvingContext.h"
#include "levelgen/carver/CarvingMask.h"
#include "levelgen/Aquifer.h"
#include "levelgen/DensityFunction.h"
#include "world/ChunkPos.h"
#include "world/IChunk.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "random/XoroshiroRandomSource.h"
#include "random/LegacyRandomSource.h"
#include "core/BlockPos.h"
#include <cstdint>
#include <functional>
#include <cmath>
#include <algorithm>

// Reference: net/minecraft/world/level/levelgen/carver/WorldCarver.java

namespace minecraft {
namespace levelgen {
namespace carver {

// Forward declaration
class CaveWorldCarver;
class CanyonWorldCarver;

/**
 * CarveSkipChecker - Function interface for skip checking during carving
 * Reference: WorldCarver.java lines 202-204
 */
using CarveSkipChecker = std::function<bool(const CarvingContext&, double xd, double yd, double zd, int32_t y)>;

/**
 * WorldCarver - Base class for world carvers (caves, canyons, etc.)
 * Reference: WorldCarver.java
 *
 * Template parameter C is the configuration type (CaveCarverConfiguration, etc.)
 */
template<typename C>
class WorldCarver {
protected:
    // Set of block names that can be replaced by carving
    std::set<std::string> m_replaceableBlocks;

public:
    WorldCarver() = default;
    virtual ~WorldCarver() = default;

    /**
     * Get the range in chunks that this carver can affect
     * Reference: WorldCarver.java lines 59-61
     */
    virtual int32_t getRange() const {
        return 4;
    }

    /**
     * Carve the given chunk (XoroshiroRandomSource version)
     * Reference: WorldCarver.java line 170
     */
    virtual bool carve(
        CarvingContext& context,
        const C& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        XoroshiroRandomSource& random,
        Aquifer* aquifer,
        const world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) = 0;

    /**
     * Carve the given chunk (LegacyRandomSource version)
     * Reference: WorldCarver.java line 170
     * Note: Java uses LegacyRandomSource for carving
     */
    virtual bool carve(
        CarvingContext& context,
        const C& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        LegacyRandomSource& random,
        Aquifer* aquifer,
        const world::ChunkPos& sourceChunkPos,
        CarvingMask& mask
    ) = 0;

    /**
     * Check if this chunk should start carving (XoroshiroRandomSource version)
     * Reference: WorldCarver.java line 172
     */
    virtual bool isStartChunk(const C& configuration, XoroshiroRandomSource& random) = 0;

    /**
     * Check if this chunk should start carving (LegacyRandomSource version)
     * Reference: WorldCarver.java line 172
     * Note: Java uses LegacyRandomSource for carving
     */
    virtual bool isStartChunk(const C& configuration, LegacyRandomSource& random) = 0;

protected:
    /**
     * Carve an ellipsoid shape at the given position
     * Reference: WorldCarver.java lines 63-108
     */
    bool carveEllipsoid(
        CarvingContext& context,
        const C& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        Aquifer* aquifer,
        double x, double y, double z,
        double horizontalRadius,
        double verticalRadius,
        CarvingMask& mask,
        CarveSkipChecker skipChecker
    );

    /**
     * Carve a single block
     * Reference: WorldCarver.java lines 110-144
     */
    bool carveBlock(
        CarvingContext& context,
        const C& configuration,
        world::IChunk* chunk,
        std::function<void*(const core::BlockPos&)> biomeGetter,
        CarvingMask& mask,
        core::BlockPos::MutableBlockPos& blockPos,
        core::BlockPos::MutableBlockPos& helperPos,
        Aquifer* aquifer,
        bool& hasGrass
    );

    /**
     * Get the block state to carve with
     * Reference: WorldCarver.java lines 146-157
     */
    BlockState* getCarveState(
        const CarvingContext& context,
        const C& configuration,
        const core::BlockPos& blockPos,
        Aquifer* aquifer
    );

    /**
     * Check if a block can be replaced by carving
     * Reference: WorldCarver.java lines 174-176
     */
    bool canReplaceBlock(const C& configuration, const BlockState* block) const;

    /**
     * Check if the carver can reach from current position
     * Reference: WorldCarver.java lines 178-186
     */
    static bool canReach(
        const world::ChunkPos& chunkPos,
        double x, double z,
        int32_t currentStep, int32_t totalSteps,
        float thickness
    );
};

//=============================================================================
// Template Implementation
//=============================================================================

template<typename C>
bool WorldCarver<C>::carveEllipsoid(
    CarvingContext& context,
    const C& configuration,
    world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    Aquifer* aquifer,
    double x, double y, double z,
    double horizontalRadius,
    double verticalRadius,
    CarvingMask& mask,
    CarveSkipChecker skipChecker
) {
    world::ChunkPos chunkPos = chunk->getPos();
    double centerX = static_cast<double>(chunkPos.getMiddleBlockX());
    double centerZ = static_cast<double>(chunkPos.getMiddleBlockZ());
    double maxDelta = 16.0 + horizontalRadius * 2.0;

    // Quick bounds check
    if (std::abs(x - centerX) > maxDelta || std::abs(z - centerZ) > maxDelta) {
        return false;
    }

    int32_t chunkMinX = chunkPos.getMinBlockX();
    int32_t chunkMinZ = chunkPos.getMinBlockZ();

    // Calculate bounds within chunk - Reference: lines 71-77
    int32_t minXIndex = std::max(static_cast<int32_t>(std::floor(x - horizontalRadius)) - chunkMinX - 1, 0);
    int32_t maxXIndex = std::min(static_cast<int32_t>(std::floor(x + horizontalRadius)) - chunkMinX, 15);
    int32_t minY = std::max(static_cast<int32_t>(std::floor(y - verticalRadius)) - 1, context.getMinGenY() + 1);
    int32_t protectedBlocksOnTop = chunk->isUpgrading() ? 0 : 7;
    int32_t maxY = std::min(static_cast<int32_t>(std::floor(y + verticalRadius)) + 1,
                            context.getMinGenY() + context.getGenDepth() - 1 - protectedBlocksOnTop);
    int32_t minZIndex = std::max(static_cast<int32_t>(std::floor(z - horizontalRadius)) - chunkMinZ - 1, 0);
    int32_t maxZIndex = std::min(static_cast<int32_t>(std::floor(z + horizontalRadius)) - chunkMinZ, 15);

    bool carved = false;
    core::BlockPos::MutableBlockPos blockPos;
    core::BlockPos::MutableBlockPos helperPos;

    // Reference: lines 82-102
    for (int32_t xIndex = minXIndex; xIndex <= maxXIndex; ++xIndex) {
        int32_t worldX = chunkPos.getBlockX(xIndex);
        double xd = (static_cast<double>(worldX) + 0.5 - x) / horizontalRadius;

        for (int32_t zIndex = minZIndex; zIndex <= maxZIndex; ++zIndex) {
            int32_t worldZ = chunkPos.getBlockZ(zIndex);
            double zd = (static_cast<double>(worldZ) + 0.5 - z) / horizontalRadius;

            if (xd * xd + zd * zd >= 1.0) {
                continue;
            }

            bool hasGrass = false;

            for (int32_t worldY = maxY; worldY > minY; --worldY) {
                double yd = (static_cast<double>(worldY) - 0.5 - y) / verticalRadius;

                bool shouldSkip = skipChecker(context, xd, yd, zd, worldY);
                bool alreadyMasked = mask.get(xIndex, worldY, zIndex);

                if (!shouldSkip && !alreadyMasked) {
                    mask.set(xIndex, worldY, zIndex);
                    blockPos.set(worldX, worldY, worldZ);
                    carved |= carveBlock(context, configuration, chunk, biomeGetter,
                                         mask, blockPos, helperPos, aquifer, hasGrass);
                }
            }
        }
    }

    return carved;
}

template<typename C>
bool WorldCarver<C>::carveBlock(
    CarvingContext& context,
    const C& configuration,
    world::IChunk* chunk,
    std::function<void*(const core::BlockPos&)> biomeGetter,
    CarvingMask& mask,
    core::BlockPos::MutableBlockPos& blockPos,
    core::BlockPos::MutableBlockPos& helperPos,
    Aquifer* aquifer,
    bool& hasGrass
) {
    // Get block at position - Reference: line 111
    BlockState* blockType = chunk->getBlockState(blockPos);
    if (blockType == nullptr) {
        return false;
    }

    std::string blockName = blockType->getIdentifier();

    // Check for grass/mycelium - Reference: lines 112-114
    if (blockName == "minecraft:grass_block" || blockName == "minecraft:mycelium") {
        hasGrass = true;
    }

    // Check if block can be replaced - Reference: lines 116-117
    if (configuration.replaceable.count(blockName) == 0) {
        return false;
    }

    // Get the carved block state - Reference: lines 119-121
    BlockState* carvedBlock = getCarveState(context, configuration, blockPos, aquifer);
    if (carvedBlock == nullptr) {
        return false;
    }

    // Set the carved block - Reference: line 123
    chunk->setBlockState(blockPos, carvedBlock, false);

    // Handle grass turning to podzol/dirt - Reference: lines 128-139
    if (hasGrass) {
        helperPos.setWithOffset(blockPos, 0, -1, 0);  // Direction::DOWN
        BlockState* belowBlock = chunk->getBlockState(helperPos);
        if (belowBlock && belowBlock->getIdentifier() == "minecraft:dirt") {
            // Get top material from context (could be grass, podzol, etc.)
            BlockState* topBlock = context.topMaterial(biomeGetter, chunk, helperPos,
                                                                  carvedBlock->isFluid());
            // Apply topMaterial if present
            if (topBlock != nullptr) {
                chunk->setBlockState(helperPos, topBlock, false);
            }
        }
    }

    return true;
}

template<typename C>
BlockState* WorldCarver<C>::getCarveState(
    const CarvingContext& context,
    const C& configuration,
    const core::BlockPos& blockPos,
    Aquifer* aquifer
) {
    // Below lava level, fill with lava - Reference: lines 147-148
    if (blockPos.getY() <= configuration.lavaLevel.resolveY(context)) {
        return minecraft::world::level::block::Blocks::LAVA->defaultBlockState();
    }

    // Use aquifer to determine the block state - Reference: lines 150-156
    // Java: BlockState state = aquifer.computeSubstance(..., 0.0F);
    //       if (state == null) return null;  // Don't carve (barrier)
    //       return state;  // Return what aquifer says (AIR, WATER, etc.)
    if (aquifer) {
        density::DensityFunction::SinglePointContext singleContext(
            blockPos.getX(), blockPos.getY(), blockPos.getZ()
        );
        BlockState* block = aquifer->computeSubstance(singleContext, 0.0);

        if (block == nullptr) {
            // Aquifer returns null = barrier, don't carve here
            return nullptr;
        }

        // Return the block type from aquifer directly
        // Java returns AIR (not CAVE_AIR) for normal caves
        if (block->isAir()) {
            return minecraft::world::level::block::Blocks::AIR->defaultBlockState();  // Normal cave - use AIR (matches Java)
        } else if (block->getIdentifier() == "minecraft:water") {
            return minecraft::world::level::block::Blocks::WATER->defaultBlockState();  // Underwater cave
        } else if (block->getIdentifier() == "minecraft:lava") {
            return minecraft::world::level::block::Blocks::LAVA->defaultBlockState();  // Lava
        }

        // Fallback to air
        return minecraft::world::level::block::Blocks::AIR->defaultBlockState();
    }

    // No aquifer - default to air (matches Java behavior)
    return minecraft::world::level::block::Blocks::AIR->defaultBlockState();
}

template<typename C>
bool WorldCarver<C>::canReplaceBlock(const C& configuration, const BlockState* block) const {
    // Check if block name is in replaceable set - Reference: line 175
    return block && configuration.replaceable.count(block->getIdentifier()) > 0;
}

template<typename C>
bool WorldCarver<C>::canReach(
    const world::ChunkPos& chunkPos,
    double x, double z,
    int32_t currentStep, int32_t totalSteps,
    float thickness
) {
    // Reference: WorldCarver.java lines 178-186
    double xMid = static_cast<double>(chunkPos.getMiddleBlockX());
    double zMid = static_cast<double>(chunkPos.getMiddleBlockZ());
    double xd = x - xMid;
    double zd = z - zMid;
    double remaining = static_cast<double>(totalSteps - currentStep);
    double rr = static_cast<double>(thickness + 2.0f + 16.0f);
    return xd * xd + zd * zd - remaining * remaining <= rr * rr;
}

} // namespace carver
} // namespace levelgen
} // namespace minecraft
