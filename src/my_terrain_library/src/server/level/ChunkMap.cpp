#include "util/LiveCounters.h"
#include "server/level/ChunkMap.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include "util/TerrainProfiling.h"
#include "server/level/ServerLevel.h"
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
    Executor laneExecutor,
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
    , m_dataVersion(world::level::chunk::storage::ChunkSerializer::DATA_VERSION)
    , m_storagePath(storagePath)
    , m_serverLevel(std::make_unique<ServerLevel>(
        minY, worldHeight, blockRegistry, airBlock, defaultBlock, seed
    ))
    , m_blockRegistry(blockRegistry)
    , m_airBlock(airBlock)
    , m_defaultBlock(defaultBlock)
    , m_minY(minY)
    , m_worldHeight(worldHeight)
{
    // Initialize world gen context
    // Reference: ChunkMap.java - worldGenContext includes executors for async work
    m_worldGenContext = WorldGenContext(generator, randomState, seed);
    m_worldGenContext.level = m_serverLevel.get();
    m_worldGenContext.mainThreadExecutor = m_mainThreadExecutor;
    // Wire background executor for async tasks (NOISE, BIOMES use supplyAsync)
    // Reference: NoiseBasedChunkGenerator.fillFromNoise() uses Util.backgroundExecutor()
    m_worldGenContext.backgroundExecutor = m_backgroundExecutor;
    // Parallel decoration (ChunkStatusTasks::generateFeatures with
    // FeatureClaims) stays DISABLED. Even with structure placement under one
    // lock (ChunkGenerator::applyBiomeDecoration) it still crashed in
    // WorldGenRegionLevel::getBlockState from JigsawPieceBehavior: structure
    // pieces read blocks well outside the decorating chunk's 3x3, i.e. in
    // chunks another decoration task may be writing. Vanilla's single
    // worldgen lane is the invariant that makes those reads safe; keep it.
    // Parallel decoration under FeatureClaims is OPT-IN (OBEY_PARALLEL_FEATURES=1).
    // Measured 2026-08-30 on an M4 (4P+6E): correct and crash-free with the
    // distance rule, but the far-teleport view took 57-58 s vs 53 s serial —
    // decoration moved off the lane's performance core onto default-QoS pool
    // threads (efficiency cores) and the frontier serialised on claims
    // (5,280 retries / 7,450 chunks). The serial lane is the better default here.
    m_worldGenContext.featureClaims = std::getenv("OBEY_PARALLEL_FEATURES")
        ? std::make_shared<world::chunk::status::FeatureClaims>() : nullptr;
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
    // Java runs the mailbox and the worldgen lane on Util.backgroundExecutor —
    // a ForkJoinPool whose small tasks are picked up immediately. Our pool is
    // a FIFO: every hop of the serial lane queued behind 10-30 ms noise and
    // biome tasks, so the pipeline advanced at the pool's queue latency
    // instead of the lane's speed. The embedder passes dedicated threads for
    // these two serial executors.
    Executor lane = laneExecutor ? laneExecutor : m_backgroundExecutor;
    m_worldgenConsecutiveExecutor = std::make_shared<util::thread::ConsecutiveExecutor>(
        lane, "worldgen"
    );
    m_worldgenTaskDispatcher = std::make_unique<ChunkTaskDispatcher>(
        m_worldgenConsecutiveExecutor, lane
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
            [this](const world::ChunkPos& p, std::function<int()> oldLevel, int newLevel,
                   std::function<void(int)> setQueueLevel) {
                onLevelChange(p, std::move(oldLevel), newLevel, std::move(setQueueLevel));
            },
            // playerProvider - no-op
            [](const world::ChunkPos&, bool) { return std::vector<void*>{}; }
        );
        ChunkHolder* ptr = holder.get();
        m_updatingChunkMap[chunkPos] = std::move(holder);
        m_visibleAdds.emplace_back(chunkPos, ptr);
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
    {
        std::lock_guard<std::mutex> lock(m_pendingTasksMutex);
        m_pendingGenerationTasks.push_back(task);
    }
    return task;
}

