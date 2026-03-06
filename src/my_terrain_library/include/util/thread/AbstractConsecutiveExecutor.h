#pragma once

#include "util/thread/StrictQueue.h"
#include "util/thread/TaskScheduler.h"
#include <atomic>
#include <memory>
#include <functional>
#include <string>
#include <sstream>

// Reference: net/minecraft/util/thread/AbstractConsecutiveExecutor.java

namespace minecraft {
namespace util {
namespace thread {

/**
 * AbstractConsecutiveExecutor - Base class for consecutive task execution
 * Reference: AbstractConsecutiveExecutor.java
 *
 * This executor ensures tasks are executed consecutively (one at a time)
 * even when dispatched from multiple threads.
 *
 * @tparam T The type of runnable this executor accepts
 */
template<typename T>
class AbstractConsecutiveExecutor : public TaskScheduler<T> {
public:
    using Executor = std::function<void(std::function<void()>)>;

    /**
     * Status enum for executor state
     * Reference: AbstractConsecutiveExecutor.java lines 131-140
     */
    enum class Status {
        SLEEPING,
        RUNNING,
        CLOSED
    };

    /**
     * Constructor
     * Reference: AbstractConsecutiveExecutor.java lines 23-29
     */
    AbstractConsecutiveExecutor(
        std::unique_ptr<StrictQueue<T>> queue,
        Executor executor,
        const std::string& name
    )
        : m_status(Status::SLEEPING)
        , m_queue(std::move(queue))
        , m_executor(std::move(executor))
        , m_name(name)
    {}

    virtual ~AbstractConsecutiveExecutor() = default;

    /**
     * Close the executor
     * Reference: AbstractConsecutiveExecutor.java lines 35-37
     */
    void close() override {
        m_status.store(Status::CLOSED, std::memory_order_release);
    }

    /**
     * Schedule a task
     * Reference: AbstractConsecutiveExecutor.java lines 74-77
     */
    void schedule(T task) override {
        m_queue->push(std::move(task));
        registerForExecution();
    }

    /**
     * Get the name
     * Reference: AbstractConsecutiveExecutor.java lines 107-109
     */
    std::string name() const override {
        return m_name;
    }

    /**
     * Get the queue size
     * Reference: AbstractConsecutiveExecutor.java lines 94-96
     */
    int size() const {
        return m_queue->size();
    }

    /**
     * Check if there's work to do
     * Reference: AbstractConsecutiveExecutor.java lines 98-100
     */
    bool hasWork() const {
        return isRunning() && !m_queue->isEmpty();
    }

    /**
     * Run a single task (called by the dispatch executor)
     * Reference: AbstractConsecutiveExecutor.java lines 53-61
     */
    void run() {
        try {
            pollTask();
        } catch (...) {
            // Log error but continue
        }
        setSleeping();
        registerForExecution();
    }

    /**
     * Run all pending tasks
     * Reference: AbstractConsecutiveExecutor.java lines 63-72
     */
    void runAll() {
        try {
            while (pollTask()) {
                // Continue polling
            }
        } catch (...) {
            // Log error but continue
        }
        setSleeping();
        registerForExecution();
    }

    /**
     * Get string representation
     * Reference: AbstractConsecutiveExecutor.java lines 102-105
     */
    std::string toString() const {
        std::ostringstream ss;
        ss << m_name << " " << statusToString(m_status.load()) << " " << m_queue->isEmpty();
        return ss.str();
    }

protected:
    /**
     * Check if can be scheduled for execution
     * Reference: AbstractConsecutiveExecutor.java lines 31-33
     */
    bool canBeScheduled() const {
        return !isClosed() && !m_queue->isEmpty();
    }

    /**
     * Poll and run a single task
     * Reference: AbstractConsecutiveExecutor.java lines 39-51
     */
    bool pollTask() {
        if (!isRunning()) {
            return false;
        }

        auto runnable = m_queue->pop();
        if (!runnable) {
            return false;
        }

        try {
            runnable();
        } catch (...) {
            // Log error - Util.runNamed equivalent
        }

        return true;
    }

    /**
     * Register for execution if needed
     * Reference: AbstractConsecutiveExecutor.java lines 79-92
     */
    void registerForExecution() {
        if (canBeScheduled() && setRunning()) {
            try {
                // Capture this for the lambda
                auto self = this;
                m_executor([self]() {
                    self->run();
                });
            } catch (...) {
                // Retry once
                try {
                    auto self = this;
                    m_executor([self]() {
                        self->run();
                    });
                } catch (...) {
                    // Log error: Could not schedule ConsecutiveExecutor
                }
            }
        }
    }

    /**
     * Set status to RUNNING if currently SLEEPING
     * Reference: AbstractConsecutiveExecutor.java lines 115-117
     */
    bool setRunning() {
        Status expected = Status::SLEEPING;
        return m_status.compare_exchange_strong(expected, Status::RUNNING,
            std::memory_order_acq_rel);
    }

    /**
     * Set status to SLEEPING if currently RUNNING
     * Reference: AbstractConsecutiveExecutor.java lines 119-121
     */
    void setSleeping() {
        Status expected = Status::RUNNING;
        m_status.compare_exchange_strong(expected, Status::SLEEPING,
            std::memory_order_acq_rel);
    }

    /**
     * Check if currently running
     * Reference: AbstractConsecutiveExecutor.java lines 123-125
     */
    bool isRunning() const {
        return m_status.load(std::memory_order_acquire) == Status::RUNNING;
    }

    /**
     * Check if closed
     * Reference: AbstractConsecutiveExecutor.java lines 127-129
     */
    bool isClosed() const {
        return m_status.load(std::memory_order_acquire) == Status::CLOSED;
    }

    static std::string statusToString(Status status) {
        switch (status) {
            case Status::SLEEPING: return "SLEEPING";
            case Status::RUNNING: return "RUNNING";
            case Status::CLOSED: return "CLOSED";
            default: return "UNKNOWN";
        }
    }

    std::atomic<Status> m_status;
    std::unique_ptr<StrictQueue<T>> m_queue;
    Executor m_executor;
    std::string m_name;
};

} // namespace thread
} // namespace util
} // namespace minecraft
