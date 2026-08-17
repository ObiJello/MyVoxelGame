// File: src/server/world/MyTerrainGenerator.hpp
#pragma once

#include "common/world/gen/IChunkGenerator.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Log.hpp"
#include <unordered_map>

// Terrain library includes
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/placement/PlacedFeature.h"
#include "world/ProtoChunk.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/chunk/status/ChunkStatus.h"

// Server-level includes (the async pipeline)
#include "server/level/ServerChunkCache.h"
#include "util/TerrainProfiling.h"

#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>

namespace Game {

    /**
     * Background thread pool executor - simulates Minecraft's Util.backgroundExecutor()
     * Reference: Minecraft uses ForkJoinPool.commonPool() for background work
     *
     * THIS POOL DOES THE ACTUAL TERRAIN GENERATION. ChunkMap wires it in as both
     * the "worldgen" and "light" executor, so every noise/surface/carver/feature
     * pass runs here — not on the ServerWorker that called GetChunk. That caller
     * is off the library's main thread, so ServerChunkCache::getChunk enqueues
     * and then sleep-polls in 100us steps until this pool finishes.
     *
     * Consequence for profiling: the game-side TerrainLibGetChunk zone measures
     * the caller's WAIT, not work. Real cost lives on these threads, which is why
     * they are named below — before that they emitted nothing and were invisible
     * in every capture we took.
     */
    class BackgroundExecutor {
    public:
        using Task = std::function<void()>;

        // MC parity: Util.maxAllowedExecutorThreads() is
        //   clamp(availableProcessors - 1, 1, getMaxThreads())
        // (minecraft_code/decompiled_net/minecraft/util/Util.java:177).
        // The -1 matters — it leaves a core for the thread waiting on the result.
        // We previously took hardware_concurrency() flat, which on a 10-core M4
        // put 10 worldgen threads on top of 4 ServerWorkers, 3 MeshWorkers, the
        // server thread, the occlusion thread and the main thread.
        static size_t DefaultThreadCount() {
            const unsigned hw = std::thread::hardware_concurrency();
            if (hw == 0) return 1;
            return static_cast<size_t>(std::max(1u, hw - 1u));
        }

        explicit BackgroundExecutor(size_t numThreads = DefaultThreadCount())
            : m_running(true)
        {
            for (size_t i = 0; i < numThreads; ++i) {
                m_workers.emplace_back([this]() { workerLoop(); });
            }
        }

        ~BackgroundExecutor() { shutdown(); }

        void shutdown() {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_running = false;
            }
            m_cv.notify_all();
            for (auto& worker : m_workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        }

