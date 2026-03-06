#pragma once

#include "world/level/lighting/DynamicGraphMinFixedPoint.h"
#include "world/ChunkPos.h"
#include <cstdint>

// Reference: net/minecraft/server/level/ChunkTracker.java

namespace minecraft {
namespace server {
namespace level {

/**
 * ChunkTracker - Abstract base class for chunk-based level propagation
 * Reference: ChunkTracker.java
 *
 * This extends DynamicGraphMinFixedPoint to work with chunk positions,
 * propagating levels to the 8 neighboring chunks (Chebyshev distance 1).
 */
class ChunkTracker : public world::level::lighting::DynamicGraphMinFixedPoint {
public:
    /**
     * Update a node's level from a source
     * Reference: ChunkTracker.java lines 68-70
     *
     * @param node The chunk position as a long
     * @param newLevelFrom The new level from the source
     * @param onlyDecreased If true, only decrease levels (optimization)
     */
    void update(int64_t node, int newLevelFrom, bool onlyDecreased);

protected:
    /**
     * Constructor
     * Reference: ChunkTracker.java lines 7-9
     */
    ChunkTracker(int levelCount, int minQueueSize, int minMapSize);

    /**
     * Check if a node is a source (invalid chunk pos)
     * Reference: ChunkTracker.java lines 11-13
     */
    bool isSource(int64_t node) override;

    /**
     * Check all 8 neighbors after an update
     * Reference: ChunkTracker.java lines 15-30
     */
    void checkNeighborsAfterUpdate(int64_t node, int level, bool onlyDecrease) override;

    /**
     * Compute level considering all neighbors
     * Reference: ChunkTracker.java lines 32-60
     */
    int getComputedLevel(int64_t node, int64_t knownParent, int knownLevelFromParent) override;

    /**
     * Compute level from a specific neighbor
     * Reference: ChunkTracker.java lines 62-64
     */
    int computeLevelFromNeighbor(int64_t from, int64_t to, int fromLevel) override;

    /**
     * Get level from the source for a node
     * Reference: ChunkTracker.java line 66
     *
     * This must be implemented by subclasses to get the base level
     * for a chunk (e.g., from tickets).
     */
    virtual int getLevelFromSource(int64_t to) = 0;
};

} // namespace level
} // namespace server
} // namespace minecraft
