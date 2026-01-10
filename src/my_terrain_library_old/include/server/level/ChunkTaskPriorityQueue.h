#pragma once

#include "server/level/ChunkLevel.h"
#include "world/ChunkPos.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include <string>
#include <sstream>
#include <mutex>
#include <atomic>
#include <optional>

// Reference: net/minecraft/server/level/ChunkTaskPriorityQueue.java

namespace minecraft {
namespace server {
namespace level {

/**
 * ChunkTaskPriorityQueue - Multi-priority queue for chunk tasks
 * Reference: ChunkTaskPriorityQueue.java
 *
 * Tasks are organized by priority level (lower = higher priority).
 * Within a priority level, tasks are organized by chunk position.
 *
 * OPTIMIZATION: Uses per-priority mutexes for submit() to reduce contention.
 * Operations that access multiple priorities (resort, release) use global mutex.
 */
class ChunkTaskPriorityQueue {
public:
    /**
     * TasksForChunk - A chunk and its pending tasks
     * Reference: ChunkTaskPriorityQueue.java lines 93-94
     */
    struct TasksForChunk {
        int64_t chunkPos;
        std::vector<std::function<void()>> tasks;

        TasksForChunk(int64_t pos, std::vector<std::function<void()>> t)
            : chunkPos(pos), tasks(std::move(t)) {}
    };

    /**
     * Get the number of priority levels
     * Reference: ChunkTaskPriorityQueue.java lines 89-91
     */
    static int getPriorityLevelCount() {
        return ChunkLevel::getMaxLevel() + 2;
    }

    /**
     * Constructor
     * Reference: ChunkTaskPriorityQueue.java lines 16-20
     */
    explicit ChunkTaskPriorityQueue(const std::string& name)
        : m_name(name)
        , m_topPriorityQueueIndex(getPriorityLevelCount())
        , m_priorityMutexes(getPriorityLevelCount())
    {
        m_queuesPerPriority.resize(getPriorityLevelCount());
    }

    /**
     * Resort tasks for a chunk from old priority to new priority
     * Reference: ChunkTaskPriorityQueue.java lines 22-38
     * Note: Uses global mutex since it accesses multiple priority levels
     */
    void resortChunkTasks(int oldPriority, const world::ChunkPos& pos, int newPriority) {
        std::lock_guard<std::mutex> lock(m_globalMutex);
        resortChunkTasksInternal(oldPriority, pos.toLong(), newPriority);
    }

