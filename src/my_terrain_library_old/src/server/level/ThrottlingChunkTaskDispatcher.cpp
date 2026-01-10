#include "server/level/ThrottlingChunkTaskDispatcher.h"
#include "world/ChunkPos.h"
#include <sstream>

// Reference: net/minecraft/server/level/ThrottlingChunkTaskDispatcher.java

namespace minecraft {
namespace server {
namespace level {

// Reference: ThrottlingChunkTaskDispatcher.java lines 23-25
void ThrottlingChunkTaskDispatcher::onRelease(int64_t key) {
    m_chunkPositionsInExecution.erase(key);
}

// Reference: ThrottlingChunkTaskDispatcher.java lines 27-29
std::optional<ChunkTaskPriorityQueue::TasksForChunk> ThrottlingChunkTaskDispatcher::popTasks() {
    if (static_cast<int>(m_chunkPositionsInExecution.size()) < m_maxChunksInExecution) {
        return ChunkTaskDispatcher::popTasks();
    }
    return std::nullopt;
}

// Reference: ThrottlingChunkTaskDispatcher.java lines 31-34
void ThrottlingChunkTaskDispatcher::scheduleForExecution(
    ChunkTaskPriorityQueue::TasksForChunk& tasksForChunk) {
    m_chunkPositionsInExecution.insert(tasksForChunk.chunkPos);
    ChunkTaskDispatcher::scheduleForExecution(tasksForChunk);
}

// Reference: ThrottlingChunkTaskDispatcher.java lines 36-39
std::string ThrottlingChunkTaskDispatcher::getDebugStatus() const {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (int64_t key : m_chunkPositionsInExecution) {
        if (!first) {
            oss << ",";
        }
        first = false;
        world::ChunkPos pos(key);
        oss << key << ":" << pos.toString();
    }
    oss << "]";
    return oss.str();
}

} // namespace level
} // namespace server
} // namespace minecraft
