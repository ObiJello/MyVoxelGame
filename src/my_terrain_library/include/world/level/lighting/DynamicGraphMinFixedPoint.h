#pragma once

#include "world/level/lighting/LeveledPriorityQueue.h"
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>

// Reference: net/minecraft/world/level/lighting/DynamicGraphMinFixedPoint.java

namespace minecraft {
namespace world {
namespace level {
namespace lighting {

/**
 * DynamicGraphMinFixedPoint - Base class for level propagation algorithms
 * Reference: DynamicGraphMinFixedPoint.java
 *
 * This implements a dynamic graph algorithm for propagating "levels" through
 * a graph of nodes. It's used for:
 * - Light level propagation
 * - Ticket level propagation for chunk loading
 *
 * The algorithm maintains computed levels for each node and efficiently
 * propagates changes through the graph using a priority queue.
 */
class DynamicGraphMinFixedPoint {
public:
    // Reference: DynamicGraphMinFixedPoint.java lines 12-13
    static constexpr int64_t SOURCE = std::numeric_limits<int64_t>::max();
    static constexpr int NO_COMPUTED_LEVEL = 255;

    virtual ~DynamicGraphMinFixedPoint() = default;

    /**
     * Remove a node from the update queue
     * Reference: DynamicGraphMinFixedPoint.java lines 41-48
     */
    void removeFromQueue(int64_t node);

    /**
     * Remove all nodes matching a predicate
     * Reference: DynamicGraphMinFixedPoint.java lines 50-59
     */
    void removeIf(std::function<bool(int64_t)> predicate);

    /**
     * Check if there is work to do
     * Reference: DynamicGraphMinFixedPoint.java lines 129-131
     */
    bool hasWork() const { return m_hasWork.load(std::memory_order_acquire); }

    /**
     * Get the current queue size
     * Reference: DynamicGraphMinFixedPoint.java lines 161-163
     */
    int getQueueSize() const { return static_cast<int>(m_computedLevels.size()); }

protected:
    /**
     * Constructor
     * Reference: DynamicGraphMinFixedPoint.java lines 19-37
     *
     * @param levelCount Number of priority levels (must be < 254)
     * @param minQueueSize Minimum queue size
     * @param minMapSize Minimum map size
     */
    DynamicGraphMinFixedPoint(int levelCount, int minQueueSize, int minMapSize);

    /**
     * Check a node for updates
     * Reference: DynamicGraphMinFixedPoint.java lines 66-68
     */
    void checkNode(int64_t node);

    /**
     * Check an edge for updates
     * Reference: DynamicGraphMinFixedPoint.java lines 70-73
     */
    void checkEdge(int64_t from, int64_t to, int newLevelFrom, bool onlyDecreased);

    /**
     * Check a neighbor after checking edge
     * Reference: DynamicGraphMinFixedPoint.java lines 108-127
     */
    void checkNeighbor(int64_t from, int64_t to, int level, bool onlyDecreased);

    /**
     * Run pending updates
     * Reference: DynamicGraphMinFixedPoint.java lines 133-159
     *
     * @param count Maximum number of updates to process
     * @return Remaining count after processing
     */
    int runUpdates(int count);

    /**
     * Check if a node is a source node
     * Reference: DynamicGraphMinFixedPoint.java lines 165-167
     */
    virtual bool isSource(int64_t node) { return node == SOURCE; }

    /**
     * Get the computed level for a node
     * Reference: DynamicGraphMinFixedPoint.java line 169
     */
    virtual int getComputedLevel(int64_t node, int64_t knownParent, int knownLevelFromParent) = 0;

    /**
     * Check neighbors after a level update
     * Reference: DynamicGraphMinFixedPoint.java line 171
     */
    virtual void checkNeighborsAfterUpdate(int64_t node, int level, bool onlyDecrease) = 0;

    /**
     * Get the current level of a node
     * Reference: DynamicGraphMinFixedPoint.java line 173
     */
    virtual int getLevel(int64_t node) = 0;

    /**
     * Set the level of a node
     * Reference: DynamicGraphMinFixedPoint.java line 175
     */
    virtual void setLevel(int64_t node, int level) = 0;

    /**
     * Compute the level from a neighbor
     * Reference: DynamicGraphMinFixedPoint.java line 177
     */
    virtual int computeLevelFromNeighbor(int64_t from, int64_t to, int fromLevel) = 0;

    int m_levelCount;

private:
    /**
     * Calculate priority from level and computed level
     * Reference: DynamicGraphMinFixedPoint.java lines 61-63
     */
    int calculatePriority(int level, int computedLevel) const;

    /**
     * Internal edge checking implementation
     * Reference: DynamicGraphMinFixedPoint.java lines 75-106
     */
    void checkEdgeInternal(int64_t from, int64_t to, int newLevelFrom, int levelTo,
                           int oldComputedLevel, bool onlyDecreased);

    LeveledPriorityQueue m_priorityQueue;
    std::unordered_map<int64_t, uint8_t> m_computedLevels;
    std::atomic<bool> m_hasWork{false};
};

} // namespace lighting
} // namespace level
} // namespace world
} // namespace minecraft
