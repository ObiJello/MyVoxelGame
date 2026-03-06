#pragma once

#include "server/level/GenerationChunkHolder.h"
#include "util/StaticCache2D.h"
#include "world/ChunkPos.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/chunk/status/ChunkStep.h"
#include "world/IChunk.h"
#include "world/level/block/state/BlockState.h"
#include "core/BlockPos.h"
#include <atomic>
#include <functional>
#include <memory>

// Reference: net/minecraft/server/level/WorldGenRegion.java

namespace minecraft {

// Forward declarations
namespace world {
    namespace chunk {
        namespace status {
            class ChunkDependencies;
        }
    }
}

namespace server {
namespace level {

// Forward declaration
class ServerLevel;

/**
 * WorldGenRegion - Provides a view of chunks during world generation
 * Reference: WorldGenRegion.java
 *
 * This class:
 * - Wraps a cache of GenerationChunkHolders during generation
 * - Validates block access based on the current generation step
 * - Provides block state read/write with distance checking
 *
 * Key difference from ServerLevel:
 * - Only has access to chunks in the StaticCache2D
 * - Validates writes are within blockStateWriteRadius
 */
class WorldGenRegion {
public:
    using ChunkAccess = ::world::IChunk;

    /**
     * Constructor
     * Reference: WorldGenRegion.java lines 84-94
     *
     * @param level - The server level
     * @param cache - 2D cache of chunk holders
     * @param generatingStep - Current generation step
     * @param center - The center chunk being generated
     */
    WorldGenRegion(
        ServerLevel& level,
        util::StaticCache2D<GenerationChunkHolder*>& cache,
        const world::chunk::status::ChunkStep& generatingStep,
        ChunkAccess& center
    );

    ~WorldGenRegion() = default;

    /**
     * Get the center chunk position
     * Reference: WorldGenRegion.java lines 100-102
     */
    world::ChunkPos getCenter() const;

    /**
     * Set the currently generating feature (for crash reports)
     * Reference: WorldGenRegion.java lines 104-106
     */
    void setCurrentlyGenerating(std::function<std::string()> currentlyGenerating);

    /**
     * Get a chunk at the given coordinates (with EMPTY status)
     * Reference: WorldGenRegion.java lines 108-110
     */
    ChunkAccess* getChunk(int chunkX, int chunkZ);

    /**
     * Get a chunk at the given coordinates with specific status
     * Reference: WorldGenRegion.java lines 112-144
     *
     * @param chunkX - Chunk X coordinate
     * @param chunkZ - Chunk Z coordinate
     * @param targetStatus - Minimum required status
     * @param loadOrGenerate - Ignored in WorldGenRegion (always throws if unavailable)
     * @return The chunk if available at required status
     * @throws ReportedException if chunk not available
     */
    ChunkAccess* getChunk(
        int chunkX,
        int chunkZ,
        const world::chunk::status::ChunkStatus& targetStatus,
        bool loadOrGenerate
    );

    /**
     * Check if a chunk is available at the given coordinates
     * Reference: WorldGenRegion.java lines 146-149
     */
    bool hasChunk(int chunkX, int chunkZ) const;

    /**
     * Validate that we can write to a position
     * Reference: WorldGenRegion.java lines 230-248
     *
     * @param pos - Block position to validate
     * @return true if writing is allowed
     */
    bool ensureCanWrite(const core::BlockPos& pos) const;

    /**
     * Set a block state at the given position
     * Reference: WorldGenRegion.java lines 251-287
     *
     * @param pos - Block position
     * @param blockState - Block state to set
     * @param updateFlags - Update flags (Block::UpdateFlags)
     * @param updateLimit - Recursion limit for neighbor updates
     * @return true if block was set successfully
     */
    bool setBlock(
        const core::BlockPos& pos,
        BlockState* blockState,
        int updateFlags,
        int updateLimit
    );

    /**
     * Get the block state at a position
     * Reference: WorldGenRegion.java lines 151-153
     */
    BlockState* getBlockState(const core::BlockPos& pos) const;

    /**
     * Get the seed for this region
     * Reference: WorldGenRegion.java lines 346-348
     */
    int64_t getSeed() const;

    /**
     * Get the minimum Y level
     * Reference: WorldGenRegion.java lines 406-408
     */
    int getMinY() const;

    /**
     * Get the height (total Y range)
     * Reference: WorldGenRegion.java lines 410-412
     */
    int getHeight() const;

    /**
     * Get the server level (deprecated in Java)
     * Reference: WorldGenRegion.java lines 313-316
     */
    ServerLevel& getLevel();
    const ServerLevel& getLevel() const;

    /**
     * Get the generation step
     */
    const world::chunk::status::ChunkStep& getGeneratingStep() const;

    /**
     * Get the cache
     */
    util::StaticCache2D<GenerationChunkHolder*>& getCache();
    const util::StaticCache2D<GenerationChunkHolder*>& getCache() const;

private:
    /**
     * Get chunk from position helper
     */
    ChunkAccess* getChunk(const world::ChunkPos& pos);
    ChunkAccess* getChunk(const core::BlockPos& pos);

    /**
     * Check if position is outside the write range
     */
    bool isOutsideWriteRange(const core::BlockPos& pos) const;

    // Reference to the server level
    ServerLevel& m_level;

    // Cache of chunk holders for this generation region
    util::StaticCache2D<GenerationChunkHolder*>& m_cache;

    // The center chunk being generated
    ChunkAccess& m_center;

    // The current generation step
    const world::chunk::status::ChunkStep& m_generatingStep;

    // World seed
    int64_t m_seed;

    // Currently generating feature (for crash reports)
    std::function<std::string()> m_currentlyGenerating;

    // Sub-tick counter for ordering
    std::atomic<int64_t> m_subTickCount{0};
};

} // namespace level
} // namespace server
} // namespace minecraft
