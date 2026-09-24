#pragma once

#include "server/level/GeneratingChunkMap.h"
#include "server/level/ChunkHolder.h"
#include "server/level/GenerationChunkHolder.h"
#include "server/level/ChunkGenerationTask.h"
#include "server/level/ChunkTaskDispatcher.h"
#include "server/level/DistanceManager.h"
#include "server/level/ChunkLevel.h"
#include "world/chunk/status/WorldGenContext.h"
#include "world/chunk/status/ChunkStatus.h"
#include "world/ChunkPos.h"
#include "world/level/chunk/storage/IOWorker.h"
#include "world/LevelChunkSection.h"
#include "world/level/block/state/BlockState.h"
#include "util/CompletableFuture.h"
#include "util/thread/ConsecutiveExecutor.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>

// Reference: net/minecraft/server/level/ChunkMap.java

namespace minecraft {
namespace server {
namespace level {

class ServerLevel;

/**
 * ChunkMap - Central coordinator for chunk management and generation
 * Reference: ChunkMap.java (~1500 lines)
 *
 * This class manages:
 * - Chunk holder lifecycle (creation, scheduling, unloading)
 * - Generation task coordination
 * - Map visibility (updating vs visible)
 * - Disk I/O for chunk loading/saving
 */
class ChunkMap : public GeneratingChunkMap {
public:
    using ChunkAccess = ::world::IChunk;
    using ChunkResultType = std::shared_ptr<ChunkResult<ChunkAccess*>>;
    using FutureType = std::shared_ptr<util::CompletableFuture<ChunkResultType>>;
    using Executor = std::function<void(std::function<void()>)>;

    /**
     * Constructor
     * Reference: ChunkMap.java lines 130-180
     *
     * @param backgroundExecutor The background thread pool (like Java's Util.backgroundExecutor())
     * @param mainThreadExecutor The main thread executor for main-thread-only tasks
     */
    ChunkMap(
        levelgen::ChunkGenerator* generator,
        levelgen::RandomState* randomState,
        int64_t seed,
        world::level::TicketStorage& ticketStorage,
        Executor backgroundExecutor,
        Executor mainThreadExecutor,
        Executor laneExecutor,   // runs the dispatcher mailbox + worldgen lane; may equal backgroundExecutor
        const std::string& storagePath = "",
        const std::string& levelId = "world",
        const std::string& dimension = "overworld",
        world::BlockRegistry* blockRegistry = nullptr,
        BlockState* airBlock = nullptr,
        BlockState* defaultBlock = nullptr,
        int32_t minY = -64,
        int32_t worldHeight = 384
    );

    ~ChunkMap();

    // =========================================================================
    // GeneratingChunkMap interface
    // =========================================================================

    /**
     * Acquire a chunk holder for generation
     * Reference: ChunkMap.java lines 342-358
     */
    GenerationChunkHolder* acquireGeneration(int64_t chunkPos) override;

    /**
     * Release a chunk holder from generation
     * Reference: ChunkMap.java lines 360-366
     */
    void releaseGeneration(GenerationChunkHolder* holder) override;

    /**
     * Apply a generation step to a chunk
     * Reference: ChunkMap.java lines 368-426
     */
    std::shared_ptr<util::CompletableFuture<ChunkAccess*>> applyStep(
        GenerationChunkHolder* chunkHolder,
        const world::chunk::status::ChunkStep& step,
        util::StaticCache2D<GenerationChunkHolder*>& cache
    ) override;

    /**
     * Schedule a generation task
     * Reference: ChunkMap.java lines 428-438
     */
    std::shared_ptr<ChunkGenerationTask> scheduleGenerationTask(
        const world::chunk::status::ChunkStatus& targetStatus,
        const world::ChunkPos& pos
    ) override;

    /**
     * Run pending generation tasks
     * Reference: ChunkMap.java lines 440-442
     */
    void runGenerationTasks() override;

    // =========================================================================
    // Map management
    // =========================================================================

    /**
     * Promote updating chunk map to visible
     * Reference: ChunkMap.java lines 240-262
     */
    bool promoteChunkMap();

    /**
     * Get visible chunk if present
     * Reference: ChunkMap.java lines 264-270
     */
    ChunkHolder* getVisibleChunkIfPresent(int64_t key);

