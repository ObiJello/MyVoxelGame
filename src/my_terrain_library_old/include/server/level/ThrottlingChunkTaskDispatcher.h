#pragma once

#include "server/level/ChunkTaskDispatcher.h"
#include <unordered_set>
#include <cstdint>
#include <string>

// Reference: net/minecraft/server/level/ThrottlingChunkTaskDispatcher.java

namespace minecraft {
namespace server {
namespace level {

/**
 * ThrottlingChunkTaskDispatcher - A task dispatcher with execution throttling
 * Reference: ThrottlingChunkTaskDispatcher.java
 *
 * This extends ChunkTaskDispatcher to limit the number of chunks
 * that can be processed concurrently. This is used for player ticket
 * operations to prevent overwhelming the server.
 */
class ThrottlingChunkTaskDispatcher : public ChunkTaskDispatcher {
public:
    /**
     * Constructor
     * Reference: ThrottlingChunkTaskDispatcher.java lines 17-21
     *
     * @param executor The task scheduler
     * @param dispatcherExecutor The dispatcher executor
     * @param maxChunksInExecution Maximum concurrent chunks
     */
    ThrottlingChunkTaskDispatcher(
        std::shared_ptr<util::thread::TaskScheduler<std::function<void()>>> executor,
        Executor dispatcherExecutor,
        int maxChunksInExecution
    )
        : ChunkTaskDispatcher(std::move(executor), std::move(dispatcherExecutor))
        , m_maxChunksInExecution(maxChunksInExecution)
    {}

    /**
     * Get debug status string
     * Reference: ThrottlingChunkTaskDispatcher.java lines 36-39
     */
    std::string getDebugStatus() const;

protected:
    /**
     * Called when a chunk is released - removes from execution set
     * Reference: ThrottlingChunkTaskDispatcher.java lines 23-25
     */
    void onRelease(int64_t key) override;

    /**
     * Pop tasks only if under throttle limit
     * Reference: ThrottlingChunkTaskDispatcher.java lines 27-29
     */
    std::optional<ChunkTaskPriorityQueue::TasksForChunk> popTasks() override;

    /**
     * Track chunk when scheduling for execution
     * Reference: ThrottlingChunkTaskDispatcher.java lines 31-34
     */
    void scheduleForExecution(ChunkTaskPriorityQueue::TasksForChunk& tasksForChunk) override;

private:
    std::unordered_set<int64_t> m_chunkPositionsInExecution;
    int m_maxChunksInExecution;
};

} // namespace level
} // namespace server
} // namespace minecraft