// Reference: ChunkMap.java lines 440-442
// Java: this.pendingGenerationTasks.forEach(this::runGenerationTask);
//       this.pendingGenerationTasks.clear();
void ChunkMap::runGenerationTasks() {
    // Take the list in one step under its lock (see m_pendingTasksMutex):
    // a task scheduled from the lane while these run lands in the fresh
    // list and runs on the next call. The shared_ptrs keep tasks alive.
    std::vector<std::shared_ptr<ChunkGenerationTask>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_pendingTasksMutex);
        tasks.swap(m_pendingGenerationTasks);
    }

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
            TERRAIN_ZONE_N("Lane.RunUntilWait");
            task->noteRun();
            ChunkGenerationTask::FutureType waitFuture;
            try {
                waitFuture = task->runUntilWait();
            } catch (const std::exception& e) {
                // Java would crash the server here. We cannot: release the
                // task's claims so its holders can unload and other tasks
                // waiting on them resume, and report loudly.
                std::fprintf(stderr, "[ChunkMap] generation task for (%d,%d) threw: %s\n",
                             pos.x(), pos.z(), e.what());
                task->markForCancellation();
                task->releaseClaim();
                return;
            }
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
            // Reference: ChunkMap.scheduleChunkLoad - a saved chunk is read back
            // with everything it had (SerializableChunkData.parse + read); no
            // data (or a tag without a Status) is a fresh EMPTY proto. A chunk
            // that fails to decode is reported and generated anew, and is then
            // never saved over (the storage remembers it as unreadable).
            ChunkAccess* chunk = nullptr;
            if (optionalTag && *optionalTag) {
                try {
                    auto data = world::level::chunk::storage::SerializableChunkData::parse(
                        m_minY, m_worldHeight, **optionalTag, m_airBlock);
                    if (data) {
                        auto proto = data->read(pos, m_minY, m_worldHeight, m_airBlock,
                                                m_defaultBlock, m_blockRegistry);
                        if (proto) {
                            markPosition(pos, data->getChunkStatus().getChunkType()
                                                  == world::chunk::status::ChunkType::LEVELCHUNK);
                            chunk = proto.release();
                        }
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[ChunkMap] Couldn't load chunk (%d,%d): %s\n",
                                 pos.x(), pos.z(), e.what());
                    m_chunkIo->markUnreadable(pos);
                    chunk = nullptr;
                }
            }
            if (!chunk) {
                chunk = createEmptyChunk(pos);
            }
            resultFuture->complete(chunk);
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

void ChunkMap::setChunkStorage(
    std::shared_ptr<world::level::chunk::storage::ChunkStorageBackend> storage, int dataVersion) {
    m_chunkIo = storage ? std::make_unique<world::level::chunk::storage::IOWorker>(std::move(storage))
                        : nullptr;
    m_dataVersion = dataVersion;
}

// Reference: ChunkMap.markPosition.
void ChunkMap::markPosition(const world::ChunkPos& pos, bool full) {
    std::lock_guard<std::mutex> lock(m_chunkTypeCacheMutex);
    m_chunkTypeCache[pos.toLong()] = full ? 1 : -1;
}

// Reference: ChunkMap.isExistingChunkFull. MC reads the chunk's status from
// disk when the cache does not know it; here the storage makes the same
// check itself, atomically with the write (a backend refuses to replace a
// full chunk), so only what this map has already seen is consulted.
bool ChunkMap::isExistingChunkFull(const world::ChunkPos& pos) {
    std::lock_guard<std::mutex> lock(m_chunkTypeCacheMutex);
    auto it = m_chunkTypeCache.find(pos.toLong());
    return it != m_chunkTypeCache.end() && it->second == 1;
}