    /**
     * Get updating chunk if present
     * Reference: ChunkMap.java lines 272-278
     */
    ChunkHolder* getUpdatingChunkIfPresent(int64_t key);

    /**
     * Update chunk scheduling (ticket level changes)
     * Reference: ChunkMap.java lines 280-340
     */
    ChunkHolder* updateChunkScheduling(
        int64_t node, int level, ChunkHolder* chunk, int oldLevel
    );

    /**
     * Check if chunk is pending removal
     */
    bool isChunkToRemove(int64_t node) const;

    /**
     * Get the distance manager
     */
    DistanceManager& getDistanceManager() { return m_distanceManager; }

    /**
     * Get the number of chunks
     */
    size_t size() const { return m_updatingChunkMap.size(); }
    ChunkTaskDispatcher::Stats worldgenDispatcherStats() const { return m_worldgenTaskDispatcher->stats(); }
    size_t pendingGenerationTaskCount() const {
        std::lock_guard<std::mutex> lock(m_pendingTasksMutex);
        return m_pendingGenerationTasks.size();
    }
    std::string debugHotTasks(size_t n);   // diagnostics
    size_t featureClaimRetries() const { return m_worldGenContext.featureClaims ? m_worldGenContext.featureClaims->retries.load() : 0; }
    size_t featureClaimWaiters() const {
        if (!m_worldGenContext.featureClaims) return 0;
        std::lock_guard<std::mutex> lock(m_worldGenContext.featureClaims->mutex);
        return m_worldGenContext.featureClaims->waiters.size();
    }
    size_t pendingUnloadCount() const { std::lock_guard<std::mutex> lock(m_mapMutex); return m_pendingUnloads.size(); }

    // Reference: ChunkMap.java processUnloads + scheduleUnload: every holder
    // whose ticket level rose above the max is saved (a proto chunk; FULL
    // chunks are the embedder's) and destroyed. `canUnload(key)` lets the
    // embedder veto a holder it still references (a conversion in flight).
    // Budgeted by `haveTime`, plus Java's minimum: however far the holders
    // ready to unload are over 2,000. Main thread.
    // Returns holders destroyed.
    //
    // Without storage (a read-only world) nothing can be saved, so a proto
    // that may carry another chunk's decoration (TERRAIN up to FULL) is kept
    // instead: see the .cpp.
    size_t processUnloads(const std::function<bool()>& haveTime,
                          const std::function<bool(int64_t)>& canUnload);

    // The embedder took this FULL chunk into its own world (it converts FULL
    // chunks to its own format, saves them itself and loads them from that
    // save). Makes it a candidate for releaseHandedOffChunks. Main thread.
    void markHandedOff(int64_t key);

    // Check up to `maxChecks` candidates, and release the block data
    // (ProtoChunk::releaseBlockData) of each one nothing will read again:
    //  - the holder has completed FULL, and no generation task holds it;
    //  - all 8 neighbours are past SPAWN — SPAWN is the last step that reads
    //    a neighbour's blocks, and FEATURES, the one that writes them, is
    //    already done for every neighbour of a FULL chunk (LIGHT needs
    //    INITIALIZE_LIGHT at radius 1). A neighbour saved and reloaded keeps
    //    its status, so this stays true;
    //  - chunks are saved (with no storage the embedder may regenerate a
    //    chunk it dropped, from this copy);
    //  - `canRelease(key)`: the embedder is not reading it.
    // What other chunks still read — status, structure starts and
    // references — is kept. A candidate not ready yet stays for a later
    // call; one whose holder is gone is dropped. Main thread. Returns
    // chunks released.
    size_t releaseHandedOffChunks(size_t maxChecks, const std::function<bool(int64_t)>& canRelease);

    // Reference: ChunkMap.onChunkReadyToSend, called from prepareTickingChunk
    // once a chunk and its 3x3 are FULL — MC sends it to every player
    // tracking it then, whichever ticket made it FULL. The embedder, which
    // does the sending, is told through this listener. Main thread; the
    // chunk stays valid for the call (the embedder pins it to keep it).
    using ChunkReadyListener = std::function<void(int64_t key, ChunkAccess* chunk)>;
    void setChunkReadyListener(ChunkReadyListener listener);
    size_t releasedChunkCount() const { return m_releasedChunks; }

