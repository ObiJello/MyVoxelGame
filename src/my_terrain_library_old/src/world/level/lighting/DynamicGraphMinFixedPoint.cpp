#include "world/level/lighting/DynamicGraphMinFixedPoint.h"
#include <algorithm>
#include <stdexcept>
#include <vector>

// Reference: net/minecraft/world/level/lighting/DynamicGraphMinFixedPoint.java

namespace minecraft {
namespace world {
namespace level {
namespace lighting {

// Reference: DynamicGraphMinFixedPoint.java lines 19-37
DynamicGraphMinFixedPoint::DynamicGraphMinFixedPoint(int levelCount, int minQueueSize, int minMapSize)
    : m_levelCount(levelCount)
    , m_priorityQueue(levelCount, minQueueSize)
{
    if (levelCount >= 254) {
        throw std::invalid_argument("Level count must be < 254.");
    }
    m_computedLevels.reserve(minMapSize);
}

// Reference: DynamicGraphMinFixedPoint.java lines 41-48
void DynamicGraphMinFixedPoint::removeFromQueue(int64_t node) {
    auto it = m_computedLevels.find(node);
    if (it == m_computedLevels.end()) {
        return;
    }
    int computedLevel = static_cast<int>(it->second) & 255;
    m_computedLevels.erase(it);

    if (computedLevel != NO_COMPUTED_LEVEL) {
        int level = getLevel(node);
        int priority = calculatePriority(level, computedLevel);
        m_priorityQueue.dequeue(node, priority, m_levelCount);
        m_hasWork.store(!m_priorityQueue.isEmpty(), std::memory_order_release);
    }
}

// Reference: DynamicGraphMinFixedPoint.java lines 50-59
void DynamicGraphMinFixedPoint::removeIf(std::function<bool(int64_t)> predicate) {
    std::vector<int64_t> nodesToRemove;
    for (const auto& pair : m_computedLevels) {
        if (predicate(pair.first)) {
            nodesToRemove.push_back(pair.first);
        }
    }
    for (int64_t node : nodesToRemove) {
        removeFromQueue(node);
    }
}

// Reference: DynamicGraphMinFixedPoint.java lines 61-63
int DynamicGraphMinFixedPoint::calculatePriority(int level, int computedLevel) const {
    return std::min(std::min(level, computedLevel), m_levelCount - 1);
}

// Reference: DynamicGraphMinFixedPoint.java lines 66-68
void DynamicGraphMinFixedPoint::checkNode(int64_t node) {
    checkEdge(node, node, m_levelCount - 1, false);
}

// Reference: DynamicGraphMinFixedPoint.java lines 70-73
void DynamicGraphMinFixedPoint::checkEdge(int64_t from, int64_t to, int newLevelFrom, bool onlyDecreased) {
    auto it = m_computedLevels.find(to);
    int oldComputedLevel = (it != m_computedLevels.end()) ? (static_cast<int>(it->second) & 255) : NO_COMPUTED_LEVEL;
    checkEdgeInternal(from, to, newLevelFrom, getLevel(to), oldComputedLevel, onlyDecreased);
    m_hasWork.store(!m_priorityQueue.isEmpty(), std::memory_order_release);
}

// Reference: DynamicGraphMinFixedPoint.java lines 75-106
void DynamicGraphMinFixedPoint::checkEdgeInternal(int64_t from, int64_t to, int newLevelFrom,
                                                   int levelTo, int oldComputedLevel, bool onlyDecreased) {
    if (isSource(to)) {
        return;
    }

    newLevelFrom = std::clamp(newLevelFrom, 0, m_levelCount - 1);
    levelTo = std::clamp(levelTo, 0, m_levelCount - 1);

    bool wasConsistent = (oldComputedLevel == NO_COMPUTED_LEVEL);
    if (wasConsistent) {
        oldComputedLevel = levelTo;
    }

    int newComputedLevel;
    if (onlyDecreased) {
        newComputedLevel = std::min(oldComputedLevel, newLevelFrom);
    } else {
        newComputedLevel = std::clamp(getComputedLevel(to, from, newLevelFrom), 0, m_levelCount - 1);
    }

    int oldPriority = calculatePriority(levelTo, oldComputedLevel);

    if (levelTo != newComputedLevel) {
        int newPriority = calculatePriority(levelTo, newComputedLevel);
        if (oldPriority != newPriority && !wasConsistent) {
            m_priorityQueue.dequeue(to, oldPriority, newPriority);
        }
        m_priorityQueue.enqueue(to, newPriority);
        m_computedLevels[to] = static_cast<uint8_t>(newComputedLevel);
    } else if (!wasConsistent) {
        m_priorityQueue.dequeue(to, oldPriority, m_levelCount);
        m_computedLevels.erase(to);
    }
}

// Reference: DynamicGraphMinFixedPoint.java lines 108-127
void DynamicGraphMinFixedPoint::checkNeighbor(int64_t from, int64_t to, int level, bool onlyDecreased) {
    auto it = m_computedLevels.find(to);
    int storedOldComputedLevel = (it != m_computedLevels.end()) ? (static_cast<int>(it->second) & 255) : NO_COMPUTED_LEVEL;
    int levelFrom = std::clamp(computeLevelFromNeighbor(from, to, level), 0, m_levelCount - 1);

    if (onlyDecreased) {
        checkEdgeInternal(from, to, levelFrom, getLevel(to), storedOldComputedLevel, onlyDecreased);
    } else {
        bool wasConsistent = (storedOldComputedLevel == NO_COMPUTED_LEVEL);
        int oldComputedLevel;
        if (wasConsistent) {
            oldComputedLevel = std::clamp(getLevel(to), 0, m_levelCount - 1);
        } else {
            oldComputedLevel = storedOldComputedLevel;
        }

        if (levelFrom == oldComputedLevel) {
            checkEdgeInternal(from, to, m_levelCount - 1,
                             wasConsistent ? oldComputedLevel : getLevel(to),
                             storedOldComputedLevel, onlyDecreased);
        }
    }
}

// Reference: DynamicGraphMinFixedPoint.java lines 133-159
int DynamicGraphMinFixedPoint::runUpdates(int count) {
    if (m_priorityQueue.isEmpty()) {
        return count;
    }

    while (!m_priorityQueue.isEmpty() && count > 0) {
        --count;
        int64_t node = m_priorityQueue.removeFirstLong();
        int level = std::clamp(getLevel(node), 0, m_levelCount - 1);

        auto it = m_computedLevels.find(node);
        int computedLevel = (it != m_computedLevels.end()) ? (static_cast<int>(it->second) & 255) : NO_COMPUTED_LEVEL;
        m_computedLevels.erase(node);

        if (computedLevel < level) {
            setLevel(node, computedLevel);
            checkNeighborsAfterUpdate(node, computedLevel, true);
        } else if (computedLevel > level) {
            setLevel(node, m_levelCount - 1);
            if (computedLevel != m_levelCount - 1) {
                m_priorityQueue.enqueue(node, calculatePriority(m_levelCount - 1, computedLevel));
                m_computedLevels[node] = static_cast<uint8_t>(computedLevel);
            }
            checkNeighborsAfterUpdate(node, level, false);
        }
    }

    m_hasWork.store(!m_priorityQueue.isEmpty(), std::memory_order_release);
    return count;
}

} // namespace lighting
} // namespace level
} // namespace world
} // namespace minecraft