// Reference: ChunkMap.save(ChunkAccess).
bool ChunkMap::save(ChunkAccess* chunk) {
    auto* proto = dynamic_cast<world::ProtoChunk*>(chunk);
    if (!m_chunkIo || proto == nullptr) {
        return false;
    }
    const ChunkStatus* status = proto->getPersistedStatus();
    // FULL chunks are the embedder's: it converts and saves them itself.
    if (status == nullptr || status->getChunkType() == world::chunk::status::ChunkType::LEVELCHUNK) {
        return false;
    }
    if (!proto->tryMarkSaved()) {
        return false;
    }
    const world::ChunkPos pos = proto->getPos();
    // A position whose saved data could not be read is never written over.
    if (m_chunkIo->isUnreadable(pos) || isExistingChunkFull(pos)) {
        return false;
    }
    if (status == &ChunkStatus::EMPTY) {
        bool anyValid = false;
        for (const auto& [name, start] : proto->getAllStructureStarts()) {
            if (start.isValid()) { anyValid = true; break; }
        }
        if (!anyValid) return false;
    }

    try {
        // Copy here (the chunk is quiet), encode on the I/O thread — MC's
        // copyOf on the main thread + write on the background executor.
        std::shared_ptr<world::level::chunk::storage::SerializableChunkData> data =
            world::level::chunk::storage::SerializableChunkData::copyOf(
                *proto, m_gameTime.load(std::memory_order_relaxed));
        const int dataVersion = m_dataVersion;
        m_chunkIo->store(pos, [data, dataVersion]() { return data->write(dataVersion); });
        markPosition(pos, false);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[ChunkMap] Failed to save chunk (%d,%d): %s\n", pos.x(), pos.z(), e.what());
        return false;
    }
}

// Reference: ChunkMap.saveAllChunks(boolean).
void ChunkMap::saveAllChunks(bool flush, const std::function<bool(int64_t)>& canUnload,
                             bool quiesced) {
    if (!flush || !m_chunkIo) {
        return;
    }
    std::vector<ChunkHolder*> holders;
    {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        holders.reserve(m_updatingChunkMap.size());
        for (auto& [key, holder] : m_updatingChunkMap) {
            holders.push_back(holder.get());
        }
    }
    for (ChunkHolder* holder : holders) {
        // MC blocks until each holder is ready. Here a holder a task still
        // works on keeps what it had when it was last saved — unless the
        // embedder has stopped every task (quiesced).
        if (!quiesced && (holder->generationRefCount() != 0 || !holder->isReadyForSaving())) continue;
        if (ChunkAccess* chunk = holder->getLatestChunk()) {
            save(chunk);
        }
    }
    processUnloads([]() { return true; }, canUnload);
    m_chunkIo->synchronize(true)->join();
}

