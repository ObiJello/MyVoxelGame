#pragma once

#include "server/level/ChunkTaskPriorityQueue.h"
#include "util/thread/TaskScheduler.h"
#include "util/thread/PriorityConsecutiveExecutor.h"
#include "util/thread/StrictQueue.h"
#include "util/CompletableFuture.h"
#include "world/ChunkPos.h"
#include <memory>
#include <functional>
#include <atomic>

// Reference: net/minecraft/server/level/ChunkTaskDispatcher.java

namespace minecraft {
namespace server {
namespace level {

/**
 * ChunkTaskDispatcher - Dispatches chunk tasks with priority ordering
 * Reference: ChunkTaskDispatcher.java
 *
 * This class manages the scheduling and execution of chunk tasks.
 * It uses a priority queue to order tasks and a dispatcher to execute them.
 *
 * The 4 dispatcher priority levels are:
 * - 0: Level changes (highest priority)
 * - 1: Release operations
 * - 2: Task submission
 * - 3: Task polling (lowest priority)
 */
class ChunkTaskDispatcher {
public:
    /**
     * Number of priority levels in the dispatcher
     * Reference: ChunkTaskDispatcher.java line 18
     */
    static constexpr int DISPATCHER_PRIORITY_COUNT = 4;

    using Executor = std::function<void(std::function<void()>)>;

    /**
     * Constructor
     * Reference: ChunkTaskDispatcher.java lines 25-30
     *
     * @param executor The task scheduler to execute work on
     * @param dispatcherExecutor The executor for the dispatcher
     */
    ChunkTaskDispatcher(
        std::shared_ptr<util::thread::TaskScheduler<std::function<void()>>> executor,
        Executor dispatcherExecutor
    )
        : m_queue(executor->name() + "_queue")
        , m_executor(std::move(executor))
        , m_dispatcher(std::make_unique<util::thread::PriorityConsecutiveExecutor>(
            DISPATCHER_PRIORITY_COUNT, std::move(dispatcherExecutor), "dispatcher"))
        , m_sleeping(true)
    {}

    /**
     * Check if there is work to do
     * Reference: ChunkTaskDispatcher.java lines 32-34
     */
    bool hasWork() const {
        return m_dispatcher->hasWork() || m_queue.hasWork();
    }

    /**
     * Handle a level change for a chunk
     * Reference: ChunkTaskDispatcher.java lines 36-46
     *
     * @param pos The chunk position
     * @param oldLevel Supplier for the old ticket level
     * @param newLevel The new ticket level
     * @param setQueueLevel Consumer to set the queue level
     */
    void onLevelChange(
        const world::ChunkPos& pos,
        std::function<int()> oldLevel,
        int newLevel,
        std::function<void(int)> setQueueLevel
    ) {
        m_dispatcher->schedule(0, [this, pos, oldLevel, newLevel, setQueueLevel]() {
            int oldTicketLevel = oldLevel();
            // Debug logging would go here
            m_queue.resortChunkTasks(oldTicketLevel, pos, newLevel);
            setQueueLevel(newLevel);
        });
    }

    /**
     * Release a chunk position from the queue
     * Reference: ChunkTaskDispatcher.java lines 48-59
     *
     * @param pos The chunk position (as long)
     * @param whenReleased Callback to run after release
     * @param clearQueue If true, clear all pending tasks for this chunk
     */
    void release(int64_t pos, std::function<void()> whenReleased, bool clearQueue) {
        m_dispatcher->schedule(1, [this, pos, whenReleased, clearQueue]() {
            m_queue.release(pos, clearQueue);
            onRelease(pos);
            if (m_sleeping.load(std::memory_order_acquire)) {
                m_sleeping.store(false, std::memory_order_release);
                pollTask();
            }
            whenReleased();
        });
    }