    /**
     * Submit a task for a chunk at a given priority level
     * Reference: ChunkTaskPriorityQueue.java lines 40-43
     * OPTIMIZATION: Only locks the specific priority level
     */
    void submit(std::function<void()> task, int64_t chunkPos, int level) {
        {
            std::lock_guard<std::mutex> lock(m_priorityMutexes[level]);
            m_queuesPerPriority[level][chunkPos].push_back(std::move(task));
        }
        // Update top priority atomically (lock-free)
        int currentTop = m_topPriorityQueueIndex.load(std::memory_order_relaxed);
        while (level < currentTop) {
            if (m_topPriorityQueueIndex.compare_exchange_weak(currentTop, level,
                    std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
        }
    }

    /**
     * Release tasks for a chunk position
     * Reference: ChunkTaskPriorityQueue.java lines 45-63
     * Note: Uses global mutex since it accesses multiple priority levels
     *
     * @param pos The chunk position
     * @param unschedule If true, clear all tasks; otherwise just remove empty entries
     */
    void release(int64_t pos, bool unschedule) {
        std::lock_guard<std::mutex> lock(m_globalMutex);
        releaseInternal(pos, unschedule);
    }

    /**
     * Pop the highest priority chunk's tasks
     * Reference: ChunkTaskPriorityQueue.java lines 65-79
     * OPTIMIZATION: Only locks the specific priority level being popped
     *
     * @return TasksForChunk or nullopt if no work
     */
    std::optional<TasksForChunk> pop() {
        // Fast path: check if there's work without locking
        if (!hasWork()) {
            return std::nullopt;
        }

        int index = m_topPriorityQueueIndex.load(std::memory_order_acquire);
        int priorityLevelCount = getPriorityLevelCount();

        // Try each priority level starting from current top
        while (index < priorityLevelCount) {
            std::lock_guard<std::mutex> lock(m_priorityMutexes[index]);
            auto& queue = m_queuesPerPriority[index];

            if (!queue.empty()) {
                // Get the first entry (simulating LinkedHashMap behavior)
                auto it = queue.begin();
                int64_t chunkPos = it->first;
                auto tasks = std::move(it->second);
                queue.erase(it);

                // Update top priority if this queue is now empty
                if (queue.empty()) {
                    updateTopPriorityQueueIndexFrom(index);
                }

                return TasksForChunk(chunkPos, std::move(tasks));
            }

            ++index;
        }

        // No work found, update top index
        m_topPriorityQueueIndex.store(priorityLevelCount, std::memory_order_release);
        return std::nullopt;
    }

    /**
     * Check if there is work to do
     * Reference: ChunkTaskPriorityQueue.java lines 81-83
     */
    bool hasWork() const {
        return m_topPriorityQueueIndex.load(std::memory_order_acquire) < getPriorityLevelCount();
    }

    /**
     * Get string representation
     * Reference: ChunkTaskPriorityQueue.java lines 85-87
     */
    std::string toString() const {
        std::ostringstream ss;
        ss << m_name << " " << m_topPriorityQueueIndex.load() << "...";
        return ss.str();
    }

protected:
    void resortChunkTasksInternal(int oldPriority, int64_t pos, int newPriority) {
        int priorityLevelCount = getPriorityLevelCount();

        if (oldPriority < priorityLevelCount) {
            auto& oldQueue = m_queuesPerPriority[oldPriority];
            auto it = oldQueue.find(pos);

            std::vector<std::function<void()>> oldTasks;
            if (it != oldQueue.end()) {
                oldTasks = std::move(it->second);
                oldQueue.erase(it);
            }

            // Update top priority if needed
            if (oldPriority == m_topPriorityQueueIndex.load(std::memory_order_relaxed)) {
                updateTopPriorityQueueIndexFrom(oldPriority);
            }

            // Move tasks to new priority
            if (!oldTasks.empty()) {
                auto& newQueue = m_queuesPerPriority[newPriority];
                auto& taskList = newQueue[pos];
                taskList.insert(taskList.end(),
                    std::make_move_iterator(oldTasks.begin()),
                    std::make_move_iterator(oldTasks.end()));

                int currentTop = m_topPriorityQueueIndex.load(std::memory_order_relaxed);
                if (newPriority < currentTop) {
                    m_topPriorityQueueIndex.store(newPriority, std::memory_order_release);
                }
            }
        }
    }

    void releaseInternal(int64_t pos, bool unschedule) {
        for (auto& queue : m_queuesPerPriority) {
            auto it = queue.find(pos);
            if (it != queue.end()) {
                if (unschedule) {
                    it->second.clear();
                }
                if (it->second.empty()) {
                    queue.erase(it);
                }
            }
        }

        updateTopPriorityQueueIndexFrom(0);
    }

    void updateTopPriorityQueueIndexFrom(int startIndex) {
        int priorityLevelCount = getPriorityLevelCount();
        int currentTop = startIndex;

        while (currentTop < priorityLevelCount &&
               m_queuesPerPriority[currentTop].empty()) {
            ++currentTop;
        }

        m_topPriorityQueueIndex.store(currentTop, std::memory_order_release);
    }

private:
    std::string m_name;
    std::atomic<int> m_topPriorityQueueIndex;
    std::vector<std::unordered_map<int64_t, std::vector<std::function<void()>>>> m_queuesPerPriority;
    std::vector<std::mutex> m_priorityMutexes;  // Per-priority mutexes for submit/pop
    std::mutex m_globalMutex;  // For operations that touch multiple priorities
};

} // namespace level
} // namespace server
} // namespace minecraft
