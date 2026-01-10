#include "server/level/LoadingChunkTracker.h"
#include "server/level/DistanceManager.h"
#include "world/level/TicketStorage.h"

// Reference: net/minecraft/server/level/LoadingChunkTracker.java

namespace minecraft {
namespace server {
namespace level {

// Reference: LoadingChunkTracker.java lines 10-14
LoadingChunkTracker::LoadingChunkTracker(
    DistanceManager& distanceManager,
    world::level::TicketStorage& ticketStorage)
    : ChunkTracker(getMaxLevel() + 1, 16, 256)
    , m_distanceManager(distanceManager)
    , m_ticketStorage(ticketStorage)
{
    // Register as listener for loading ticket updates
    ticketStorage.setLoadingChunkUpdatedListener(
        [this](int64_t node, int newLevel, bool onlyDecreased) {
            this->update(node, newLevel, onlyDecreased);
        });
}

// Reference: LoadingChunkTracker.java lines 17-19
int LoadingChunkTracker::getLevelFromSource(int64_t to) {
    return m_ticketStorage.getTicketLevelAt(to, false);
}

// Reference: LoadingChunkTracker.java lines 21-30
int LoadingChunkTracker::getLevel(int64_t node) {
    if (!m_distanceManager.isChunkToRemove(node)) {
        ChunkHolder* chunk = m_distanceManager.getChunk(node);
        if (chunk != nullptr) {
            return chunk->getTicketLevel();
        }
    }
    return getMaxLevel();
}

// Reference: LoadingChunkTracker.java lines 32-42
void LoadingChunkTracker::setLevel(int64_t node, int level) {
    ChunkHolder* chunk = m_distanceManager.getChunk(node);
    int oldLevel = (chunk == nullptr) ? getMaxLevel() : chunk->getTicketLevel();

    if (oldLevel != level) {
        chunk = m_distanceManager.updateChunkScheduling(node, level, chunk, oldLevel);
        if (chunk != nullptr) {
            m_distanceManager.addChunkToUpdateFutures(chunk);
        }
    }
}

// Reference: LoadingChunkTracker.java lines 44-46
int LoadingChunkTracker::runDistanceUpdates(int count) {
    return runUpdates(count);
}

} // namespace level
} // namespace server
} // namespace minecraft
