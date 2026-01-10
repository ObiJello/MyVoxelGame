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
#include "world/IBlockType.h"
#include "util/CompletableFuture.h"
#include "util/thread/ConsecutiveExecutor.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <mutex>
#include <string>

// Reference: net/minecraft/server/level/ChunkMap.java

namespace minecraft {
namespace server {
namespace level {

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
        const std::string& storagePath = "",
        const std::string& levelId = "world",
        const std::string& dimension = "overworld",
        world::BlockRegistry* blockRegistry = nullptr,
        world::IBlockType* airBlock = nullptr,
        world::IBlockType* defaultBlock = nullptr,
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

    /**
     * Get the worldgen context
     */
    const world::chunk::status::WorldGenContext& getWorldGenContext() const {
        return m_worldGenContext;
    }

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

    // Generation state
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
    std::string m_storagePath;

    // World parameters for chunk creation
    world::BlockRegistry* m_blockRegistry;
    world::IBlockType* m_airBlock;
    world::IBlockType* m_defaultBlock;
    int32_t m_minY;
    int32_t m_worldHeight;
};

} // namespace level
} // namespace server
} // namespace minecraft