    // Reference: ChunkMap.java onLevelChange — forwards a holder's new ticket
    // level to both task dispatchers so queued work is re-sorted by it. This
    // was a no-op in the port, which left every holder at queue level MAX+1:
    // no priority at all, so a whole ticketed area generated layer-by-layer
    // breadth-first and nothing reached FULL until almost everything had.
    void onLevelChange(const world::ChunkPos& pos, std::function<int()> oldLevel,
                       int newLevel, std::function<void(int)> setQueueLevel);

    // Visit every holder (main thread). The callback must not add or remove
    // holders.
    template <typename F>
    void forEachHolder(F&& f) {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        for (auto& [key, holder] : m_updatingChunkMap) {
            f(*holder);
        }
    }

    /**
     * Get the worldgen context
     */
    // Mutable access for opt-in wiring (e.g. structure state injection by the
    // embedder after construction; nullptr keeps structures disabled).
    world::chunk::status::WorldGenContext& worldGenContextMutable() {
        return m_worldGenContext;
    }

    const world::chunk::status::WorldGenContext& getWorldGenContext() const {
        return m_worldGenContext;
    }

    /**
     * Chunk storage handed in by the embedder (its region I/O), replacing
     * the storage-path IOWorker, and the DataVersion saves are written in
     * (the embedder's world format). Call before the first chunk is
     * requested. Null storage turns saving and loading off.
     */
    void setChunkStorage(std::shared_ptr<world::level::chunk::storage::ChunkStorageBackend> storage,
                         int dataVersion);

    world::level::chunk::storage::IOWorker* getChunkIo() const { return m_chunkIo.get(); }

    /** The world clock, for each save's LastUpdate. Any thread. */
    void setGameTime(int64_t gameTime) { m_gameTime.store(gameTime, std::memory_order_relaxed); }

    /**
     * Reference: ChunkMap.save(ChunkAccess). Writes a proto chunk that has
     * changed since it was last saved. FULL chunks are the embedder's and are
     * never written here. Main thread; the chunk must not be generating.
     * Returns true when a write was queued.
     */
    bool save(ChunkAccess* chunk);

    /**
     * Reference: ChunkMap.saveAllChunks(boolean). With flush: saves every
     * proto chunk that is not generating, unloads everything queued for
     * unloading (saving it), and waits for the writes to reach the storage.
     * Without flush it does nothing here: MC's periodic save covers only
     * LevelChunks, which are the embedder's. Main thread.
     *
     * `quiesced`: the embedder has stopped every generation task (shutdown),
     * so a holder a cancelled task still claims is quiet too and is saved —
     * MC instead blocks until each holder is ready.
     */
    void saveAllChunks(bool flush, const std::function<bool(int64_t)>& canUnload,
                       bool quiesced = false);

    // =========================================================================
    // Chunk preparation methods
    // =========================================================================

    /**
     * Prepare chunk for accessible (FULL) status
     * Reference: ChunkMap.java lines 692-694
     */
    std::shared_ptr<util::CompletableFuture<std::shared_ptr<ChunkResult<ChunkAccess*>>>>
    prepareAccessibleChunk(ChunkHolder* chunk);

    /**
     * Prepare chunk for ticking (BLOCK_TICKING) status
     * Reference: ChunkMap.java lines 662-677
     */
    std::shared_ptr<util::CompletableFuture<std::shared_ptr<ChunkResult<ChunkAccess*>>>>
    prepareTickingChunk(ChunkHolder* chunk);

    /**
     * Prepare chunk for entity ticking status
     * Reference: ChunkMap.java lines 343-345
     */
    std::shared_ptr<util::CompletableFuture<std::shared_ptr<ChunkResult<ChunkAccess*>>>>
    prepareEntityTickingChunk(ChunkHolder* chunk);

    // =========================================================================
    // Chunk range futures (for loading multiple chunks)
    // =========================================================================

    /**
     * Get a future for a range of chunks around a holder
     * Reference: ChunkMap.java lines 278-319
     */
    std::shared_ptr<util::CompletableFuture<std::shared_ptr<ChunkResult<std::vector<ChunkAccess*>>>>>
    getChunkRangeFuture(
        ChunkHolder* holder,
        int radius,
        std::function<const world::chunk::status::ChunkStatus&(int distance)> statusGetter
    );

private:
    /**
     * Run a single generation task
     * Reference: ChunkMap.java lines 444-460
     * Takes shared_ptr to keep task alive during async execution
     */
    void runGenerationTask(std::shared_ptr<ChunkGenerationTask> task);

