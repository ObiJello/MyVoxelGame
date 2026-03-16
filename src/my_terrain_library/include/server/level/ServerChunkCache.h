#pragma once

#include "server/level/ChunkMap.h"
#include "server/level/DistanceManager.h"
#include "server/level/ChunkResult.h"
#include "server/level/ChunkLevel.h"
#include "server/level/Ticket.h"
#include "server/level/TicketType.h"
#include "world/level/TicketStorage.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/ChunkPos.h"
#include "util/CompletableFuture.h"
#include <functional>
#include <memory>
#include <thread>
#include <array>

// Reference: net/minecraft/server/level/ServerChunkCache.java

namespace minecraft {
namespace server {
namespace level {

/**
 * ServerChunkCache - Main entry point for chunk loading and generation
 * Reference: ServerChunkCache.java
 *
 * This class provides the public API for:
 * - Getting chunks at various generation statuses
 * - Managing the chunk loading/generation pipeline
 * - Coordinating ticking of loaded chunks
 */
class ServerChunkCache {
public:
    using ChunkAccess = ::world::IChunk;
    using ChunkResultType = std::shared_ptr<ChunkResult<ChunkAccess*>>;
    using FutureType = std::shared_ptr<util::CompletableFuture<ChunkResultType>>;
    using Executor = std::function<void(std::function<void()>)>;

    static constexpr int CACHE_SIZE = 4;

    /**
     * Constructor (minimal - no chunk creation support)
     * Reference: ServerChunkCache.java lines 78-96
     */
    ServerChunkCache(
        levelgen::ChunkGenerator* generator,
        levelgen::RandomState* randomState,
        int64_t seed,
        Executor mainThreadExecutor
    );

    /**
     * Constructor with world parameters (full chunk creation support)
     * This constructor provides all parameters needed for the ChunkMap
     * to create new chunks during generation.
     *
     * @param backgroundExecutor Background thread pool (like Util.backgroundExecutor())
     * @param mainThreadExecutor Main thread executor for main-thread-only tasks
     */
    ServerChunkCache(
        levelgen::ChunkGenerator* generator,
        levelgen::RandomState* randomState,
        int64_t seed,
        Executor backgroundExecutor,
        Executor mainThreadExecutor,
        world::BlockRegistry* blockRegistry,
        BlockState* airBlock,
        BlockState* defaultBlock,
        int32_t minY = -64,
        int32_t worldHeight = 384
    );

    ~ServerChunkCache();

    /**
     * Get a chunk, optionally loading/generating it
     * Reference: ServerChunkCache.java lines 119-150
     *
     * @param x Chunk X coordinate
     * @param z Chunk Z coordinate
     * @param targetStatus The minimum status the chunk must be at
     * @param loadOrGenerate If true, load/generate if not present
     * @return The chunk, or nullptr if not available
     */
    ChunkAccess* getChunk(int x, int z, const world::chunk::status::ChunkStatus& targetStatus,
                          bool loadOrGenerate);

    /**
     * Get a chunk future
     * Reference: ServerChunkCache.java lines 189-202
     */
    FutureType getChunkFuture(int x, int z, const world::chunk::status::ChunkStatus& targetStatus,
                              bool loadOrGenerate);

    /**
     * Get a chunk immediately if present (non-blocking)
     * Reference: ServerChunkCache.java lines 152-181
     */
    ChunkAccess* getChunkNow(int x, int z);

    /**
     * Check if a chunk exists at FULL status
     * Reference: ServerChunkCache.java lines 230-234
     */
    bool hasChunk(int x, int z);

    /**
     * Tick the chunk cache
     * Reference: ServerChunkCache.java lines 283-300
     */
    void tick(std::function<bool()> haveTime, bool tickChunks);

    /**
     * Run distance manager updates
     * Reference: ServerChunkCache.java lines 250-260
     */
    bool runDistanceManagerUpdates();

    /**
     * Add a ticket
     * Reference: ServerChunkCache.java lines 448-450
     */
    void addTicket(const Ticket& ticket, const world::ChunkPos& pos);

    /**
     * Add a ticket with radius
     * Reference: ServerChunkCache.java lines 466-468
     */
    void addTicketWithRadius(const TicketType& type, const world::ChunkPos& pos, int radius);

    /**
     * Remove a ticket with radius
     * Reference: ServerChunkCache.java lines 470-472
     */
    void removeTicketWithRadius(const TicketType& type, const world::ChunkPos& pos, int radius);

    /**
     * Get the chunk map
     */
    ChunkMap& getChunkMap() { return m_chunkMap; }

    /**
     * Get the distance manager
     */
    DistanceManager& getDistanceManager() { return m_chunkMap.getDistanceManager(); }

    /**
     * Get the ticket storage
     */
    world::level::TicketStorage& getTicketStorage() { return m_ticketStorage; }

    /**
     * Get the generator
     */
    levelgen::ChunkGenerator* getGenerator() const;

    /**
     * Get the number of loaded chunks
     */
    int getLoadedChunksCount() const;

    /**
     * Set a callback to poll main thread tasks during blocking waits
     * This implements Java's managedBlock behavior where tasks are polled
     * while waiting for chunk futures to complete.
     * @param poller A function that runs pending main thread tasks
     */
    void setTaskPoller(std::function<void()> poller) {
        m_taskPoller = std::move(poller);
    }

    /**
     * Signal the chunk cache to abort all blocking getChunk() loops.
     * Called during shutdown to prevent worker threads from hanging forever.
     */
    void requestAbort() { m_abort.store(true, std::memory_order_release); }
    bool isAbortRequested() const { return m_abort.load(std::memory_order_acquire); }

private:
    /**
     * Get chunk future on main thread
     * Reference: ServerChunkCache.java lines 204-224
     */
    FutureType getChunkFutureMainThread(int x, int z,
                                         const world::chunk::status::ChunkStatus& targetStatus,
                                         bool loadOrGenerate);

    /**
     * Store a chunk in the cache
     * Reference: ServerChunkCache.java lines 107-117
     */
    void storeInCache(int64_t pos, ChunkAccess* chunk,
                      const world::chunk::status::ChunkStatus* status);

    /**
     * Clear the cache
     * Reference: ServerChunkCache.java lines 183-187
     */
    void clearCache();

    /**
     * Check if chunk holder is absent or has wrong level
     * Reference: ServerChunkCache.java lines 226-228
     */
    bool chunkAbsent(ChunkHolder* holder, int targetTicketLevel) const;

    /**
     * Get visible chunk if present
     * Reference: ServerChunkCache.java lines 103-105
     */
    ChunkHolder* getVisibleChunkIfPresent(int64_t key);

    // Components
    world::level::TicketStorage m_ticketStorage;
    ChunkMap m_chunkMap;

    // Cache (4-entry LRU)
    std::array<int64_t, CACHE_SIZE> m_lastChunkPos;
    std::array<const world::chunk::status::ChunkStatus*, CACHE_SIZE> m_lastChunkStatus;
    std::array<ChunkAccess*, CACHE_SIZE> m_lastChunk;

    // Thread info
    std::thread::id m_mainThreadId;
    Executor m_mainThreadExecutor;

    // Task poller for managedBlock (polls main thread tasks while waiting)
    std::function<void()> m_taskPoller;

    // Abort flag for clean shutdown — breaks blocking getChunk() loops
    std::atomic<bool> m_abort{false};
};

} // namespace level
} // namespace server
} // namespace minecraft
