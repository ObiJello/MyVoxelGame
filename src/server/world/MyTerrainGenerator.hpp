// File: src/server/world/MyTerrainGenerator.hpp
#pragma once

#include "common/world/gen/IChunkGenerator.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Log.hpp"
#include <unordered_map>
#include <cstdlib>

// Terrain library includes
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/FlatLevelSource.h"
#include "levelgen/structure/ChunkGeneratorStructureState.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/placement/PlacedFeature.h"
#include "world/ProtoChunk.h"
#include "common/core/ThreadPriority.hpp"
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

        explicit BackgroundExecutor(size_t numThreads = DefaultThreadCount(), bool elevated = false)
            : m_running(true), m_elevated(elevated)
        {
            for (size_t i = 0; i < numThreads; ++i) {
                m_workers.emplace_back([this, i]() { workerLoop(i); });
            }
        }
        bool m_elevated = false;

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


        // Diagnostics: per worker thread, the time of a fixed spin loop run
        // every 25 tasks (a direct read of how fast the core it currently sits
        // on is — an efficiency core is ~2x slower) and the CPU-time/wall-time
        // ratio of its tasks (below 1 = preempted).
        std::string DebugProbe() const {
            std::string out;
            for (size_t i = 0; i < m_workers.size() && i < kMaxProbe; ++i) {
                const int64_t p = m_probeNs[i].load(std::memory_order_relaxed);
                const int64_t cpu = m_cpuNs[i].load(std::memory_order_relaxed);
                const int64_t wall = m_wallNs[i].load(std::memory_order_relaxed);
                char buf[64];
                std::snprintf(buf, sizeof buf, " t%zu:%.2fms/%.2f", i, p / 1e6, wall > 0 ? double(cpu) / double(wall) : 0.0);
                out += buf;
            }
            return out;
        }
    private:
        static constexpr size_t kMaxProbe = 16;
        static inline const bool s_probeEnabled = std::getenv("OBEY_POOL_PROBE") != nullptr;
        std::array<std::atomic<int64_t>, kMaxProbe> m_probeNs{};
        std::array<std::atomic<int64_t>, kMaxProbe> m_cpuNs{};
        std::array<std::atomic<int64_t>, kMaxProbe> m_wallNs{};
        static int64_t ThreadCpuNs() {
            timespec ts{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
        }
        static int64_t RunProbe() {
            const auto t0 = std::chrono::steady_clock::now();
            uint64_t x = 0x9E3779B97F4A7C15ull;
            for (int i = 0; i < 3000000; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
            volatile uint64_t sink = x; (void)sink;
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        }

        void workerLoop(size_t index) {
            int taskCount = 0;
            // Without a name these threads show up in Tracy as bare numeric ids
            // with no zones — which is exactly why the most expensive work in the
            // program stayed invisible across several captures.
            TERRAIN_THREAD("TerrainWorker");
            if (m_elevated) Core::SetCurrentThreadPriority(Core::ThreadPriorityClass::Elevated);   // decoration pool: performance cores
            // QoS stays DEFAULT on purpose. Elevated (tried 2026-08-30) put all
            // nine threads on the four performance cores in competition with
            // the render thread and the serial worldgen lane, and fresh
            // generation measured ~10% SLOWER (2,540 vs 2,762 chunks / 30 s
            // on a cool machine); at default the bulk noise work spreads onto
            // the efficiency cores and the lane keeps a fast core.

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
                    const auto w0 = std::chrono::steady_clock::now();
                    const int64_t c0 = ThreadCpuNs();
                    try { task(); } catch (...) {}
                    if (index < kMaxProbe) {
                        m_cpuNs[index].fetch_add(ThreadCpuNs() - c0, std::memory_order_relaxed);
                        m_wallNs[index].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - w0).count(), std::memory_order_relaxed);
                        // OFF unless OBEY_POOL_PROBE is set. Measured 2026-08-29
                        // (Tracy, GL fresh-gen phase): the pool runs ~5,700
                        // tasks/s, so "every 25 tasks" was 5,352 spins of 4-13 ms
                        // in 26 s — 33.5 s of CPU, 23% of the whole terrain
                        // pool's busy time, spent measuring core speed.
                        if (s_probeEnabled && ++taskCount % 25 == 0) {
                            m_probeNs[index].store(RunProbe(), std::memory_order_relaxed);
                        }
                    }
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
     * The ONE worldgen thread pool for the whole process.
     *
     * There is a generator per dimension (overworld/nether/end), and each one
     * used to build its own BackgroundExecutor. That is hardware_concurrency()-1
     * threads EACH: 27 worldgen threads on a 10-core Mac, on top of 4
     * ServerWorkers, 3 MeshWorkers, the server thread, the occlusion thread and
     * the main thread — every one of them fighting for the same 10 cores, which
     * makes the pool's own -1 (see DefaultThreadCount above) meaningless.
     *
     * Deliberately leaked rather than a plain function-local static object: the
     * generators are owned by World/ChunkProvider, and nothing guarantees they
     * are destroyed before this translation unit's statics are. A generator
     * outliving the pool would hand ServerChunkCache an executor referencing a
     * destroyed object; leaking costs one idle condition-variable wait per
     * thread and the OS reclaims them at exit.
     */
    BackgroundExecutor& SharedBackgroundExecutor();
    BackgroundExecutor& SharedLaneExecutor();

    /**
     * A single generator's claim on the shared pool.
     *
     * The shared pool cannot be shut down to fence a teardown — the other
     * dimensions are still using it — but the guarantee the old per-generator
     * `m_backgroundExecutor.reset()` provided is still required: when Shutdown
     * deletes the ServerChunkCache, no worker thread may still be inside a task
     * that touches it. This lease reproduces exactly that, per generator:
     *
     *   - every task submitted through it is counted,
     *   - closeAndWait() blocks until the count reaches zero (the old
     *     shutdown()+join()),
     *   - submissions after the close are DROPPED, matching the old executor,
     *     whose queue was destroyed with it (a ServerChunkCache destructor that
     *     schedules background work must not resurrect the pipeline).
     *
     * Not merged into BackgroundExecutor: the pool has no idea which pipeline a
     * task belongs to, and a global drain would make one dimension's shutdown
     * wait on another dimension's generation queue.
     */
    class SharedExecutorLease {
    public:
        using Task = std::function<void()>;

        ~SharedExecutorLease() { closeAndWait(); }

        void submit(Task task) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_closed) return;
                ++m_pending;
            }
            SharedBackgroundExecutor().submit([this, task = std::move(task)]() {
                // The pool swallows exceptions around the whole lambda, so the
                // decrement has to be protected here — losing one would hang
                // closeAndWait() forever.
                try {
                    task();
                } catch (...) {
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                if (--m_pending == 0) {
                    m_cv.notify_all();
                }
            });
        }

        // Stop accepting work and block until everything already submitted has
        // finished. Idempotent; also run by the destructor.
        void closeAndWait() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_closed = true;
            m_cv.wait(lock, [this] { return m_pending == 0; });
        }

        std::function<void(std::function<void()>)> getExecutor() {
            return [this](std::function<void()> task) {
                this->submit(std::move(task));
            };
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_cv;
        int  m_pending = 0;
        bool m_closed = false;
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
        bool IsAbortRequested() const override;

        // MC ChunkGenerator's ChunkGeneratorStructureState — where the
        // structure placements and, for strongholds, the precomputed ring
        // positions live. Null before Initialize.
        //
        // Exposed for the Eye of Ender, which needs the stronghold ring
        // positions and nothing else. Read-only in spirit: the state is built
        // once during Initialize and its ring positions are seed-derived, so a
        // caller cannot make it disagree with the terrain.
        minecraft::levelgen::structure::ChunkGeneratorStructureState*
        GetStructureState() const { return m_structureState.get(); }

        // The library generator, its RandomState (climate sampler) and biome
        // source, for /locate: MC's findNearestMapStructure runs a structure's
        // own findValidGenerationPoint at each candidate chunk, and
        // findClosestBiome3d samples the biome source — both need these. Null
        // before Initialize. Read-only in spirit, like GetStructureState.
        minecraft::levelgen::ChunkGenerator* GetLibGenerator() const { return m_generator; }
        // The generated terrain's surface height at a column (MC
        // ChunkGenerator.getBaseHeight, WORLD_SURFACE_WG): the Y a player
        // would stand at, computed from the noise with no chunk loaded.
        // INT_MIN before Initialize.
        int SurfaceHeightAt(int blockX, int blockZ) const;
        minecraft::levelgen::RandomState*    GetRandomState()   const { return m_randomState; }
        minecraft::world::biome::BiomeSource* GetBiomeSource()  const { return m_biomeSource.get(); }

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
        // The completion lands in the sink (TakeCompletions) from whatever
        // thread finishes the last step.
        bool RequestChunkGeneration(Math::ChunkPos position);

        // ── Ticket-driven request path (MC ChunkMap model, 2026-08-30) ──
        // Any thread: a chunk the disk did not have. Server thread drains with
        // TakeRequests and turns each into RequestChunkGeneration.
        void EnqueueGenerationRequest(Math::ChunkPos position);
        void TakeRequests(std::vector<Math::ChunkPos>& out);

        struct Completion {
            Math::ChunkPos position;
            minecraft::world::IChunk* chunk = nullptr;   // null = generation failed
        };
        void TakeCompletions(std::vector<Completion>& out);

        // Any thread: library chunk -> game chunk. The holder must stay alive
        // until this returns (nothing unloads yet; see PinConversion for when
        // it does).
        std::shared_ptr<Chunk> ConvertCompletedChunk(minecraft::world::IChunk* chunk,
                                                     Math::ChunkPos position);
        // MC DistanceManager player tickets: one radius ticket per viewer at
        // their chunk, so ticket levels form a distance gradient and the
        // library generates nearest-first. Server thread.
        void SetViewTicket(uint32_t viewerId, Math::ChunkPos center, int radius);
        void ClearViewTicket(uint32_t viewerId);

        void PinConversion(Math::ChunkPos position);
        void UnpinConversion(Math::ChunkPos position);
        bool IsConversionPinned(Math::ChunkPos position) const;

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

        // Drop the cached NoiseChunk of every holder that is not FULL and has
        // no generation task in flight. See the .cpp for why. Server thread.
        // Returns how many were released.
        // Budgeted like MC ChunkMap.processUnloads: candidates are collected
        // on a rescan (every ~100 calls), then freed a few at a time until
        // `deadline` passes, with a small floor so a late tick still makes
        // progress. Freeing 800 in one tick measured 850 ms (2026-08-29).
        size_t ReleaseIdleNoiseChunks(std::chrono::steady_clock::time_point deadline);

        // Once per server tick (MC ServerChunkCache.tick): expire stale
        // tickets, propagate levels, and destroy holders nobody's ticket
        // covers any more — bounded by `deadline`. Returns holders unloaded.
        size_t TickLibrary(std::chrono::steady_clock::time_point deadline);
        struct UnloadDiag { size_t pendingUnload = 0, refHeld = 0, pinned = 0, aboveMax = 0; };
        UnloadDiag GetUnloadDiag() const;

        // Chunk holders the library's ChunkMap currently owns (every chunk it
        // ever generated or touched — the UNKNOWN tickets it holds never
        // expire because nothing calls its tick()). Diagnostics; server thread.
        size_t LibraryChunkCount() const {
            return m_chunkCache ? m_chunkCache->getChunkMap().size() : 0;
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
        // Base type: default/amplified/large_biomes/single_biome are
        // NoiseBasedChunkGenerator, flat is FlatLevelSource.
        minecraft::levelgen::ChunkGenerator* m_generator = nullptr;
        // MultiNoise for default/amplified/large_biomes, FixedBiomeSource for
        // single_biome; null for flat (FlatLevelSource owns its own).
        std::unique_ptr<minecraft::world::biome::BiomeSource> m_biomeSource;
        minecraft::world::BlockRegistry* m_blockRegistry = nullptr;
        minecraft::BlockState* m_airBlock = nullptr;
        minecraft::BlockState* m_stoneBlock = nullptr;

        // Structure placement state (MC ChunkGeneratorStructureState), built
        // in Initialize only when m_config.generateStructures is set. The
        // pipeline's WorldGenContext holds a raw pointer to this, so it must
        // outlive m_chunkCache (declared before it; Shutdown resets the cache
        // first).
        std::unique_ptr<minecraft::levelgen::structure::ChunkGeneratorStructureState> m_structureState;

        // Full async pipeline components (same as async_chunk_test).
        //
        // The background pool is shared process-wide (see
        // SharedBackgroundExecutor); this generator owns only its lease on it.
        // The main-thread queue stays PER GENERATOR and is not hoisted: it
        // spawns no threads at all, so sharing it would save nothing, and it is
        // drained from whichever thread is servicing this pipeline — the server
        // thread via PumpOneTask, but also any ServerWorker blocked inside
        // ServerChunkCache::getChunk, through the task poller Initialize
        // installs on the cache. One shared
        // queue would therefore let a worker blocked on the overworld run the
        // nether's ChunkMap/DistanceManager tasks concurrently with the server
        // thread pumping them, and would leave a shut-down generator's queued
        // tasks alive in another dimension's queue.
        // Completion sink shared with the futures' callbacks so a callback
        // firing after this generator is destroyed writes into an orphan
        // rather than freed memory.
        struct CompletionSink {
            std::mutex mutex;
            std::vector<Completion> completions;
            bool closed = false;
            std::function<void()> wake;   // pokes the server thread's park
        };
        std::shared_ptr<CompletionSink> m_sink = std::make_shared<CompletionSink>();
        struct ViewTicket { Math::ChunkPos center; int radius; };
        std::unordered_map<uint32_t, ViewTicket> m_viewTickets;
        std::mutex m_requestMutex;
        std::vector<Math::ChunkPos> m_requests;
        mutable std::mutex m_pinMutex;
        std::unordered_set<Math::ChunkPos, Math::ChunkPosHash> m_pinned;

        std::vector<int64_t> m_noiseReleaseQueue;   // holder keys awaiting release
        // key -> consecutive rescans (5 s apart) seen idle. A ring chunk that
        // is still being advanced by nearby tasks is idle only between two
        // tasks; releasing its NoiseChunk then costs a ~10 ms rebuild per
        // layer, which made the last rim chunks of an area take a minute.
        std::unordered_map<int64_t, int> m_noiseIdleScans;
        int                  m_noiseReleaseRescan = 0;

        std::unique_ptr<SharedExecutorLease> m_backgroundLease;
        std::unique_ptr<BackgroundExecutor> m_decorationPool;   // OBEY_DECO_THREADS=n: elevated-QoS decoration threads
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
            BlockStateIndex state = 0;
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