// Reference: ChunkMap.java lines 240-262
std::string ChunkMap::debugHotTasks(size_t n) {
    std::vector<std::pair<int, std::string>> rows;
    {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        for (auto& [key, holder] : m_updatingChunkMap) {
            std::string st = holder->debugState();
            auto p = st.find("runs=");
            if (p == std::string::npos) continue;
            int runs = std::atoi(st.c_str() + p + 5);
            if (runs > 20) rows.emplace_back(runs, world::ChunkPos(key).toString() + " " + st);
        }
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string out;
    for (size_t i = 0; i < rows.size() && i < n; ++i) out += "\n    " + rows[i].second;
    return out + "\n    (" + std::to_string(rows.size()) + " tasks with >20 runs)";
}

size_t ChunkMap::processUnloads(const std::function<bool()>& haveTime,
                                const std::function<bool(int64_t)>& canUnload) {
    // Reference: ChunkMap.processUnloads / scheduleUnload (26.3). A dropped
    // holder joins MC's unloadQueue only once its save-sync future completes
    // (no generation task still works on it); the tick then unloads while
    // (minimalNumberOfChunksToProcess > 0 || haveTime()), the minimum being
    // how far that READY queue is over 2,000, counted down per unload. The
    // same here: m_pendingUnloads holds every dropped holder, and a holder
    // counts as queued once it passes the readiness checks below.
    constexpr size_t kUnloadQueueReserve = 2000;
    std::vector<int64_t> candidates;
    {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        candidates.assign(m_pendingUnloads.begin(), m_pendingUnloads.end());
    }
    if (candidates.empty()) return 0;
    // Fewer pending than the reserve means fewer ready than it too: with no
    // time left there is nothing MC would force.
    if (candidates.size() <= kUnloadQueueReserve && !haveTime()) return 0;

    const int maxLevel = ChunkLevel::getMaxLevel();

    // Whether a pending holder is ready to go (MC: in the unloadQueue). Also
    // drops the holders that left the pending set meanwhile.
    //
    // Status and the embedder's veto run OUTSIDE the map lock: the status
    // takes the holder's futures mutex, and future completions run their
    // continuations under that mutex — which can reach back into this map.
    // The holder cannot go meanwhile: only this function destroys holders,
    // on this thread.
    //
    // Java (scheduleUnload) waits for the holder's save-sync future — every
    // generation task that touched the chunk has finished — then saves the
    // chunk and drops the holder whatever its status; a later request
    // reloads it with everything it had. So does this port: a proto is
    // written to the storage (FULL chunks are the embedder's, which saves them
    // itself).
    //
    // Without storage (a read-only world) nothing can be saved. There a holder
    // is dropped only when regenerating it loses nothing: FULL (the embedder
    // owns the chunk) or before TERRAIN (only its own deterministic generation
    // touched it; decoration writes only into neighbours at TERRAIN or
    // later). A proto from TERRAIN up to FULL may hold a neighbour's
    // decoration — a tree crown or ore vein spilled across the border — that
    // the neighbour will not place again, so it stays resident.
    auto readyHolder = [&](int64_t key) -> ChunkHolder* {
        ChunkHolder* holder = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mapMutex);
            auto it = m_updatingChunkMap.find(key);
            if (it == m_updatingChunkMap.end()) { m_pendingUnloads.erase(key); return nullptr; }
            holder = it->second.get();
            if (holder->getTicketLevel() <= maxLevel) { m_pendingUnloads.erase(key); return nullptr; }   // wanted again
            if (holder->generationRefCount() != 0) return nullptr;   // a task still holds it in its cache
        }
        const ChunkStatus* latest = holder->getLatestStatus();
        const bool holdsSpills = !m_chunkIo && latest != nullptr &&
                                 !latest->isBefore(ChunkStatus::TERRAIN) &&
                                 latest->isBefore(ChunkStatus::FULL);
        if (holdsSpills) {
            std::lock_guard<std::mutex> lock(m_mapMutex);
            m_pendingUnloads.erase(key);
            return nullptr;
        }
        if (!canUnload(key)) return nullptr;             // embedder still reads it
        if (!holder->isReadyForSaving()) return nullptr; // a task still writes it
        return holder;
    };

    // The unload itself (MC: the scheduleUnload task).
    auto unload = [&](int64_t key, ChunkHolder* holder) -> bool {
        std::unique_ptr<ChunkHolder> victim;
        {
            std::lock_guard<std::mutex> lock(m_mapMutex);
            auto it = m_updatingChunkMap.find(key);
            if (it == m_updatingChunkMap.end() || it->second.get() != holder) return false;
            // Re-checked: the lane may have claimed it, or a ticket landed.
            if (holder->getTicketLevel() <= maxLevel) { m_pendingUnloads.erase(key); return false; }
            if (holder->generationRefCount() != 0) return false;
            victim = std::move(it->second);
            m_updatingChunkMap.erase(it);
            m_pendingUnloads.erase(key);
            // Out of the visible map NOW, not at the next promotion: until
            // then getChunkFutureMainThread / getChunkNow would hand out the
            // holder being destroyed. Both maps are main-thread (this is).
            // A holder created and unloaded between two promotions is still
            // on the add list — it must not be promoted afterwards either.
            m_visibleChunkMap.erase(key);
            m_visibleAdds.erase(std::remove_if(m_visibleAdds.begin(), m_visibleAdds.end(),
                                               [key, holder](const auto& add) {
                                                   return add.first == key && add.second == holder;
                                               }),
                                m_visibleAdds.end());
            m_modified = true;
        }
        // Reference: ChunkMap.scheduleUnload -> save(chunk). Out of the maps
        // already, so no new task can reach the chunk while it is copied.
        if (ChunkAccess* chunk = victim->getLatestChunk()) {
            save(chunk);
        }
        // Lifetime: ChunkHolder::updateFutures hands the dispatchers lambdas
        // that capture the holder (`[this] { return getQueueLevel(); }`), and
        // they run later on the dispatcher thread. Deleting the holder here
        // was a use-after-free (SIGSEGV in that lambda, 2026-08-30). Java has
        // a GC; we instead queue the delete BEHIND those callbacks on both
        // dispatchers (release() with clearQueue=false only sequences — it
        // does not drop queued tasks; and a holder with queued tasks never
        // gets here because they hold generation refs).
        ChunkHolder* raw = victim.release();
        auto light = m_lightTaskDispatcher.get();
        m_worldgenTaskDispatcher->release(key, [raw, light, key]() {
            auto destroy = [raw]() {
                delete raw;
                ::minecraft::util::LiveCounters::holdersDeleted().fetch_add(1, std::memory_order_relaxed);
            };
            if (light) light->release(key, destroy, false);
            else       destroy();
        }, false);
        return true;
    };

    size_t destroyed = 0;
    if (candidates.size() <= kUnloadQueueReserve) {
        // No minimum: unload ready holders while the tick has time.
        for (int64_t key : candidates) {
            if (!haveTime()) break;
            if (ChunkHolder* holder = readyHolder(key)) {
                if (unload(key, holder)) ++destroyed;
            }
        }
        return destroyed;
    }

    // Over the reserve: find the ready queue first, then force its excess.
    std::vector<std::pair<int64_t, ChunkHolder*>> ready;
    ready.reserve(candidates.size());
    for (int64_t key : candidates) {
        if (ChunkHolder* holder = readyHolder(key)) ready.emplace_back(key, holder);
    }
    size_t minimalNumberOfChunksToProcess = ready.size() > kUnloadQueueReserve ? ready.size() - kUnloadQueueReserve : 0;
    for (const auto& [key, holder] : ready) {
        if (minimalNumberOfChunksToProcess == 0 && !haveTime()) break;
        if (minimalNumberOfChunksToProcess > 0) --minimalNumberOfChunksToProcess;
        if (unload(key, holder)) ++destroyed;
    }
    return destroyed;
}