    /**
     * Schedule chunk loading from disk
     * Reference: ChunkMap.java lines 462-500
     */
    std::shared_ptr<util::CompletableFuture<ChunkAccess*>>
    scheduleChunkLoad(const world::ChunkPos& pos);

    /**
     * Create an empty protoChunk
     * Reference: ChunkMap.java lines 502-518
     */
    ChunkAccess* createEmptyChunk(const world::ChunkPos& pos);

    // Chunk holder maps
    std::unordered_map<int64_t, std::unique_ptr<ChunkHolder>> m_updatingChunkMap;
    std::unordered_map<int64_t, ChunkHolder*> m_visibleChunkMap;
    std::unordered_set<int64_t> m_pendingUnloads;
    // releaseHandedOffChunks candidates, walked round-robin. Main thread.
    std::vector<int64_t> m_handedOff;
    std::unordered_set<int64_t> m_handedOffSet;
    size_t m_handedOffCursor = 0;
    size_t m_releasedChunks = 0;   // lifetime count, diagnostics
    ChunkReadyListener m_chunkReadyListener;
    void onChunkReadyToSend(int64_t key);
    // Holders created since the last promoteChunkMap. Java clones the whole
    // updating map on every promotion; that clone grew to 13k+ entries and
    // ran once per chunk request, so promotion appends these instead.
    // Removals need no list: processUnloads erases from the visible map
    // (and from this list) as it destroys a holder.
    std::vector<std::pair<int64_t, ChunkHolder*>> m_visibleAdds;

    // Generation state. Java's pendingGenerationTasks is main-thread-only;
    // here a task can also be scheduled from the worldgen lane (the loading-
    // pyramid recovery in ChunkGenerationTask::scheduleChunkInLayer), so the
    // list has its own lock. Unlocked, a push from the lane landing between
    // runGenerationTasks' copy and its clear() was dropped: the task never
    // ran, yet kept its claim on every holder of its pyramid (~529) — the
    // "task scheduled=none, refs in the hundreds" holders that stopped
    // holder unloading from being enabled (2026-08-30).
    mutable std::mutex m_pendingTasksMutex;
    std::vector<std::shared_ptr<ChunkGenerationTask>> m_pendingGenerationTasks;
    world::chunk::status::WorldGenContext m_worldGenContext;

    // ConsecutiveExecutors - ensure tasks for each dispatcher run one at a time
    // Reference: ChunkMap.java lines 183-187 - uses ConsecutiveExecutor for worldgen and light
    std::shared_ptr<util::thread::ConsecutiveExecutor> m_worldgenConsecutiveExecutor;
    std::shared_ptr<util::thread::ConsecutiveExecutor> m_lightConsecutiveExecutor;

    // Dispatchers
    std::unique_ptr<ChunkTaskDispatcher> m_worldgenTaskDispatcher;
    std::unique_ptr<ChunkTaskDispatcher> m_lightTaskDispatcher;

    // Components
    DistanceManager m_distanceManager;

    // Synchronization
    mutable std::mutex m_mapMutex;
    bool m_modified = false;

    // References
    world::level::TicketStorage& m_ticketStorage;
    Executor m_backgroundExecutor;  // Background thread pool (like Util.backgroundExecutor())
    Executor m_mainThreadExecutor;

    // Storage
    std::unique_ptr<world::level::chunk::storage::IOWorker> m_chunkIo;
    int m_dataVersion;
    std::atomic<int64_t> m_gameTime{0};
    // Reference: ChunkMap.chunkTypeCache - what kind of chunk each position
    // holds on disk (1 = full, -1 = proto), filled by loads and saves.
    std::mutex m_chunkTypeCacheMutex;
    std::unordered_map<int64_t, int8_t> m_chunkTypeCache;
    void markPosition(const world::ChunkPos& pos, bool full);
    bool isExistingChunkFull(const world::ChunkPos& pos);
    std::string m_storagePath;
    std::unique_ptr<ServerLevel> m_serverLevel;

    // World parameters for chunk creation
    world::BlockRegistry* m_blockRegistry;
    BlockState* m_airBlock;
    BlockState* m_defaultBlock;
    int32_t m_minY;
    int32_t m_worldHeight;
};

} // namespace level
} // namespace server
} // namespace minecraft
