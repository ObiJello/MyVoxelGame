#pragma once

#include "server/level/ChunkTracker.h"
#include "server/level/SimulationChunkTracker.h"
#include "server/level/ThrottlingChunkTaskDispatcher.h"
#include "server/level/ChunkLevel.h"
#include "server/level/FullChunkStatus.h"
#include "server/level/ChunkHolder.h"
#include "world/level/TicketStorage.h"
#include "world/ChunkPos.h"
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <cstdint>

// Reference: net/minecraft/server/level/DistanceManager.java

// Forward declarations
namespace minecraft {
namespace server {
namespace level {
class ChunkMap;
class LoadingChunkTracker;
}
}
}

namespace minecraft {
namespace server {
namespace level {

/**
 * DistanceManager - Manages chunk loading based on ticket levels and player positions
 * Reference: DistanceManager.java
 *
 * This class coordinates:
 * - Ticket-based chunk loading
 * - Player-based chunk loading (natural spawn, entity ticking)
 * - Chunk holder lifecycle management
 */
class DistanceManager {
public:
    /**
     * Player ticket level for entity ticking
     * Reference: DistanceManager.java line 49
     */
    static int getPlayerTicketLevel() {
        return ChunkLevel::byStatus(FullChunkStatus::ENTITY_TICKING);
    }

    using Executor = std::function<void(std::function<void()>)>;

    /**
     * Constructor
     * Reference: DistanceManager.java lines 51-64
     */
    DistanceManager(
        Executor mainThreadExecutor,
        world::level::TicketStorage& ticketStorage
    );

    ~DistanceManager();

    /**
     * Run all pending updates
     * Reference: DistanceManager.java lines 66-98
     *
     * @param scheduler The chunk map/scheduler
     * @return true if any updates were performed
     */
    bool runAllUpdates(ChunkMap& scheduler);

    /**
     * Add a player to chunk tracking
     * Reference: DistanceManager.java lines 112-137
     */
    void addPlayer(int64_t chunkKey, void* player);

    /**
     * Remove a player from chunk tracking
     * Reference: DistanceManager.java lines 139-161
     */
    void removePlayer(int64_t chunkKey, void* player);

    /**
     * Check if a chunk is in entity ticking range
     * Reference: DistanceManager.java lines 163-177
     */
    bool inEntityTickingRange(int64_t key) const;

    /**
     * Check if a chunk is in block ticking range
     * Reference: DistanceManager.java lines 179-193
     */
    bool inBlockTickingRange(int64_t key) const;

    /**
     * Get the number of natural spawn chunks
     * Reference: DistanceManager.java lines 202-204
     */
    int getNaturalSpawnChunkCount() const;

    /**
     * Update simulation distance
     * Reference: DistanceManager.java lines 206-220
     */
    void updateSimulationDistance(int newDistance);

    /**
     * Get the ticket dispatcher
     * Reference: DistanceManager.java lines 199-201
     */
    ThrottlingChunkTaskDispatcher& getTicketDispatcher() { return *m_ticketDispatcher; }

    // Methods used by LoadingChunkTracker
    bool isChunkToRemove(int64_t node) const;
    ChunkHolder* getChunk(int64_t node);
    ChunkHolder* updateChunkScheduling(int64_t node, int level, ChunkHolder* chunk, int oldLevel);
    void addChunkToUpdateFutures(ChunkHolder* chunk);

    // Set the ChunkMap for callbacks
    void setChunkMap(ChunkMap* chunkMap) { m_chunkMap = chunkMap; }

private:
    /**
     * FixedPlayerDistanceChunkTracker - Tracks chunks within fixed distance of players
     * Reference: DistanceManager.java lines 222-275
     */
    class FixedPlayerDistanceChunkTracker : public ChunkTracker {
    public:
        FixedPlayerDistanceChunkTracker(int maxDistance);

        void addSource(int64_t key);
        void removeSource(int64_t key);
        void runAllUpdates();
        int getChunkCount() const;

    protected:
        int getLevelFromSource(int64_t to) override;
        int getLevel(int64_t node) override;
        void setLevel(int64_t node, int level) override;

        virtual void onLevelIncrease(const world::ChunkPos& pos, int oldLevel, int level);
        virtual void onLevelDecrease(const world::ChunkPos& pos, int level, int oldLevel);

        std::unordered_set<int64_t> m_sources;
        std::unordered_map<int64_t, uint8_t> m_chunks;
        int m_maxDistance;
    };

    /**
     * PlayerTicketTracker - Manages player-based tickets
     * Reference: DistanceManager.java lines 277-302
     */
    class PlayerTicketTracker : public FixedPlayerDistanceChunkTracker {
    public:
        PlayerTicketTracker(int maxDistance, world::level::TicketStorage& ticketStorage);

    protected:
        void onLevelIncrease(const world::ChunkPos& pos, int oldLevel, int level) override;
        void onLevelDecrease(const world::ChunkPos& pos, int level, int oldLevel) override;

    private:
        world::level::TicketStorage& m_ticketStorage;
    };

    // Components
    std::unique_ptr<LoadingChunkTracker> m_loadingChunkTracker;
    SimulationChunkTracker m_simulationChunkTracker;
    FixedPlayerDistanceChunkTracker m_naturalSpawnChunkCounter;
    PlayerTicketTracker m_playerTicketManager;
    std::unique_ptr<ThrottlingChunkTaskDispatcher> m_ticketDispatcher;

    // State
    std::unordered_map<int64_t, std::unordered_set<void*>> m_playersPerChunk;
    std::unordered_set<ChunkHolder*> m_chunksToUpdateFutures;
    std::unordered_set<int64_t> m_ticketsToRelease;
    Executor m_mainThreadExecutor;
    int m_simulationDistance = 10;

    // References
    world::level::TicketStorage& m_ticketStorage;
    ChunkMap* m_chunkMap = nullptr;

    static constexpr int NATURAL_SPAWN_MAX_DISTANCE = 8;
    static constexpr int PLAYER_TICKET_MAX_DISTANCE = 32;
};

} // namespace level
} // namespace server
} // namespace minecraft
