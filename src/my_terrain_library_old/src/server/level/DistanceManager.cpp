#include "server/level/DistanceManager.h"
#include "server/level/LoadingChunkTracker.h"
#include "server/level/ChunkMap.h"
#include "server/level/Ticket.h"
#include "server/level/TicketType.h"
#include <limits>
#include <iostream>

// Reference: net/minecraft/server/level/DistanceManager.java

namespace minecraft {
namespace server {
namespace level {

// ============================================================================
// DistanceManager
// ============================================================================

// Reference: DistanceManager.java lines 51-64
DistanceManager::DistanceManager(
    Executor mainThreadExecutor,
    world::level::TicketStorage& ticketStorage)
    : m_simulationChunkTracker(ticketStorage)
    , m_naturalSpawnChunkCounter(NATURAL_SPAWN_MAX_DISTANCE)
    , m_playerTicketManager(PLAYER_TICKET_MAX_DISTANCE, ticketStorage)
    , m_mainThreadExecutor(std::move(mainThreadExecutor))
    , m_ticketStorage(ticketStorage)
{
    // Create LoadingChunkTracker after m_ticketStorage is set
    m_loadingChunkTracker = std::make_unique<LoadingChunkTracker>(*this, ticketStorage);

    // Create the throttling dispatcher
    // Note: In full implementation, this would use a real TaskScheduler
    // For now, use a placeholder
    auto scheduler = std::make_shared<util::thread::ExecutorTaskScheduler>(
        "ticket_dispatcher",
        [this](std::function<void()> task) {
            m_mainThreadExecutor(std::move(task));
        }
    );
    m_ticketDispatcher = std::make_unique<ThrottlingChunkTaskDispatcher>(
        scheduler,
        m_mainThreadExecutor,
        4  // maxChunksInExecution
    );
}

DistanceManager::~DistanceManager() = default;

// Reference: DistanceManager.java lines 66-98
bool DistanceManager::runAllUpdates(ChunkMap& scheduler) {
    m_naturalSpawnChunkCounter.runAllUpdates();
    m_simulationChunkTracker.runAllUpdates();
    m_playerTicketManager.runAllUpdates();

    int updates = std::numeric_limits<int>::max() -
                  m_loadingChunkTracker->runDistanceUpdates(std::numeric_limits<int>::max());
    bool updated = (updates != 0);

    if (!m_chunksToUpdateFutures.empty()) {
        // First pass: update highest allowed status
        for (ChunkHolder* chunk : m_chunksToUpdateFutures) {
            chunk->updateHighestAllowedStatus(scheduler);
        }
        // Second pass: update futures
        for (ChunkHolder* chunk : m_chunksToUpdateFutures) {
            chunk->updateFutures(scheduler, m_mainThreadExecutor);
        }
        m_chunksToUpdateFutures.clear();
        return true;
    }

    // Handle tickets to release
    if (!m_ticketsToRelease.empty()) {
        for (int64_t key : m_ticketsToRelease) {
            Ticket ticket(TicketType::PLAYER_LOADING, getPlayerTicketLevel());
            m_ticketStorage.removeTicket(ticket, world::ChunkPos(key));
        }
        m_ticketsToRelease.clear();
        return true;
    }

    return updated;
}

// Reference: DistanceManager.java lines 112-137
void DistanceManager::addPlayer(int64_t chunkKey, void* player) {
    auto& players = m_playersPerChunk[chunkKey];
    bool wasEmpty = players.empty();
    players.insert(player);

    if (wasEmpty) {
        m_naturalSpawnChunkCounter.addSource(chunkKey);
        m_playerTicketManager.addSource(chunkKey);
    }
}

// Reference: DistanceManager.java lines 139-161
void DistanceManager::removePlayer(int64_t chunkKey, void* player) {
    auto it = m_playersPerChunk.find(chunkKey);
    if (it == m_playersPerChunk.end()) {
        return;
    }

    auto& players = it->second;
    players.erase(player);

    if (players.empty()) {
        m_playersPerChunk.erase(it);
        m_naturalSpawnChunkCounter.removeSource(chunkKey);
        m_playerTicketManager.removeSource(chunkKey);
    }
}

// Reference: DistanceManager.java lines 163-177
bool DistanceManager::inEntityTickingRange(int64_t key) const {
    return m_simulationChunkTracker.getLevel(world::ChunkPos(key)) <=
           SimulationChunkTracker::MAX_LEVEL - 2;
}

// Reference: DistanceManager.java lines 179-193
bool DistanceManager::inBlockTickingRange(int64_t key) const {
    return m_simulationChunkTracker.getLevel(world::ChunkPos(key)) <=
           SimulationChunkTracker::MAX_LEVEL - 1;
}

// Reference: DistanceManager.java lines 202-204
int DistanceManager::getNaturalSpawnChunkCount() const {
    return m_naturalSpawnChunkCounter.getChunkCount();
}

// Reference: DistanceManager.java lines 206-220
void DistanceManager::updateSimulationDistance(int newDistance) {
    if (newDistance != m_simulationDistance) {
        m_simulationDistance = newDistance;
        m_ticketStorage.replaceTicketLevelOfType(
            getPlayerTicketLevel() - newDistance - 1,
            TicketType::PLAYER_SIMULATION
        );
    }
}

bool DistanceManager::isChunkToRemove(int64_t node) const {
    // Check pending unloads in ChunkMap
    if (m_chunkMap != nullptr) {
        return m_chunkMap->isChunkToRemove(node);
    }
    return false;
}

ChunkHolder* DistanceManager::getChunk(int64_t node) {
    if (m_chunkMap != nullptr) {
        return m_chunkMap->getUpdatingChunkIfPresent(node);
    }
    return nullptr;
}

ChunkHolder* DistanceManager::updateChunkScheduling(
    int64_t node, int level, ChunkHolder* chunk, int oldLevel) {
    if (m_chunkMap != nullptr) {
        return m_chunkMap->updateChunkScheduling(node, level, chunk, oldLevel);
    }
    return nullptr;
}

void DistanceManager::addChunkToUpdateFutures(ChunkHolder* chunk) {
    m_chunksToUpdateFutures.insert(chunk);
}

// ============================================================================
// FixedPlayerDistanceChunkTracker
// ============================================================================

// Reference: DistanceManager.java lines 222-275
DistanceManager::FixedPlayerDistanceChunkTracker::FixedPlayerDistanceChunkTracker(int maxDistance)
    : ChunkTracker(maxDistance + 2, 16, 256)
    , m_maxDistance(maxDistance)
{
}

void DistanceManager::FixedPlayerDistanceChunkTracker::addSource(int64_t key) {
    if (m_sources.insert(key).second) {
        update(key, 0, true);
    }
}

void DistanceManager::FixedPlayerDistanceChunkTracker::removeSource(int64_t key) {
    if (m_sources.erase(key) > 0) {
        update(key, m_levelCount - 1, false);
    }
}

void DistanceManager::FixedPlayerDistanceChunkTracker::runAllUpdates() {
    runUpdates(std::numeric_limits<int>::max());
}

int DistanceManager::FixedPlayerDistanceChunkTracker::getChunkCount() const {
    return static_cast<int>(m_chunks.size());
}

int DistanceManager::FixedPlayerDistanceChunkTracker::getLevelFromSource(int64_t to) {
    return m_sources.count(to) > 0 ? 0 : m_levelCount - 1;
}

int DistanceManager::FixedPlayerDistanceChunkTracker::getLevel(int64_t node) {
    auto it = m_chunks.find(node);
    if (it != m_chunks.end()) {
        return static_cast<int>(it->second);
    }
    return m_levelCount - 1;
}

void DistanceManager::FixedPlayerDistanceChunkTracker::setLevel(int64_t node, int level) {
    int oldLevel = getLevel(node);

    if (level >= m_levelCount - 1) {
        m_chunks.erase(node);
    } else {
        m_chunks[node] = static_cast<uint8_t>(level);
    }

    // Notify subclasses of level changes
    if (oldLevel < level) {
        onLevelIncrease(world::ChunkPos(node), oldLevel, level);
    } else if (oldLevel > level) {
        onLevelDecrease(world::ChunkPos(node), level, oldLevel);
    }
}

void DistanceManager::FixedPlayerDistanceChunkTracker::onLevelIncrease(
    const world::ChunkPos& /*pos*/, int /*oldLevel*/, int /*level*/) {
    // Default: no-op
}

void DistanceManager::FixedPlayerDistanceChunkTracker::onLevelDecrease(
    const world::ChunkPos& /*pos*/, int /*level*/, int /*oldLevel*/) {
    // Default: no-op
}

// ============================================================================
// PlayerTicketTracker
// ============================================================================

// Reference: DistanceManager.java lines 277-302
DistanceManager::PlayerTicketTracker::PlayerTicketTracker(
    int maxDistance, world::level::TicketStorage& ticketStorage)
    : FixedPlayerDistanceChunkTracker(maxDistance)
    , m_ticketStorage(ticketStorage)
{
}

void DistanceManager::PlayerTicketTracker::onLevelIncrease(
    const world::ChunkPos& pos, int /*oldLevel*/, int level) {
    // When level increases (worse), remove ticket if we're at max distance
    if (level >= m_maxDistance) {
        Ticket ticket(TicketType::PLAYER_LOADING, DistanceManager::getPlayerTicketLevel());
        m_ticketStorage.removeTicket(ticket, pos);
    }
}

void DistanceManager::PlayerTicketTracker::onLevelDecrease(
    const world::ChunkPos& pos, int level, int /*oldLevel*/) {
    // When level decreases (better), add ticket if we're within max distance
    if (level < m_maxDistance) {
        Ticket ticket(TicketType::PLAYER_LOADING, DistanceManager::getPlayerTicketLevel());
        m_ticketStorage.addTicket(ticket, pos);
    }
}

} // namespace level
} // namespace server
} // namespace minecraft
