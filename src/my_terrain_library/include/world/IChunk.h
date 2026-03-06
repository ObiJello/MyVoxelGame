#pragma once

#include "world/level/block/state/BlockState.h"
#include "core/BlockPos.h"
#include "world/ChunkPos.h"
#include "world/biome/Biome.h"

// Forward declare to avoid circular dependencies
namespace minecraft {
    namespace core {
        class BlockPos;
    }
    namespace levelgen {
        class Heightmap;
    }
    namespace world {
        class ChunkPos;
        class LevelChunkSection;
        namespace chunk {
            namespace status {
                class ChunkStatus;
            }
        }
        namespace level {
            namespace block {
                namespace state {
                    class BlockState;
                }
            }
        }
    }
}

namespace minecraft {
namespace world {

// Import position types for convenience
using minecraft::core::BlockPos;
using BlockState = level::block::state::BlockState;

/**
 * Abstract interface for chunk storage.
 * This allows the terrain generation to work with any chunk implementation.
 *
 * A chunk is 16x?x16 blocks. The height varies by dimension.
 * In Minecraft Overworld: 16x384x16 (Y from -64 to 319)
 */
class IChunk {
public:
    virtual ~IChunk() = default;

    /**
     * Get this chunk's position
     */
    virtual ChunkPos getPos() const = 0;

    /**
     * Get minimum build height (e.g., -64 for Overworld)
     */
    virtual int getMinBuildHeight() const = 0;

    /**
     * Alias for getMinBuildHeight()
     * Reference: LevelHeightAccessor.java getMinY()
     */
    virtual int getMinY() const { return getMinBuildHeight(); }

    /**
     * Get maximum build height (e.g., 320 for Overworld)
     */
    virtual int getMaxBuildHeight() const = 0;

    /**
     * Alias for getMaxBuildHeight()
     * Reference: LevelHeightAccessor.java getMaxY()
     */
    virtual int getMaxY() const { return getMaxBuildHeight(); }

    /**
     * Get number of vertical sections in this chunk
     * Reference: LevelHeightAccessor.java getSectionsCount()
     */
    virtual int32_t getSectionsCount() const = 0;

    /**
     * Get section by index (0 = bottom section)
     * Reference: ChunkAccess.java getSection()
     */
    virtual LevelChunkSection& getSection(int32_t index) = 0;

    /**
     * Get block at absolute position
     */
    virtual BlockState* getBlockState(const BlockPos& pos) = 0;

    /**
     * Get block at relative position (x, z in 0-15, y in getMinBuildHeight() to getMaxBuildHeight())
     */
    virtual BlockState* getBlockState(int localX, int y, int localZ) = 0;

    /**
     * Set block at absolute position
     * @param moved - whether the block is being moved (affects updates)
     * @return previous block state
     */
    virtual BlockState* setBlockState(const BlockPos& pos, BlockState* state, bool moved) = 0;

    /**
     * Set block at relative position
     */
    virtual BlockState* setBlockState(int localX, int y, int localZ, BlockState* state, bool moved) = 0;

    /**
     * Mark a position for post-processing (e.g., fluid updates)
     */
    virtual void markPosForPostprocessing(const BlockPos& pos) = 0;

    /**
     * Check if this chunk is being upgraded from an older format
     * Reference: ChunkAccess.java isUpgrading()
     */
    virtual bool isUpgrading() const { return false; }

    /**
     * Get height at position using specified heightmap type
     * Reference: ChunkAccess.java getHeight()
     */
    virtual int getHeight(int heightmapType, int localX, int localZ) const = 0;

    /**
     * Get biome at a block position
     * Reference: ChunkAccess.java getNoiseBiome()
     */
    virtual biome::BiomeHolder getBiome(const BlockPos& pos) const {
        return biome::BiomeHolder{};  // Default empty biome
    }

    /**
     * Get the persisted chunk status
     * Reference: ChunkAccess.java getPersistedStatus()
     */
    virtual const chunk::status::ChunkStatus* getPersistedStatus() const {
        return nullptr;
    }

    /**
     * Set the persisted chunk status
     * Reference: ChunkAccess.java / ProtoChunk.java setPersistedStatus()
     */
    virtual void setPersistedStatus(const chunk::status::ChunkStatus& status) {
        // Default no-op, overridden in ProtoChunk
    }

    /**
     * Check if light data is correct
     * Reference: ChunkAccess.java isLightCorrect()
     */
    virtual bool isLightCorrect() const { return false; }

    /**
     * Get the time players have spent in this chunk (in ticks)
     * Reference: ChunkAccess.java getInhabitedTime()
     */
    virtual int64_t getInhabitedTime() const { return 0; }

    /**
     * Set the time players have spent in this chunk
     * Reference: ChunkAccess.java setInhabitedTime()
     */
    virtual void setInhabitedTime(int64_t time) { (void)time; }

    /**
     * Get Y coordinate of highest non-empty section
     * Reference: ChunkAccess.java getHighestSectionPosition()
     *
     * @return Y coordinate of highest filled section, or minY if empty
     */
    virtual int32_t getHighestSectionPosition() const = 0;

    /**
     * Get or create a heightmap for the given type (unprimed)
     * Reference: ChunkAccess.java getOrCreateHeightmapUnprimed()
     *
     * Unlike getOrCreateHeightmap(), this returns a pointer for use with
     * primeHeightmaps() which needs to collect multiple heightmaps.
     *
     * @param type - Heightmap type to get/create
     * @return Pointer to the heightmap
     */
    virtual levelgen::Heightmap* getOrCreateHeightmapUnprimed(int heightmapType) = 0;
};

} // namespace world
} // namespace minecraft

// For backwards compatibility with code using ::world::IChunk
namespace world {
    using IChunk = minecraft::world::IChunk;
}