void ChunkMap::markHandedOff(int64_t key) {
    if (m_handedOffSet.insert(key).second) m_handedOff.push_back(key);
}

size_t ChunkMap::releaseHandedOffChunks(size_t maxChecks,
                                        const std::function<bool(int64_t)>& canRelease) {
    // No storage: a chunk the embedder drops cannot be read back from its
    // save, so this copy is what it would regenerate from. Keep them all.
    if (!m_chunkIo) {
        m_handedOff.clear();
        m_handedOffSet.clear();
        m_handedOffCursor = 0;
        return 0;
    }

    // Map lookups under the map lock, statuses outside it (a status takes the
    // holder's futures mutex, which must not nest inside the map lock — see
    // processUnloads). Holders are only destroyed on this thread.
    const auto holderAt = [this](int64_t key) -> ChunkHolder* {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        auto it = m_updatingChunkMap.find(key);
        return it == m_updatingChunkMap.end() ? nullptr : it->second.get();
    };
    const auto neighboursPastSpawn = [&](int64_t key) {
        const world::ChunkPos pos(key);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dz == 0) continue;
                ChunkHolder* neighbour = holderAt(world::ChunkPos::asLong(pos.x() + dx, pos.z() + dz));
                // Absent: it may yet be generated (or reloaded below SPAWN),
                // and its TERRAIN and SPAWN would read this chunk.
                if (neighbour == nullptr) return false;
                const ChunkStatus* status = neighbour->getLatestStatus();
                if (status == nullptr || status->isBefore(ChunkStatus::SPAWN)) return false;
            }
        }
        return true;
    };
    // Swap-remove: the candidate order carries no meaning.
    const auto dropCandidate = [this](size_t index) {
        m_handedOffSet.erase(m_handedOff[index]);
        m_handedOff[index] = m_handedOff.back();
        m_handedOff.pop_back();
    };

    size_t released = 0;
    for (size_t checked = 0; checked < maxChecks && !m_handedOff.empty(); ++checked) {
        if (m_handedOffCursor >= m_handedOff.size()) m_handedOffCursor = 0;
        const int64_t key = m_handedOff[m_handedOffCursor];
        ChunkHolder* holder = holderAt(key);
        auto* proto = holder ? dynamic_cast<world::ProtoChunk*>(holder->getLatestChunk()) : nullptr;
        if (proto == nullptr || proto->isBlockDataReleased()) {
            dropCandidate(m_handedOffCursor);   // unloaded, or already done
            continue;
        }
        const bool ready = holder->hasCompletedStep(ChunkStatus::FULL) &&
                           holder->generationRefCount() == 0 &&
                           canRelease(key) &&
                           neighboursPastSpawn(key);
        if (!ready) {
            ++m_handedOffCursor;
            continue;
        }
        proto->releaseBlockData();
        ++released;
        ++m_releasedChunks;
        dropCandidate(m_handedOffCursor);
    }
    return released;
}

