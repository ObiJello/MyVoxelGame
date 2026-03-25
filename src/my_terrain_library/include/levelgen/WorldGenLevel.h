#pragma once

/**
 * WorldGenLevel - Interface for world generation level access
 * Reference: net/minecraft/world/level/WorldGenLevel.java
 *
 * This interface provides access to block/chunk data during world generation.
 * In Java, WorldGenLevel extends ServerLevelAccessor which extends LevelAccessor.
 * The key implementation is WorldGenRegion which provides multi-chunk access.
 *
 * Uses BlockState* for block access, matching Java's implementation.
 */

#include "core/BlockPos.h"
#include "world/ChunkPos.h"
#include "world/IChunk.h"
#include "world/level/block/state/BlockState.h"
#include "world/biome/Biome.h"
#include "levelgen/Heightmap.h"
#include <functional>
#include <cstdint>

namespace minecraft {
namespace levelgen {

// Forward declarations
class ChunkGenerator;

/**
 * WorldGenLevel - Core interface for world generation level access
 * Reference: WorldGenLevel.java
 */
class WorldGenLevel {
public:
    virtual ~WorldGenLevel() = default;

    //=========================================================================
    // Block Access (from LevelAccessor)
    //=========================================================================

    /**
     * Get block state at position
     * Reference: LevelReader.getBlockState(BlockPos)
     */
    virtual BlockState* getBlockState(const core::BlockPos& pos) const = 0;

    virtual bool isEmptyBlock(const core::BlockPos& pos) const {
        BlockState* state = getBlockState(pos);
        return state && state->isAir();
    }

    /**
     * Set block at position with update flags
     * Reference: LevelWriter.setBlock(BlockPos, BlockState, int)
     * @param flags Block update flags (19 = UPDATE_NEIGHBORS | UPDATE_CLIENTS typical for features)
     */
    virtual bool setBlock(const core::BlockPos& pos, BlockState* state, int flags) = 0;

    /**
     * Check if state at position matches predicate
     * Reference: LevelReader.isStateAtPosition(BlockPos, Predicate)
     */
    virtual bool isStateAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const = 0;

    /**
     * Check if fluid at position matches predicate
     * Reference: LevelReader.isFluidAtPosition(BlockPos, Predicate)
     */
    virtual bool isFluidAtPosition(const core::BlockPos& pos,
        std::function<bool(BlockState*)> predicate) const = 0;

    virtual bool isWaterAt(const core::BlockPos& pos) const {
        BlockState* state = getBlockState(pos);
        return state && state->hasWaterFluid();
    }

    //=========================================================================
    // Chunk Access (from ChunkSource pattern)
    //=========================================================================

    /**
     * Get chunk at chunk coordinates
     * Reference: WorldGenRegion.getChunk(int, int)
     */
    virtual ::world::IChunk* getChunk(int chunkX, int chunkZ) = 0;

    //=========================================================================
    // Height Access (from LevelHeightAccessor)
    //=========================================================================

    /**
     * Get height at position using specified heightmap type
     * Reference: WorldGenRegion.getHeight(Heightmap.Types, int, int)
     */
    virtual int getHeight(Heightmap::Types type, int x, int z) const = 0;

    /**
     * Get minimum Y coordinate
     * Reference: LevelHeightAccessor.getMinY()
     */
    virtual int getMinY() const = 0;

    /**
     * Get maximum Y coordinate
     * Reference: LevelHeightAccessor.getMaxY()
     */
    virtual int getMaxY() const = 0;

    /**
     * Check if position is outside build height
     * Reference: LevelHeightAccessor.isOutsideBuildHeight(BlockPos)
     */
    virtual bool isOutsideBuildHeight(const core::BlockPos& pos) const {
        int y = pos.getY();
        return y < getMinY() || y >= getMaxY();
    }

    /**
     * Check if position is unobstructed (no entity collision)
     * Reference: CollisionGetter.isUnobstructed - simplified for worldgen
     * In world generation, we just check if the block is air.
     */
    virtual bool isUnobstructed(const core::BlockPos& pos) const {
        BlockState* state = getBlockState(pos);
        return state && state->isAir();
    }

    //=========================================================================
    // Biome Access
    //=========================================================================

    /**
     * Get biome at position
     * Reference: BiomeManager.getBiome(BlockPos) pattern
     */
    virtual const world::biome::Biome* getBiome(const core::BlockPos& pos) const = 0;

    //=========================================================================
    // World Properties (from WorldGenLevel)
    //=========================================================================

    /**
     * Get world seed
     * Reference: WorldGenLevel.getSeed()
     */
    virtual int64_t getSeed() const = 0;

    /**
     * Check if position can be written to
     * Reference: WorldGenLevel.ensureCanWrite(BlockPos)
     * Default returns true - override for distance checking
     */
    virtual bool ensureCanWrite(const core::BlockPos& pos) const {
        return true;
    }

    /**
     * Set currently generating feature description (for debug/crash reports)
     * Reference: WorldGenLevel.setCurrentlyGenerating(Supplier<String>)
     * Default is no-op
     */
    virtual void setCurrentlyGenerating(const std::string& description) {
        // No-op by default
    }

    virtual void scheduleTick(const core::BlockPos& pos, const std::string& blockName, int delay) {
        (void)pos;
        (void)blockName;
        (void)delay;
    }
};

} // namespace levelgen
} // namespace minecraft
