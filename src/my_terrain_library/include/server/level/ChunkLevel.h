#pragma once

#include "server/level/FullChunkStatus.h"
#include "world/chunk/status/ChunkStatus.h"

// Reference: net/minecraft/server/level/ChunkLevel.java

// Forward declarations - ChunkPyramid will be implemented later
namespace minecraft { namespace world { namespace chunk { namespace status {
    class ChunkStep;
    class ChunkPyramid;
}}}}

namespace minecraft {
namespace server {
namespace level {

/**
 * ChunkLevel - Ticket level calculations for chunk loading/generation
 * Reference: ChunkLevel.java
 *
 * Ticket levels determine what status a chunk should reach:
 * - Level 31: ENTITY_TICKING (entities can tick)
 * - Level 32: BLOCK_TICKING (blocks can tick)
 * - Level 33: FULL (chunk is fully loaded)
 * - Level 34+: Lower statuses based on distance from full chunk
 */
class ChunkLevel {
public:
    // Constants from ChunkLevel.java lines 10-12
    static constexpr int FULL_CHUNK_LEVEL = 33;
    static constexpr int BLOCK_TICKING_LEVEL = 32;
    static constexpr int ENTITY_TICKING_LEVEL = 31;

    /**
     * Initialize the ChunkLevel system
     * Must be called after ChunkPyramid is initialized
     */
    static void initialize();

    /**
     * Check if the system is initialized
     */
    static bool isInitialized();

    /**
     * Get the radius around full chunk
     * Reference: ChunkLevel.java line 14
     */
    static int getRadiusAroundFullChunk();

    /**
     * Get the maximum level
     * Reference: ChunkLevel.java line 15
     */
    static int getMaxLevel();

    /**
     * Get generation status for a given level
     * Reference: ChunkLevel.java lines 17-19
     *
     * @param level The ticket level
     * @return The ChunkStatus to generate to, or nullptr if level is too high
     */
    static const world::chunk::status::ChunkStatus* generationStatus(int level);

    /**
     * Get the status at a given distance from a full chunk
     * Reference: ChunkLevel.java lines 22-28
     *
     * @param distanceToFullChunk Distance from a full chunk
     * @param defaultValue Value to return if distance is too large
     * @return The ChunkStatus at that distance
     */
    static const world::chunk::status::ChunkStatus* getStatusAroundFullChunk(
        int distanceToFullChunk,
        const world::chunk::status::ChunkStatus* defaultValue = nullptr
    );

    /**
     * Get the status at a given distance from a full chunk (with EMPTY as default)
     * Reference: ChunkLevel.java lines 30-32
     */
    static const world::chunk::status::ChunkStatus* getStatusAroundFullChunkOrEmpty(
        int distanceToFullChunk
    );

    /**
     * Get the ticket level for a given ChunkStatus
     * Reference: ChunkLevel.java lines 34-36
     */
    static int byStatus(const world::chunk::status::ChunkStatus& status);

    /**
     * Get the FullChunkStatus for a given level
     * Reference: ChunkLevel.java lines 38-46
     */
    static FullChunkStatus fullStatus(int level);

    /**
     * Get the ticket level for a given FullChunkStatus
     * Reference: ChunkLevel.java lines 48-59
     */
    static int byStatus(FullChunkStatus status);

    /**
     * Check if a level allows entity ticking
     * Reference: ChunkLevel.java lines 61-63
     */
    static bool isEntityTicking(int level);

    /**
     * Check if a level allows block ticking
     * Reference: ChunkLevel.java lines 65-67
     */
    static bool isBlockTicking(int level);

    /**
     * Check if a level means the chunk is loaded
     * Reference: ChunkLevel.java lines 69-71
     */
    static bool isLoaded(int level);

private:
    static bool s_initialized;
    static const world::chunk::status::ChunkStep* s_fullChunkStep;
    static int s_radiusAroundFullChunk;
    static int s_maxLevel;
};

} // namespace level
} // namespace server
} // namespace minecraft
