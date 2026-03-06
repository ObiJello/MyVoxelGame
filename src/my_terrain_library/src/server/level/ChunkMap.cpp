#include "server/level/ChunkMap.h"
#include "server/level/WorldGenRegion.h"
#include "world/chunk/status/ChunkPyramid.h"
#include "world/chunk/status/ChunkStatusTasks.h"
#include "world/chunk/status/ChunkDependencies.h"
#include "world/level/chunk/storage/SerializableChunkData.h"
#include "world/ProtoChunk.h"
#include "nbt/CompoundTag.h"

// Reference: net/minecraft/server/level/ChunkMap.java

namespace minecraft {
namespace server {
namespace level {

using namespace world::chunk::status;

// Reference: ChunkMap.java lines 130-180
ChunkMap::ChunkMap(
    levelgen::ChunkGenerator* generator,
    levelgen::RandomState* randomState,
    int64_t seed,
    world::level::TicketStorage& ticketStorage,
    Executor backgroundExecutor,
    Executor mainThreadExecutor,
    const std::string& storagePath,
    const std::string& levelId,
    const std::string& dimension,
    world::BlockRegistry* blockRegistry,
    BlockState* airBlock,
    BlockState* defaultBlock,
    int32_t minY,
    int32_t worldHeight)
    : m_distanceManager(backgroundExecutor, ticketStorage)
    , m_ticketStorage(ticketStorage)
    , m_backgroundExecutor(backgroundExecutor)
    , m_mainThreadExecutor(std::move(mainThreadExecutor))
    , m_storagePath(storagePath)
    , m_blockRegistry(blockRegistry)
    , m_airBlock(airBlock)
    , m_defaultBlock(defaultBlock)
    , m_minY(minY)
    , m_worldHeight(worldHeight)
{
    // Initialize world gen context
    // Reference: ChunkMap.java - worldGenContext includes executors for async work
    m_worldGenContext = WorldGenContext(generator, randomState, seed);
    m_worldGenContext.mainThreadExecutor = m_mainThreadExecutor;
    // Wire background executor for async tasks (NOISE, BIOMES use supplyAsync)
    // Reference: NoiseBasedChunkGenerator.fillFromNoise() uses Util.backgroundExecutor()
    m_worldGenContext.backgroundExecutor = m_backgroundExecutor;
    m_worldGenContext.unsavedListener = [this](int x, int z) {
        // Mark chunk as unsaved
        int64_t key = world::ChunkPos::asLong(x, z);
        auto it = m_updatingChunkMap.find(key);
        if (it != m_updatingChunkMap.end()) {
            it->second->markUnsaved();
        }
    };

    // Set up distance manager
    m_distanceManager.setChunkMap(this);

    // Create task dispatchers
    // Reference: ChunkMap.java lines 183-187
    // ConsecutiveExecutor worldgen = new ConsecutiveExecutor(executor, "worldgen");
    // this.worldgenTaskDispatcher = new ChunkTaskDispatcher(worldgen, executor);
    //
    // CRITICAL: Must use ConsecutiveExecutor, not plain ExecutorTaskScheduler!
    // The ConsecutiveExecutor ensures tasks execute ONE AT A TIME, preventing
    // contention and ensuring proper ordering. This is how Java does it.
    m_worldgenConsecutiveExecutor = std::make_shared<util::thread::ConsecutiveExecutor>(
        m_backgroundExecutor, "worldgen"
    );
    m_worldgenTaskDispatcher = std::make_unique<ChunkTaskDispatcher>(
        m_worldgenConsecutiveExecutor, m_backgroundExecutor
    );

    m_lightConsecutiveExecutor = std::make_shared<util::thread::ConsecutiveExecutor>(
        m_backgroundExecutor, "light"
    );
    m_lightTaskDispatcher = std::make_unique<ChunkTaskDispatcher>(
        m_lightConsecutiveExecutor, m_backgroundExecutor
    );

    // Initialize chunk I/O if storage path provided
    // Reference: ChunkMap.java lines 155-170
    if (!storagePath.empty()) {
        world::level::chunk::storage::RegionStorageInfo storageInfo(levelId, dimension, "chunk");
        std::string regionPath = storagePath + "/region";
        m_chunkIo = std::make_unique<world::level::chunk::storage::IOWorker>(
            storageInfo, regionPath, true  // sync = true for safety
        );
    }

    // Ensure ChunkPyramid is initialized
    ChunkPyramid::initialize();
}

ChunkMap::~ChunkMap() = default;

// Reference: ChunkMap.java lines 342-358
GenerationChunkHolder* ChunkMap::acquireGeneration(int64_t chunkPos) {
    std::lock_guard<std::mutex> lock(m_mapMutex);

    auto it = m_updatingChunkMap.find(chunkPos);
    if (it == m_updatingChunkMap.end()) {
        // Create new holder
        world::ChunkPos pos(chunkPos);
        auto holder = std::make_unique<ChunkHolder>(
            pos,
            ChunkLevel::getMaxLevel(),
            m_worldHeight,
            m_minY,
            // onLevelChange - no-op for now
            [](const world::ChunkPos&, std::function<int()>, int, std::function<void(int)>) {},
            // playerProvider - no-op
            [](const world::ChunkPos&, bool) { return std::vector<void*>{}; }
        );
        ChunkHolder* ptr = holder.get();
        m_updatingChunkMap[chunkPos] = std::move(holder);
        m_modified = true;
        ptr->increaseGenerationRefCount();
        return ptr;
    }

    it->second->increaseGenerationRefCount();
    return it->second.get();
}

// Reference: ChunkMap.java lines 360-366
void ChunkMap::releaseGeneration(GenerationChunkHolder* holder) {
    holder->decreaseGenerationRefCount();
}

// Reference: ChunkMap.java lines 608-635
// Java's applyStep directly calls step.apply() which returns a CompletableFuture
// The async work happens inside the task (e.g., NOISE uses supplyAsync to background executor)
std::shared_ptr<util::CompletableFuture<ChunkMap::ChunkAccess*>>
ChunkMap::applyStep(
    GenerationChunkHolder* chunkHolder,
    const ChunkStep& step,
    util::StaticCache2D<GenerationChunkHolder*>& cache)
{
    const ChunkStatus& targetStatus = step.targetStatus();
    world::ChunkPos pos = chunkHolder->getPos();

    // Reference: ChunkMap.java line 610-611
    // For EMPTY status, schedule chunk load
    if (&targetStatus == &ChunkStatus::EMPTY) {
        return scheduleChunkLoad(pos);
    }

    try {
        // Reference: ChunkMap.java lines 613-619
        GenerationChunkHolder* holder = cache.get(pos.x(), pos.z());
        ChunkAccess* centerChunk = holder->getChunkIfPresentUnchecked(targetStatus.getParent());

        if (centerChunk == nullptr) {
            // Reference: ChunkMap.java line 617
            throw std::logic_error("Parent chunk missing");
        }

        // Build 2D grid of neighbor chunks for the task
        // Note: Java passes StaticCache2D<GenerationChunkHolder> directly to task.doWork()
        // but our C++ tasks expect a 2D vector of chunks
        const auto& deps = step.accumulatedDependencies();
        int radius = deps.size() - 1;
        int gridSize = radius * 2 + 1;

        std::vector<std::vector<ChunkAccess*>> neighborChunks(gridSize);
        for (int i = 0; i < gridSize; ++i) {
            neighborChunks[i].resize(gridSize, nullptr);
        }

        // Fill the grid with chunks from cache
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                int distance = std::max(std::abs(dx), std::abs(dz));
                GenerationChunkHolder* chunkHolder = cache.get(pos.x() + dx, pos.z() + dz);
                if (chunkHolder != nullptr && distance < static_cast<int>(deps.size())) {
                    const ChunkStatus& requiredStatus = deps.get(distance);
                    ChunkAccess* chunk = chunkHolder->getChunkIfPresentUnchecked(requiredStatus);
                    neighborChunks[dz + radius][dx + radius] = chunk;
                }
            }
        }

