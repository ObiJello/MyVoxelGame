#pragma once

#include <queue>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>
#include <stdexcept>
#include <sstream>

// Reference: net/minecraft/util/thread/StrictQueue.java

namespace minecraft {
namespace util {
namespace thread {

/**
 * StrictQueue - Interface for thread-safe task queues
 * Reference: StrictQueue.java
 *
 * @tparam T The type of runnable stored (must be callable)
 */
template<typename T>
class StrictQueue {
public:
    virtual ~StrictQueue() = default;

    /**
     * Pop and return the next task, or nullptr if empty
     * Reference: StrictQueue.java line 10
     */
    virtual std::function<void()> pop() = 0;

    /**
     * Push a task onto the queue
     * Reference: StrictQueue.java line 12
     */
    virtual bool push(T task) = 0;

    /**
     * Check if the queue is empty
     * Reference: StrictQueue.java line 14
     */
    virtual bool isEmpty() const = 0;

    /**
     * Get the number of tasks in the queue
     * Reference: StrictQueue.java line 16
     */
    virtual int size() const = 0;
};

/**
 * QueueStrictQueue - Simple queue-backed StrictQueue
 * Reference: StrictQueue.java lines 18-40
 *
 * OPTIMIZATION: Uses atomic size tracking to allow lock-free isEmpty() and size() checks.
 * This matches Java's ConcurrentLinkedQueue pattern where size is tracked atomically.
 */
class QueueStrictQueue : public StrictQueue<std::function<void()>> {
public:
    QueueStrictQueue() : m_size(0) {}

    std::function<void()> pop() override {
        // Fast path: if size is 0, don't even try to lock
        if (m_size.load(std::memory_order_acquire) == 0) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return nullptr;
        }
        auto task = std::move(m_queue.front());
        m_queue.pop();
        m_size.fetch_sub(1, std::memory_order_relaxed);
        return task;
    }

    bool push(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(task));
        m_size.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool isEmpty() const override {
        // Lock-free check using atomic size
        return m_size.load(std::memory_order_acquire) == 0;
    }

    int size() const override {
        // Lock-free check using atomic size
        return m_size.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex m_mutex;
    std::queue<std::function<void()>> m_queue;
    std::atomic<int> m_size;
};

/**
 * RunnableWithPriority - A runnable with an associated priority
 * Reference: StrictQueue.java lines 42-46
 */
struct RunnableWithPriority {
    int priority;
    std::function<void()> task;

    RunnableWithPriority(int p, std::function<void()> t)
        : priority(p), task(std::move(t)) {}

    void operator()() const {
        if (task) {
            task();
        }
    }

    void run() const {
        if (task) {
            task();
        }
    }
};

/**
 * FixedPriorityQueue - Multi-level priority queue
 * Reference: StrictQueue.java lines 48-91
 *
 * Lower priority number = higher priority (processed first)
 *
 * OPTIMIZATION: Uses per-priority mutexes to reduce contention.
 * Push operations to different priorities don't block each other.
 * This matches Java's pattern of using separate ConcurrentLinkedQueue per priority.
 */
class FixedPriorityQueue : public StrictQueue<RunnableWithPriority> {
public:
    explicit FixedPriorityQueue(int priorityCount)
        : m_queueMutexes(priorityCount)
        , m_queues(priorityCount)
        , m_size(0)
    {}

    std::function<void()> pop() override {
        // Fast path: if size is 0, don't try to lock anything
        if (m_size.load(std::memory_order_acquire) == 0) {
            return nullptr;
        }

        // Try each priority level with its own mutex
        for (size_t i = 0; i < m_queues.size(); ++i) {
            std::lock_guard<std::mutex> lock(m_queueMutexes[i]);
            if (!m_queues[i].empty()) {
                auto task = std::move(m_queues[i].front().task);
                m_queues[i].pop();
                m_size.fetch_sub(1, std::memory_order_relaxed);
                return task;
            }
        }
        return nullptr;
    }

    bool push(RunnableWithPriority task) override {
        int priority = task.priority;

        if (priority < 0 || priority >= static_cast<int>(m_queues.size())) {
            std::ostringstream ss;
            ss << "Priority " << priority << " not supported. Expected range [0-"
               << (m_queues.size() - 1) << "]";
            throw std::out_of_range(ss.str());
        }

        // Only lock the specific priority's mutex
        std::lock_guard<std::mutex> lock(m_queueMutexes[priority]);
        m_queues[priority].push(std::move(task));
        m_size.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool isEmpty() const override {
        return m_size.load(std::memory_order_acquire) == 0;
    }

    int size() const override {
        return m_size.load(std::memory_order_acquire);
    }

    /**
     * Get the number of priority levels
     */
    int priorityCount() const {
        return static_cast<int>(m_queues.size());
    }

private:
    std::vector<std::mutex> m_queueMutexes;  // Per-priority mutexes
    std::vector<std::queue<RunnableWithPriority>> m_queues;
    std::atomic<int> m_size;
};

} // namespace thread
} // namespace util
} // namespace minecraft