    /**
     * Submit a task for a chunk
     * Reference: ChunkTaskDispatcher.java lines 61-75
     *
     * @param task The task to execute
     * @param pos The chunk position (as long)
     * @param level Supplier for the ticket level
     */
    void submit(std::function<void()> task, int64_t pos, std::function<int()> level) {
        m_dispatcher->schedule(2, [this, task = std::move(task), pos, level]() {
            int ticketLevel = level();
            // Debug logging would go here
            m_queue.submit(task, pos, ticketLevel);
            if (m_sleeping.load(std::memory_order_acquire)) {
                m_sleeping.store(false, std::memory_order_release);
                pollTask();
            }
        });
    }

    /**
     * Close the dispatcher
     * Reference: ChunkTaskDispatcher.java lines 103-105
     */
    void close() {
        m_executor->close();
    }

protected:
    /**
     * Poll for the next task to execute
     * Reference: ChunkTaskDispatcher.java lines 77-87
     *
     * CRITICAL: pollTask() MUST schedule to dispatcher at priority 3!
     *
     * Java pattern:
     *   protected void pollTask() {
     *      this.dispatcher.schedule(new StrictQueue.RunnableWithPriority(3, () -> {
     *         TasksForChunk task = this.popTasks();
     *         if (task == null) {
     *            this.sleeping = true;
     *         } else {
     *            this.scheduleForExecution(task);
     *         }
     *      }));
     *   }
     *
     * The priority ordering is crucial for parallelism:
     * - Priority 0: Level changes (highest)
     * - Priority 1: Release operations
     * - Priority 2: Task submissions
     * - Priority 3: Task polling (lowest)
     *
     * This means ALL pending submits (priority 2) execute before ANY polls (priority 3).
     * This enables batching: many tasks can queue up before any are polled for execution,
     * allowing the ConsecutiveExecutor to batch work efficiently.
     */
    virtual void pollTask() {
        // MUST schedule to dispatcher at priority 3 (lowest) - matches Java exactly!
        m_dispatcher->schedule(3, [this]() {
            auto tasksForChunk = popTasks();
            if (!tasksForChunk.has_value()) {
                // Queue is empty - go to sleep
                m_sleeping.store(true, std::memory_order_release);
            } else {
                // Schedule tasks for execution
                scheduleForExecution(tasksForChunk.value());
            }
        });
    }

    /**
     * Schedule tasks for execution
     * Reference: ChunkTaskDispatcher.java lines 89-94
     */
    virtual void scheduleForExecution(ChunkTaskPriorityQueue::TasksForChunk& tasksForChunk) {
        // Execute all tasks and poll again when all complete
        std::vector<std::shared_ptr<util::CompletableFuture<void>>> futures;
        futures.reserve(tasksForChunk.tasks.size());

        for (auto& task : tasksForChunk.tasks) {
            auto future = m_executor->template scheduleWithResult<void>(
                [task = std::move(task)](std::shared_ptr<util::CompletableFuture<void>> f) {
                    task();
                    f->complete();
                }
            );
            futures.push_back(std::move(future));
        }

        // When all tasks complete, poll for more
        auto allFutures = util::CompletableFuture<void>::allOf(std::move(futures));
        allFutures->thenRun([this]() {
            pollTask();
        });
    }

    /**
     * Called when a chunk is released
     * Reference: ChunkTaskDispatcher.java lines 96-97
     */
    virtual void onRelease(int64_t key) {
        // Default: no-op. Subclasses can override.
    }

    /**
     * Pop tasks from the queue
     * Reference: ChunkTaskDispatcher.java lines 99-101
     */
    virtual std::optional<ChunkTaskPriorityQueue::TasksForChunk> popTasks() {
        return m_queue.pop();
    }

private:
    ChunkTaskPriorityQueue m_queue;
    std::shared_ptr<util::thread::TaskScheduler<std::function<void()>>> m_executor;
    std::unique_ptr<util::thread::PriorityConsecutiveExecutor> m_dispatcher;
    std::atomic<bool> m_sleeping;
};

} // namespace level
} // namespace server
} // namespace minecraft
