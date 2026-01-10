#include "world/level/lighting/LeveledPriorityQueue.h"

// Reference: net/minecraft/world/level/lighting/LeveledPriorityQueue.java

namespace minecraft {
namespace world {
namespace level {
namespace lighting {

// Reference: LeveledPriorityQueue.java lines 11-30
LeveledPriorityQueue::LeveledPriorityQueue(int levelCount, int minSize)
    : m_levelCount(levelCount)
    , m_firstQueuedLevel(levelCount)
{
    m_queues.reserve(levelCount);
    for (int i = 0; i < levelCount; ++i) {
        m_queues.emplace_back(minSize, 0.5f);
    }
}

// Reference: LeveledPriorityQueue.java lines 33-41
int64_t LeveledPriorityQueue::removeFirstLong() {
    util::LongLinkedOpenHashSet& queue = m_queues[m_firstQueuedLevel];
    int64_t result = queue.removeFirstLong();
    if (queue.isEmpty()) {
        checkFirstQueuedLevel(m_levelCount);
    }
    return result;
}

// Reference: LeveledPriorityQueue.java lines 43-45
bool LeveledPriorityQueue::isEmpty() const {
    return m_firstQueuedLevel >= m_levelCount;
}

// Reference: LeveledPriorityQueue.java lines 47-54
void LeveledPriorityQueue::dequeue(int64_t node, int key, int upperBound) {
    util::LongLinkedOpenHashSet& queue = m_queues[key];
    queue.remove(node);
    if (queue.isEmpty() && m_firstQueuedLevel == key) {
        checkFirstQueuedLevel(upperBound);
    }
}

// Reference: LeveledPriorityQueue.java lines 56-62
void LeveledPriorityQueue::enqueue(int64_t node, int key) {
    m_queues[key].add(node);
    if (m_firstQueuedLevel > key) {
        m_firstQueuedLevel = key;
    }
}

// Reference: LeveledPriorityQueue.java lines 64-75
void LeveledPriorityQueue::checkFirstQueuedLevel(int upperBound) {
    int oldLevel = m_firstQueuedLevel;
    m_firstQueuedLevel = upperBound;

    for (int i = oldLevel + 1; i < upperBound; ++i) {
        if (!m_queues[i].isEmpty()) {
            m_firstQueuedLevel = i;
            break;
        }
    }
}

} // namespace lighting
} // namespace level
} // namespace world
} // namespace minecraft