// Reference: ChunkMap.java lines 374-381
void ChunkMap::onLevelChange(const world::ChunkPos& pos, std::function<int()> oldLevel,
                             int newLevel, std::function<void(int)> setQueueLevel) {
    static const bool kDisabled = std::getenv("OBEY_NO_LEVELCHANGE") != nullptr;   // A/B toggle
    if (kDisabled) return;
    if (m_worldgenTaskDispatcher) {
        m_worldgenTaskDispatcher->onLevelChange(pos, oldLevel, newLevel, setQueueLevel);
    }
    if (m_lightTaskDispatcher) {
        m_lightTaskDispatcher->onLevelChange(pos, oldLevel, newLevel, std::move(setQueueLevel));
    }
}

bool ChunkMap::promoteChunkMap() {
    std::lock_guard<std::mutex> lock(m_mapMutex);

    if (!m_modified) {
        return false;
    }

    // Incremental rather than Java's full rebuild (ChunkMap.java
    // promoteChunkMap): holders added since the last promotion are appended,
    // and the ones processUnloads destroyed are erased.
    for (const auto& [key, holder] : m_visibleAdds) {
        m_visibleChunkMap[key] = holder;
    }
    m_visibleAdds.clear();

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
            [this](const world::ChunkPos& p, std::function<int()> oldLevel, int newLevel,
                   std::function<void(int)> setQueueLevel) {
                onLevelChange(p, std::move(oldLevel), newLevel, std::move(setQueueLevel));
            },
            // playerProvider - no-op
            [](const world::ChunkPos&, bool) { return std::vector<void*>{}; }
        );
        chunk = holder.get();
        m_updatingChunkMap[node] = std::move(holder);
        m_visibleAdds.emplace_back(node, chunk);
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

        // MC: postProcessGeneration and startTickingChunk are the embedder's
        // (it owns the world the chunk ticks in). onChunkReadyToSend runs on
        // the main thread (thenApplyAsync(..., mainThreadExecutor)); the
        // holder is looked up again there, since a level change in between
        // may already have dropped it.
        const int64_t key = chunk->getPos().toLong();
        m_mainThreadExecutor([this, key]() { onChunkReadyToSend(key); });

        return ChunkResult<ChunkAccess*>::of(levelChunk);
    });
}

void ChunkMap::setChunkReadyListener(ChunkReadyListener listener) {
    m_chunkReadyListener = std::move(listener);
}

void ChunkMap::onChunkReadyToSend(int64_t key) {
    // Reference: ChunkMap.onChunkReadyToSend — the chunk and its 3x3 are
    // FULL (BLOCK_TICKING), so the players tracking it get it, whichever
    // ticket brought it there. Main thread.
    if (!m_chunkReadyListener) return;
    ChunkHolder* holder = getVisibleChunkIfPresent(key);
    if (holder == nullptr || !holder->hasCompletedStep(ChunkStatus::FULL)) return;
    ChunkAccess* chunk = holder->getLatestChunk();
    if (chunk == nullptr) return;
    m_chunkReadyListener(key, chunk);
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
