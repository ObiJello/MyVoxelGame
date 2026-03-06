#include "server/level/ServerChunkCache.h"
#include "server/level/GenerationChunkHolder.h"
#include <chrono>
#include <thread>

// Reference: net/minecraft/server/level/ServerChunkCache.java

namespace minecraft {
namespace server {
namespace level {

using namespace world::chunk::status;

// Reference: ServerChunkCache.java lines 78-96
// Simple constructor - uses main thread executor for both (not ideal, use full constructor)
ServerChunkCache::ServerChunkCache(
    levelgen::ChunkGenerator* generator,
    levelgen::RandomState* randomState,
    int64_t seed,
    Executor mainThreadExecutor)
    : m_chunkMap(generator, randomState, seed, m_ticketStorage,
                 mainThreadExecutor, mainThreadExecutor)  // Use same executor for both
    , m_mainThreadId(std::this_thread::get_id())
    , m_mainThreadExecutor(std::move(mainThreadExecutor))
{
    clearCache();
}

// Constructor with world parameters for full chunk creation support
ServerChunkCache::ServerChunkCache(
    levelgen::ChunkGenerator* generator,
    levelgen::RandomState* randomState,
    int64_t seed,
    Executor backgroundExecutor,
    Executor mainThreadExecutor,
    world::BlockRegistry* blockRegistry,
    BlockState* airBlock,
    BlockState* defaultBlock,
    int32_t minY,
    int32_t worldHeight)
    : m_chunkMap(generator, randomState, seed, m_ticketStorage,
                 backgroundExecutor, mainThreadExecutor,
                 "", "world", "overworld",  // storage path, levelId, dimension
                 blockRegistry, airBlock, defaultBlock, minY, worldHeight)
    , m_mainThreadId(std::this_thread::get_id())
    , m_mainThreadExecutor(std::move(mainThreadExecutor))
{
    clearCache();
}

ServerChunkCache::~ServerChunkCache() = default;

// Reference: ServerChunkCache.java lines 119-150
ServerChunkCache::ChunkAccess* ServerChunkCache::getChunk(
    int x, int z, const ChunkStatus& targetStatus, bool loadOrGenerate)
{
    // If not on main thread, dispatch and wait
    if (std::this_thread::get_id() != m_mainThreadId) {
        // Note: In full implementation, would use managedBlock
        // For now, simple synchronous call
        auto future = getChunkFuture(x, z, targetStatus, loadOrGenerate);
        auto result = future->join();
        return result ? result->orElse(nullptr) : nullptr;
    }

    // Check cache
    int64_t pos = world::ChunkPos::asLong(x, z);
    for (int i = 0; i < CACHE_SIZE; ++i) {
        if (pos == m_lastChunkPos[i] && &targetStatus == m_lastChunkStatus[i]) {
            ChunkAccess* chunk = m_lastChunk[i];
            if (chunk != nullptr || !loadOrGenerate) {
                return chunk;
            }
        }
    }

    // Get chunk future
    auto future = getChunkFutureMainThread(x, z, targetStatus, loadOrGenerate);

    // Block until done, but pump events while waiting (like Java's managedBlock)
    // Reference: BlockableEventLoop.java lines 131-148 - managedBlock() and waitForTasks()
    // Reference: ServerChunkCache.java lines 583-589 - MainThreadExecutor.pollTask()
    //
    // Java's pattern:
    //   while(!condition.getAsBoolean()) {
    //       if (!this.pollTask()) {
    //           this.waitForTasks();  // Thread.yield() + parkNanos(100000L)
    //       }
    //   }
    ChunkResultType result = nullptr;
    while (!future->isDone()) {
        // Poll task: run distance manager updates and generation tasks
        // Reference: ServerChunkCache.MainThreadExecutor.pollTask() returns true if work done
        bool didWork = runDistanceManagerUpdates();

        // Run any pending main thread tasks (like Java's managedBlock)
        if (m_taskPoller) {
            m_taskPoller();
            didWork = true;  // Assume poller may have done work
        }

        // Reference: BlockableEventLoop.waitForTasks() - only wait if NO work was done
        // Java uses: Thread.yield(); LockSupport.parkNanos(100000L);  // 100 microseconds
        if (!didWork) {
            std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    result = future->getNow(nullptr);

    ChunkAccess* chunk = result ? result->orElse(nullptr) : nullptr;
    if (chunk == nullptr && loadOrGenerate) {
        // Error - chunk should be present
        return nullptr;
    }

    storeInCache(pos, chunk, &targetStatus);
    return chunk;
}

// Reference: ServerChunkCache.java lines 189-202
ServerChunkCache::FutureType ServerChunkCache::getChunkFuture(
    int x, int z, const ChunkStatus& targetStatus, bool loadOrGenerate)
{
    bool isMainThread = (std::this_thread::get_id() == m_mainThreadId);

    if (isMainThread) {
        auto future = getChunkFutureMainThread(x, z, targetStatus, loadOrGenerate);
        // Note: In full implementation, would managedBlock here
        return future;
    } else {
        // Dispatch to main thread
        auto resultFuture = std::make_shared<util::CompletableFuture<ChunkResultType>>();
        m_mainThreadExecutor([this, x, z, &targetStatus, loadOrGenerate, resultFuture]() {
            auto future = getChunkFutureMainThread(x, z, targetStatus, loadOrGenerate);
            future->thenAccept([resultFuture](ChunkResultType result) {
                resultFuture->complete(result);
            });
        });
        return resultFuture;
    }
}

// Reference: ServerChunkCache.java lines 204-224
ServerChunkCache::FutureType ServerChunkCache::getChunkFutureMainThread(
    int x, int z, const ChunkStatus& targetStatus, bool loadOrGenerate)
{
    world::ChunkPos pos(x, z);
    int64_t key = pos.toLong();
    int targetTicketLevel = ChunkLevel::byStatus(targetStatus);

    ChunkHolder* chunkHolder = getVisibleChunkIfPresent(key);

    if (loadOrGenerate) {
        // Add a ticket to ensure the chunk stays loaded
        addTicket(Ticket(TicketType::UNKNOWN, targetTicketLevel), pos);

        if (chunkAbsent(chunkHolder, targetTicketLevel)) {
            // Run updates to create the holder
            runDistanceManagerUpdates();
            chunkHolder = getVisibleChunkIfPresent(key);

            if (chunkAbsent(chunkHolder, targetTicketLevel)) {
                // Error - holder should exist after adding ticket
                auto future = std::make_shared<util::CompletableFuture<ChunkResultType>>();
                future->complete(nullptr);
                return future;
            }
        }
    }

    if (chunkAbsent(chunkHolder, targetTicketLevel)) {
        return GenerationChunkHolder::UNLOADED_CHUNK_FUTURE;
    }

    return chunkHolder->scheduleChunkGenerationTask(targetStatus, m_chunkMap);
}

// Reference: ServerChunkCache.java lines 152-181
ServerChunkCache::ChunkAccess* ServerChunkCache::getChunkNow(int x, int z) {
    if (std::this_thread::get_id() != m_mainThreadId) {
        return nullptr;
    }

    int64_t pos = world::ChunkPos::asLong(x, z);

    // Check cache for FULL status
    for (int i = 0; i < CACHE_SIZE; ++i) {
        if (pos == m_lastChunkPos[i] && m_lastChunkStatus[i] == &ChunkStatus::FULL) {
            return m_lastChunk[i];
        }
    }

    ChunkHolder* holder = getVisibleChunkIfPresent(pos);
    if (holder == nullptr) {
        return nullptr;
    }

    ChunkAccess* chunk = holder->getChunkIfPresent(ChunkStatus::FULL);
    if (chunk != nullptr) {
        storeInCache(pos, chunk, &ChunkStatus::FULL);
    }
    return chunk;
}

// Reference: ServerChunkCache.java lines 230-234
bool ServerChunkCache::hasChunk(int x, int z) {
    ChunkHolder* holder = getVisibleChunkIfPresent(world::ChunkPos::asLong(x, z));
    int targetTicketLevel = ChunkLevel::byStatus(ChunkStatus::FULL);
    return !chunkAbsent(holder, targetTicketLevel);
}

// Reference: ServerChunkCache.java lines 283-300
void ServerChunkCache::tick(std::function<bool()> /*haveTime*/, bool tickChunks) {
    // Purge stale tickets
    m_ticketStorage.purgeStaleTickets(m_chunkMap);

    // Run distance manager updates
    runDistanceManagerUpdates();

    if (tickChunks) {
        // TODO: Tick chunks, tick entities, etc.
        m_chunkMap.promoteChunkMap();
    }

    clearCache();
}

// Reference: ServerChunkCache.java lines 250-260
bool ServerChunkCache::runDistanceManagerUpdates() {
    bool updated = m_chunkMap.getDistanceManager().runAllUpdates(m_chunkMap);
    bool promoted = m_chunkMap.promoteChunkMap();
    m_chunkMap.runGenerationTasks();

    if (!updated && !promoted) {
        return false;
    }

    clearCache();
    return true;
}

// Reference: ServerChunkCache.java lines 448-450
void ServerChunkCache::addTicket(const Ticket& ticket, const world::ChunkPos& pos) {
    m_ticketStorage.addTicket(ticket, pos);
}

// Reference: ServerChunkCache.java lines 466-468
void ServerChunkCache::addTicketWithRadius(const TicketType& type,
                                            const world::ChunkPos& pos, int radius) {
    m_ticketStorage.addTicketWithRadius(type, pos, radius);
}

// Reference: ServerChunkCache.java lines 470-472
void ServerChunkCache::removeTicketWithRadius(const TicketType& type,
                                               const world::ChunkPos& pos, int radius) {
    m_ticketStorage.removeTicketWithRadius(type, pos, radius);
}

levelgen::ChunkGenerator* ServerChunkCache::getGenerator() const {
    return m_chunkMap.getWorldGenContext().generator;
}

int ServerChunkCache::getLoadedChunksCount() const {
    return static_cast<int>(m_chunkMap.size());
}

// Reference: ServerChunkCache.java lines 107-117
void ServerChunkCache::storeInCache(int64_t pos, ChunkAccess* chunk,
                                     const ChunkStatus* status) {
    // Shift cache entries
    for (int i = CACHE_SIZE - 1; i > 0; --i) {
        m_lastChunkPos[i] = m_lastChunkPos[i - 1];
        m_lastChunkStatus[i] = m_lastChunkStatus[i - 1];
        m_lastChunk[i] = m_lastChunk[i - 1];
    }

    // Store new entry
    m_lastChunkPos[0] = pos;
    m_lastChunkStatus[0] = status;
    m_lastChunk[0] = chunk;
}

// Reference: ServerChunkCache.java lines 183-187
void ServerChunkCache::clearCache() {
    for (int i = 0; i < CACHE_SIZE; ++i) {
        m_lastChunkPos[i] = world::ChunkPos::INVALID_CHUNK_POS;
        m_lastChunkStatus[i] = nullptr;
        m_lastChunk[i] = nullptr;
    }
}

// Reference: ServerChunkCache.java lines 226-228
bool ServerChunkCache::chunkAbsent(ChunkHolder* holder, int targetTicketLevel) const {
    return holder == nullptr || holder->getTicketLevel() > targetTicketLevel;
}

// Reference: ServerChunkCache.java lines 103-105
ChunkHolder* ServerChunkCache::getVisibleChunkIfPresent(int64_t key) {
    return m_chunkMap.getVisibleChunkIfPresent(key);
}

} // namespace level
} // namespace server
} // namespace minecraft