        void submit(Task task) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_tasks.push(std::move(task));
            }
            m_cv.notify_one();
        }

        std::function<void(std::function<void()>)> getExecutor() {
            return [this](std::function<void()> task) {
                this->submit(std::move(task));
            };
        }

    private:
        void workerLoop() {
            // Without a name these threads show up in Tracy as bare numeric ids
            // with no zones — which is exactly why the most expensive work in the
            // program stayed invisible across several captures.
            TERRAIN_THREAD("TerrainWorker");

            while (true) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this]() { return !m_running || !m_tasks.empty(); });
                    if (!m_running && m_tasks.empty()) return;
                    if (!m_tasks.empty()) {
                        task = std::move(m_tasks.front());
                        m_tasks.pop();
                    }
                }
                if (task) {
                    // Wraps the whole task so a thread's occupancy is visible even
                    // for stages that carry no zone of their own. The per-stage
                    // breakdown comes from ChunkStatusTasks.h nested inside this.
                    TERRAIN_ZONE_N("TerrainTask");
                    try { task(); } catch (...) {}
                }
            }
        }

        std::vector<std::thread> m_workers;
        std::queue<Task> m_tasks;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::atomic<bool> m_running;
    };

    /**
     * Main thread executor for tasks that must run on the main thread
     */
    class MainThreadExecutor {
    public:
        using Task = std::function<void()>;

        void submit(Task task) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_tasks.push(std::move(task));
            }
            // MC's LockSupport.unpark half of park/unpark: the server thread
            // sleeps until the next tick deadline and is woken the instant a
            // worker hands it something to run.
            m_cv.notify_one();
        }

        // Block until a task is available or `deadline` passes. Returns true if
        // there is work.
        //
        // MC BlockableEventLoop.waitForTasks, as called from waitUntilNextTick:
        //
        //     long waitNanos = this.waitingForNextTick
        //         ? this.nextTickTimeNanos - Util.getNanos() : 100000L;
        //     LockSupport.parkNanos("waiting for tasks", waitNanos);
        //
        // i.e. inside the idle window it parks for the WHOLE remaining time and
        // relies on submit() to wake it, rather than polling. Polling at a fixed
        // interval instead costs a wakeup every interval for an empty queue
        // check, and adds up to that interval of latency to every chunk handed
        // back — which is the exact cost this pump exists to remove.
        template <class Clock, class Duration>
        bool waitForTasks(const std::chrono::time_point<Clock, Duration>& deadline) {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_until(lock, deadline, [this] { return !m_tasks.empty(); });
        }

        // Wake anything parked in waitForTasks (shutdown).
        void wakeAll() { m_cv.notify_all(); }

        void runPendingTasks() {
            std::queue<Task> tasksToRun;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                std::swap(tasksToRun, m_tasks);
            }
            while (!tasksToRun.empty()) {
                tasksToRun.front()();
                tasksToRun.pop();
            }
        }

        // Run at most ONE queued task; returns false when the queue was empty.
        //
        // This is the granularity MC's main-thread pump works at
        // (BlockableEventLoop.pollTask -> one task, then back to the caller's
        // deadline check). runPendingTasks above drains the whole queue with no
        // way to stop, which is fine for a caller that has already decided to
        // block, but is exactly what stops a deadline from being honoured.
        // The lock is released before the task runs — a task may submit more.
        bool runOnePendingTask() {
            Task task;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_tasks.empty()) return false;
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            task();
            return true;
        }

        bool hasPendingTasks() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return !m_tasks.empty();
        }

        std::function<void(std::function<void()>)> getExecutor() {
            return [this](std::function<void()> task) {
                this->submit(std::move(task));
            };
        }

    private:
        std::queue<Task> m_tasks;
        mutable std::mutex m_mutex;   // mutable so hasPendingTasks() can be const
        std::condition_variable m_cv;
    };

    /**
     * Wrapper generator that integrates the terrain library with the game's
     * IChunkGenerator interface using the FULL ServerChunkCache async pipeline.
     *
     * Pipeline: ServerChunkCache -> ChunkMap -> DistanceManager ->
     *           ChunkGenerationTask -> Worker Threads
     *
     * This matches the exact same flow as Minecraft's DedicatedServer and
     * the async_chunk_test parity test.
     */
    class MyTerrainGenerator : public IChunkGenerator {
    public:
        explicit MyTerrainGenerator(const GenerationConfig& config);
        ~MyTerrainGenerator() override;

        // IChunkGenerator interface implementation
        bool Initialize() override;
        void Shutdown() override;

        // Signal the terrain library to abort blocking getChunk() loops (for clean shutdown)
        void RequestAbort() override;

        // MC MinecraftServer.setInitialSpawn(): climate SpawnFinder picks the
        // (x, z) region, then a spiral over the surrounding chunks finds the
        // first dry-land column (surface above sea level) via getBaseHeight.
        // Self-initializes the generator if needed (safe on server thread).
        glm::ivec3 FindSpawnPosition() override;

        ChunkGenerationResult GenerateChunk(Math::ChunkPos position) override;
        std::future<ChunkGenerationResult> GenerateChunkAsync(Math::ChunkPos position) override;
        std::vector<int> GenerateHeightMap(Math::ChunkPos position) override;
        std::string GenerateBiome(Math::ChunkPos position) override;

        void SetConfig(const GenerationConfig& config) override;
        GenerationConfig GetConfig() const override;
        void SetSeed(int64_t seed) override;
        int64_t GetSeed() const override;
        void SetWorldType(const std::string& worldType) override;
        std::string GetWorldType() const override;

        void SetPassEnabled(GenerationPass pass, bool enabled) override;
        bool IsPassEnabled(GenerationPass pass) const override;
        ChunkGenerationResult GenerateWithPasses(Math::ChunkPos position, const std::vector<GenerationPass>& passes) override;

        bool IsReady() const override;

        GeneratorStats GetStats() const override;
        void ResetStats() override;
        void SetMaxGenerationTime(float maxTimeMs) override;
        float GetMaxGenerationTime() const override;

        void RegisterTerrainFunction(const std::string& name, TerrainFunction func) override;
        void RegisterFeatureFunction(const std::string& name, FeatureFunction func) override;
        void SetTerrainFunction(const std::string& name) override;
        void AddFeatureFunction(const std::string& name) override;

        DebugInfo GetDebugInfo(Math::ChunkPos position) override;
        void SetDebugMode(bool enabled) override;
        bool IsDebugMode() const override;

        std::string GetLastError() const override;
        void ClearErrors() override;

        // === Non-blocking async API (must be called from server thread) ===

        // Request chunk generation without blocking. Returns true if request was queued.
        bool RequestChunkGeneration(Math::ChunkPos position);

        // Run ONE unit of the chunk pipeline. Returns true if work was done, so
        // the caller can loop until either the pipeline is idle or its own
        // deadline expires.
        //
        // Direct port of MC ServerChunkCache.MainThreadExecutor.pollTask
        // (minecraft_code/.../server/level/ServerChunkCache.java:583):
        //
        //     protected boolean pollTask() {
        //        if (ServerChunkCache.this.runDistanceManagerUpdates()) return true;
        //        else { lightEngine.tryScheduleUpdate(); return super.pollTask(); }
        //     }
        //
        // ONE unit, then back to the caller — which is what lets MC's
        // managedBlock(() -> !haveTime()) re-check the clock before every single
        // one. This replaced a `while (didWork && iterations < 256)` loop that
        // checked no clock at all: runDistanceUpdates is unbounded (it propagates
        // every outstanding ticket) and promoteChunkMap rebuilds the whole
        // visible chunk map per pass, so at world entry — 1369 chunks ticketed in
        // one tick — a single call could run for SECONDS. Measured: five, during
        // which the server thread completed no tick, sent no chunk, and the
        // already-generated spawn chunk sat in the cache unsent.
        //
        // MUST run on the server thread: these touch ChunkMap/DistanceManager,
        // which the terrain library treats as main-thread-only.
        bool PumpOneTask();

        // Park until the pipeline has work or `deadline` passes (MC
        // BlockableEventLoop.waitForTasks). Server thread only.
        bool WaitForPipelineWork(std::chrono::steady_clock::time_point deadline) {
            return m_mainThreadExecutor && m_mainThreadExecutor->waitForTasks(deadline);
        }

        // Check if a chunk is ready (fully generated). Non-blocking. Must call from server thread.
        bool IsChunkReady(Math::ChunkPos position);

        // Get a completed chunk without blocking. Returns nullptr if not ready.
        // Converts from terrain library format to Game::Chunk.
        std::shared_ptr<Chunk> GetCompletedChunk(Math::ChunkPos position);

    private:
        GenerationConfig m_config;
        GeneratorStats m_stats;
        bool m_initialized = false;

        // Terrain library components
        minecraft::levelgen::NoiseGeneratorSettings* m_settings = nullptr;
        minecraft::levelgen::RandomState* m_randomState = nullptr;
        // Value storage behind the ClimateParameterPoint* list handed to
        // m_settings (the spawn-target climates for Climate::SpawnFinder).
        // Must outlive m_settings/m_randomState — freed together in Shutdown.
        std::vector<minecraft::world::biome::Climate::ParameterPoint> m_spawnTargetStorage;
        minecraft::levelgen::FluidPicker* m_fluidPicker = nullptr;
        minecraft::levelgen::NoiseBasedChunkGenerator* m_generator = nullptr;
        std::unique_ptr<minecraft::world::biome::MultiNoiseBiomeSource> m_biomeSource;
        minecraft::world::BlockRegistry* m_blockRegistry = nullptr;
        minecraft::BlockState* m_airBlock = nullptr;
        minecraft::BlockState* m_stoneBlock = nullptr;

        // Full async pipeline components (same as async_chunk_test)
        std::unique_ptr<BackgroundExecutor> m_backgroundExecutor;
        std::unique_ptr<MainThreadExecutor> m_mainThreadExecutor;
        std::unique_ptr<minecraft::server::level::ServerChunkCache> m_chunkCache;

        // Target chunk status for generation
        const minecraft::world::chunk::status::ChunkStatus* m_targetStatus = nullptr;

        // Helper to map block types to game BlockIDs. Lock-free: uses a
        // thread_local Block*→BlockID cache plus a last-block memo (terrain is
        // dominated by runs of identical states, so the memo absorbs most
        // lookups). The cache is epoch-guarded — Blocks::bootstrap() in
        // Initialize() can recreate Block objects on world reload, so each
        // worker's cache resets when the generator epoch advances. Replaces
        // the old mutex-protected member cache, which took ~98k lock/unlock
        // per converted chunk under contention from all worker threads.
        // A generated voxel: the block plus its index into that block's own
        // state list (BlockRegistry::GetStateDefinition). The state half is
        // what keeps a generated furnace's facing, a log's axis and a leaf
        // litter clump's rotation — the library hands us a fully-propertied
        // BlockState and dropping it collapsed whole families onto their
        // default state.
        struct MappedBlock {
            BlockID id    = BlockID::Air;
            uint8_t state = 0;
        };

        MappedBlock MapBlockType(minecraft::world::BlockState* blockState) const;

        // Library Biome* -> our BiomeId, cached per worker thread under the
        // same bootstrap epoch as MapBlockType (Biome objects are re-created by
        // the registry on world reload).
        uint16_t MapBiome(const void* libBiome, const std::string& name) const;

        // Shared conversion: terrain-library chunk → game chunk. Iterates
        // section-wise (skipping all-air sections entirely) and writes
        // directly into game ChunkSection arrays. Used by GenerateChunk and
        // GetCompletedChunk.
        // One library section -> one game section. Palette-to-palette where the
        // shapes line up, per-voxel otherwise. See the .cpp for why the
        // per-voxel path is kept.
        int  ConvertSection(const minecraft::world::LevelChunkSection& libSection,
                            ChunkSection& outSection) const;
        bool TryConvertSectionByPalette(const minecraft::world::LevelChunkSection& libSection,
                                        ChunkSection& outSection, int& outNonAir) const;
        int  ConvertSectionPerVoxel(const minecraft::world::LevelChunkSection& libSection,
                                    ChunkSection& outSection) const;

        std::shared_ptr<Chunk> ConvertLibChunk(minecraft::world::IChunk* chunk,
                                               Math::ChunkPos position,
                                               int* outBlocksSet) const;
    };

} // namespace Game
