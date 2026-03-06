#pragma once

#include "util/LinkedOpenHashSet.h"
#include <vector>
#include <cstdint>

// Reference: net/minecraft/world/level/lighting/LeveledPriorityQueue.java

namespace minecraft {
namespace world {
namespace level {
namespace lighting {

/**
 * LeveledPriorityQueue - A priority queue with multiple levels
 * Reference: LeveledPriorityQueue.java
 *
 * This queue maintains separate queues for each priority level.
 * Elements are dequeued from the lowest numbered (highest priority) queue first.
 * Within a level, elements are processed in FIFO order.
 */
class LeveledPriorityQueue {
public:
    // Reference: LeveledPriorityQueue.java lines 11-30
    LeveledPriorityQueue(int levelCount, int minSize);

    /**
     * Remove and return the first element from the highest priority queue
     * Reference: LeveledPriorityQueue.java lines 33-41
     */
    int64_t removeFirstLong();

    /**
     * Check if all queues are empty
     * Reference: LeveledPriorityQueue.java lines 43-45
     */
    bool isEmpty() const;

    /**
     * Remove an element from a specific level queue
     * Reference: LeveledPriorityQueue.java lines 47-54
     *
     * @param node The element to remove
     * @param key The level/priority to remove from
     * @param upperBound Upper bound for searching the first queued level
     */
    void dequeue(int64_t node, int key, int upperBound);

    /**
     * Add an element to a specific level queue
     * Reference: LeveledPriorityQueue.java lines 56-62
     *
     * @param node The element to add
     * @param key The level/priority to add to
     */
    void enqueue(int64_t node, int key);

private:
    /**
     * Update the first queued level after removal
     * Reference: LeveledPriorityQueue.java lines 64-75
     */
    void checkFirstQueuedLevel(int upperBound);

    int m_levelCount;
    std::vector<util::LongLinkedOpenHashSet> m_queues;
    int m_firstQueuedLevel;
};

} // namespace lighting
} // namespace level
} // namespace world
} // namespace minecraft