        // Center chunk is at (radius, radius) in the grid
        neighborChunks[radius][radius] = centerChunk;

        // Reference: ChunkMap.java line 619
        // Java: return step.apply(this.worldGenContext, cache, centerChunk);
        // This returns the future DIRECTLY - async work happens inside the task
        // (e.g., NOISE task uses supplyAsync with background executor)
        return step.apply(
            const_cast<world::chunk::status::WorldGenContext&>(m_worldGenContext),
            neighborChunks,
            centerChunk
        );

    } catch (const std::exception& e) {
        // Reference: ChunkMap.java lines 621-633
        // Log error - in full implementation would create crash report
        return util::CompletableFuture<ChunkAccess*>::completed(nullptr);
    }
}

// Reference: ChunkMap.java lines 428-438
std::shared_ptr<ChunkGenerationTask> ChunkMap::scheduleGenerationTask(
    const ChunkStatus& targetStatus,
    const world::ChunkPos& pos)
{
    auto task = ChunkGenerationTask::create(*this, targetStatus, pos);
    m_pendingGenerationTasks.push_back(task);
    return task;
}

// Reference: ChunkMap.java lines 440-442
// Java: this.pendingGenerationTasks.forEach(this::runGenerationTask);
//       this.pendingGenerationTasks.clear();
void ChunkMap::runGenerationTasks() {
    // Copy tasks to iterate (like Java's forEach)
    // The shared_ptrs keep tasks alive even after clear()
    auto tasks = m_pendingGenerationTasks;
    m_pendingGenerationTasks.clear();

    for (auto& task : tasks) {
        runGenerationTask(task);
    }
}

// Reference: ChunkMap.java lines 444-460
// Takes shared_ptr by value to keep task alive during async execution
void ChunkMap::runGenerationTask(std::shared_ptr<ChunkGenerationTask> task) {
    GenerationChunkHolder* center = task->getCenter();
    int64_t chunkPos = center->getPos().toLong();
    auto pos = center->getPos();

    m_worldgenTaskDispatcher->submit(
        [this, task, pos]() {  // task captured by value (shared_ptr copy)
            auto waitFuture = task->runUntilWait();
            if (waitFuture != nullptr) {
                // Need to wait - reschedule when done
                // Lambda captures shared_ptr, keeping task alive
                waitFuture->thenRun([this, task]() {
                    this->runGenerationTask(task);
                });
            }
            // else: task is complete, shared_ptr ref count decreases
        },
        chunkPos,
        [center]() { return center->getQueueLevel(); }
    );
}

// Reference: ChunkMap.java lines 462-500
std::shared_ptr<util::CompletableFuture<ChunkMap::ChunkAccess*>>
ChunkMap::scheduleChunkLoad(const world::ChunkPos& pos) {
    auto resultFuture = std::make_shared<util::CompletableFuture<ChunkAccess*>>();

    // Try to load from disk if IOWorker is available
    // Reference: ChunkMap.java lines 533-553
    if (m_chunkIo) {
        // Async load from disk
        auto loadFuture = m_chunkIo->loadAsync(pos);

        loadFuture->thenAccept([this, pos, resultFuture](
            const std::optional<std::unique_ptr<nbt::CompoundTag>>& optionalTag) {

            if (optionalTag && *optionalTag) {
                // Found chunk data on disk - parse it
                const auto& tag = *optionalTag;

                // Get chunk status from tag to determine what type of chunk to create
                const ChunkStatus* status =
                    world::level::chunk::storage::SerializableChunkData::getChunkStatusFromTag(tag.get());

                if (status && status->isOrAfter(ChunkStatus::FULL)) {
                    // Chunk is fully generated - would create LevelChunk
                    // For now, create empty since we don't have full chunk implementation
                    ChunkAccess* chunk = createEmptyChunk(pos);
                    if (chunk) {
                        chunk->setPersistedStatus(*status);
                    }
                    resultFuture->complete(chunk);
                } else {
                    // Chunk is partially generated - would create ProtoChunk
                    ChunkAccess* chunk = createEmptyChunk(pos);
                    if (chunk && status) {
                        chunk->setPersistedStatus(*status);
                    }
                    resultFuture->complete(chunk);
                }
            } else {
                // No chunk data on disk - create new empty chunk
                ChunkAccess* chunk = createEmptyChunk(pos);
                resultFuture->complete(chunk);
            }
        });

        return resultFuture;
    }

    // No IOWorker - create new empty chunk synchronously
    ChunkAccess* chunk = createEmptyChunk(pos);
    resultFuture->complete(chunk);

    return resultFuture;
}

// Reference: ChunkMap.java lines 502-518
ChunkMap::ChunkAccess* ChunkMap::createEmptyChunk(const world::ChunkPos& pos) {
    // Ensure we have required parameters
    if (!m_blockRegistry || !m_airBlock) {
        return nullptr;
    }

    // Create new ProtoChunk with EMPTY status
    // Reference: ChunkMap.java createEmptyChunk() creates ProtoChunk
    auto* chunk = new world::ProtoChunk(
        pos,
        m_minY,
        m_worldHeight,
        m_airBlock,
        m_defaultBlock ? m_defaultBlock : m_airBlock,
        m_blockRegistry
    );

    chunk->setPersistedStatus(ChunkStatus::EMPTY);
    return chunk;
}

// Reference: ChunkMap.java lines 240-262
bool ChunkMap::promoteChunkMap() {
    std::lock_guard<std::mutex> lock(m_mapMutex);

    if (!m_modified) {
        return false;
    }

    // Update visible map
    m_visibleChunkMap.clear();
    for (const auto& pair : m_updatingChunkMap) {
        m_visibleChunkMap[pair.first] = pair.second.get();
    }

    m_modified = false;
    return true;
}

// Reference: ChunkMap.java lines 264-270
ChunkHolder* ChunkMap::getVisibleChunkIfPresent(int64_t key) {
    auto it = m_visibleChunkMap.find(key);
    return (it != m_visibleChunkMap.end()) ? it->second : nullptr;
}

// Reference: ChunkMap.java lines 272-278
ChunkHolder* ChunkMap::getUpdatingChunkIfPresent(int64_t key) {
    std::lock_guard<std::mutex> lock(m_mapMutex);
    auto it = m_updatingChunkMap.find(key);
    return (it != m_updatingChunkMap.end()) ? it->second.get() : nullptr;
}

// Reference: ChunkMap.java lines 280-340
ChunkHolder* ChunkMap::updateChunkScheduling(
    int64_t node, int level, ChunkHolder* chunk, int oldLevel)
{
    std::lock_guard<std::mutex> lock(m_mapMutex);

    if (chunk == nullptr && level > ChunkLevel::getMaxLevel()) {
        // No chunk and level is max - nothing to do
        return nullptr;
    }

    if (chunk != nullptr) {
        chunk->setTicketLevel(level);
    }

    if (level > ChunkLevel::getMaxLevel()) {
        // Chunk should be unloaded
        if (chunk != nullptr) {
            m_pendingUnloads.insert(node);
        }
        return chunk;
    }

    if (chunk == nullptr) {
        // Create new holder
        world::ChunkPos pos(node);
        auto holder = std::make_unique<ChunkHolder>(
            pos,
            level,
            m_worldHeight,
            m_minY,
            // onLevelChange - no-op
            [](const world::ChunkPos&, std::function<int()>, int, std::function<void(int)>) {},
            // playerProvider - no-op
            [](const world::ChunkPos&, bool) { return std::vector<void*>{}; }
        );
        chunk = holder.get();
        m_updatingChunkMap[node] = std::move(holder);
    }

    m_pendingUnloads.erase(node);
    m_modified = true;
    return chunk;
}

bool ChunkMap::isChunkToRemove(int64_t node) const {
    return m_pendingUnloads.count(node) > 0;
}

// =========================================================================
// Chunk preparation methods
// Reference: ChunkMap.java
// =========================================================================

// Reference: ChunkMap.java lines 692-694
std::shared_ptr<util::CompletableFuture<std::shared_ptr<ChunkResult<ChunkMap::ChunkAccess*>>>>
ChunkMap::prepareAccessibleChunk(ChunkHolder* chunk) {
    // Get chunks in radius 1 with status determined by ChunkLevel::getStatusAroundFullChunk
    return getChunkRangeFuture(chunk, 1, [](int distance) -> const ChunkStatus& {
        const ChunkStatus* status = ChunkLevel::getStatusAroundFullChunk(distance);
        return status ? *status : ChunkStatus::FULL;
    })->thenApply([](std::shared_ptr<ChunkResult<std::vector<ChunkAccess*>>> result) {
        if (!result || !result->isSuccess()) {
            return GenerationChunkHolder::UNLOADED_CHUNK;
        }
        auto chunks = result->orElse({});
        if (chunks.empty()) {
            return GenerationChunkHolder::UNLOADED_CHUNK;
        }
        // Center chunk is at the middle of the list
        ChunkAccess* centerChunk = chunks[chunks.size() / 2];
        return ChunkResult<ChunkAccess*>::of(centerChunk);
    });
}

// Reference: ChunkMap.java lines 662-677
std::shared_ptr<util::CompletableFuture<std::shared_ptr<ChunkResult<ChunkMap::ChunkAccess*>>>>
ChunkMap::prepareTickingChunk(ChunkHolder* chunk) {
    // Get chunks in radius 1 with FULL status
    return getChunkRangeFuture(chunk, 1, [](int /*distance*/) -> const ChunkStatus& {
        return ChunkStatus::FULL;
    })->thenApply([this, chunk](std::shared_ptr<ChunkResult<std::vector<ChunkAccess*>>> result) {
        if (!result || !result->isSuccess()) {
            return GenerationChunkHolder::UNLOADED_CHUNK;
        }
        auto chunks = result->orElse({});
        if (chunks.empty()) {
            return GenerationChunkHolder::UNLOADED_CHUNK;
        }
        // Center chunk is at the middle of the list
        ChunkAccess* levelChunk = chunks[chunks.size() / 2];

        // In full implementation:
        // levelChunk->postProcessGeneration(m_level);
        // m_level.startTickingChunk(levelChunk);
        //
        // auto sendSync = chunk->getSendSyncFuture();
        // if (sendSync->isDone()) {
        //     onChunkReadyToSend(chunk, levelChunk);
        // } else {
        //     sendSync->thenAcceptAsync([this, chunk, levelChunk](auto) {
        //         onChunkReadyToSend(chunk, levelChunk);
        //     }, m_mainThreadExecutor);
        // }

        return ChunkResult<ChunkAccess*>::of(levelChunk);
    });
}

// Reference: ChunkMap.java lines 343-345
std::shared_ptr<util::CompletableFuture<std::shared_ptr<ChunkResult<ChunkMap::ChunkAccess*>>>>
ChunkMap::prepareEntityTickingChunk(ChunkHolder* chunk) {
    // Get chunks in radius 2 with FULL status
    return getChunkRangeFuture(chunk, 2, [](int /*distance*/) -> const ChunkStatus& {
        return ChunkStatus::FULL;
    })->thenApply([](std::shared_ptr<ChunkResult<std::vector<ChunkAccess*>>> result) {
        if (!result || !result->isSuccess()) {
            return GenerationChunkHolder::UNLOADED_CHUNK;
        }
        auto chunks = result->orElse({});
        if (chunks.empty()) {
            return GenerationChunkHolder::UNLOADED_CHUNK;
        }
        // Center chunk is at the middle of the list
        ChunkAccess* centerChunk = chunks[chunks.size() / 2];
        return ChunkResult<ChunkAccess*>::of(centerChunk);
    });
}

// =========================================================================
// Chunk range futures
// =========================================================================

// Reference: ChunkMap.java lines 278-319
std::shared_ptr<util::CompletableFuture<std::shared_ptr<ChunkResult<std::vector<ChunkMap::ChunkAccess*>>>>>
ChunkMap::getChunkRangeFuture(
    ChunkHolder* centerChunk,
    int range,
    std::function<const ChunkStatus&(int distance)> distanceToStatus)
{
    using ChunkListResult = std::shared_ptr<ChunkResult<std::vector<ChunkAccess*>>>;
    using ChunkListFuture = std::shared_ptr<util::CompletableFuture<ChunkListResult>>;

    auto resultFuture = std::make_shared<util::CompletableFuture<ChunkListResult>>();

    // Handle the simple case of range 0
    if (range == 0) {
        const ChunkStatus& status = distanceToStatus(0);
        auto chunkFuture = centerChunk->scheduleChunkGenerationTask(status, *this);
        chunkFuture->thenAccept([resultFuture](auto chunkResult) {
            if (!chunkResult || !chunkResult->isSuccess()) {
                resultFuture->complete(ChunkResult<std::vector<ChunkAccess*>>::error("Chunk unavailable"));
                return;
            }
            ChunkAccess* chunk = chunkResult->orElse(nullptr);
            if (chunk == nullptr) {
                resultFuture->complete(ChunkResult<std::vector<ChunkAccess*>>::error("Chunk unavailable"));
                return;
            }
            resultFuture->complete(ChunkResult<std::vector<ChunkAccess*>>::of({chunk}));
        });
        return resultFuture;
    }

    // Collect all chunk futures
    int chunkCount = (range * 2 + 1) * (range * 2 + 1);
    auto pendingFutures = std::make_shared<std::vector<FutureType>>();
    pendingFutures->reserve(chunkCount);

    world::ChunkPos centerPos = centerChunk->getPos();

    // Gather futures for all chunks in range
    for (int z = -range; z <= range; ++z) {
        for (int x = -range; x <= range; ++x) {
            int distance = std::max(std::abs(x), std::abs(z));
            int64_t chunkNode = world::ChunkPos::asLong(centerPos.x() + x, centerPos.z() + z);
            ChunkHolder* chunk = getUpdatingChunkIfPresent(chunkNode);

            if (chunk == nullptr) {
                // Chunk not available - return unloaded result
                resultFuture->complete(ChunkResult<std::vector<ChunkAccess*>>::error("Chunk unavailable"));
                return resultFuture;
            }

            const ChunkStatus& depStatus = distanceToStatus(distance);
            pendingFutures->push_back(chunk->scheduleChunkGenerationTask(depStatus, *this));
        }
    }

    // Wait for all futures and combine results
    auto pendingCount = std::make_shared<std::atomic<int>>(chunkCount);
    auto chunks = std::make_shared<std::vector<ChunkAccess*>>(chunkCount, nullptr);
    auto failed = std::make_shared<std::atomic<bool>>(false);

    for (int i = 0; i < chunkCount; ++i) {
        (*pendingFutures)[i]->thenAccept([i, chunks, pendingCount, failed, resultFuture](auto chunkResult) {
            if (failed->load()) {
                return;  // Already failed
            }

            if (!chunkResult || !chunkResult->isSuccess()) {
                bool expected = false;
                if (failed->compare_exchange_strong(expected, true)) {
                    resultFuture->complete(ChunkResult<std::vector<ChunkAccess*>>::error("Chunk unavailable"));
                }
                return;
            }

            ChunkAccess* chunk = chunkResult->orElse(nullptr);
            if (chunk == nullptr) {
                bool expected = false;
                if (failed->compare_exchange_strong(expected, true)) {
                    resultFuture->complete(ChunkResult<std::vector<ChunkAccess*>>::error("Chunk unavailable"));
                }
                return;
            }

            (*chunks)[i] = chunk;

            if (pendingCount->fetch_sub(1) == 1) {
                // Last one - complete the future
                if (!failed->load()) {
                    resultFuture->complete(ChunkResult<std::vector<ChunkAccess*>>::of(std::move(*chunks)));
                }
            }
        });
    }

    return resultFuture;
}

} // namespace level
} // namespace server
} // namespace minecraft
