// File: src/server/world/ServerWorkerPool.cpp
#include "ServerWorkerPool.hpp"
#include "MyTerrainGenerator.hpp"
#include "common/core/Log.hpp"
#include "common/core/ThreadPriority.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/level/World.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "server/IntegratedServer.hpp"
#include "server/level/ServerLevel.hpp"
#include "platform/GameDirectory.hpp"
#include <algorithm>
#include <future>
#include <limits>

namespace Threading {

    // Global instance
    std::unique_ptr<ServerWorkerPool> g_serverWorkerPool = nullptr;

    // Static result queue for chunk generation results
    static Network::ResultQueue<Network::ChunkGenResult> s_chunkGenResultQueue;

    namespace {
        // The World a job's dimension belongs to, or null if that level does
        // not exist (the server is shutting down, or the dimension's generator
        // never started).
        //
        // Resolved per job rather than cached because one pool serves three
        // levels. Reading m_levels from a worker is safe for the SAME reason
        // the old cached pointer was: a job for dimension D is only ever
        // enqueued after IntegratedServer has finished building D's level, and
        // the enqueue and the dequeue both pass through m_jobQueueMutex — so
        // the slot's write happens-before this read, and the slot is never
        // rewritten afterwards.
        Game::World* WorldForJob(Game::DimensionId dimension) {
            if (!Server::g_integratedServer) return nullptr;
            Server::ServerLevel* level = Server::g_integratedServer->GetLevel(dimension);
            return level ? level->World() : nullptr;
        }
    } // namespace

    ServerWorkerPool::ServerWorkerPool(size_t workerCount)
        : m_workerCount(workerCount) {
        Log::Info("ServerWorkerPool created with %zu workers", workerCount);
    }

    ServerWorkerPool::~ServerWorkerPool() {
        if (m_running.load()) {
            Shutdown();
        }
        Log::Info("ServerWorkerPool destroyed");
    }

    void ServerWorkerPool::Initialize() {
        if (m_running.load()) {
            Log::Warning("ServerWorkerPool already running");
            return;
        }

        // No world is captured here any more — a job names its dimension and
        // the level is resolved when it runs (see WorldForJob). The overworld
        // is still required to exist, because a pool started before the server
        // has a world would only ever be handed jobs it cannot serve.
        if (!Server::g_integratedServer) {
            Log::Error("Cannot initialize ServerWorkerPool: IntegratedServer not initialized");
            return;
        }

        if (!Server::g_integratedServer->GetWorld()) {
            Log::Error("Cannot initialize ServerWorkerPool: IntegratedServer has no world");
            return;
        }

        // No queue capacity to configure: the queue is unbounded, like MC's
        // ChunkTaskDispatcher, and self-limiting because a chunk can only be
        // in flight once. See EnqueueJob.

        m_running.store(true);

        Log::Info("Starting %zu server worker threads...", m_workerCount);

        // Start worker threads
        m_workerThreads.reserve(m_workerCount);
        for (size_t i = 0; i < m_workerCount; ++i) {
            m_workerThreads.emplace_back([this]() { WorkerLoop(); });
        }

        Log::Info("ServerWorkerPool initialized successfully");
    }

    void ServerWorkerPool::Shutdown() {
        if (!m_running.load()) {
            return;
        }

        Log::Info("Shutting down ServerWorkerPool...");

        // Signal all threads to stop
        m_running.store(false);
        m_jobCondition.notify_all();

        // Wait for all worker threads to finish.
        // Workers in blocking getChunk() will exit via ServerChunkCache::requestAbort()
        // which is called from World::RequestStop() before we get here.
        for (auto& thread : m_workerThreads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        m_workerThreads.clear();

        // Clear remaining jobs
        CancelAllJobs();

        // Log final statistics
        LogStats();

        Log::Info("ServerWorkerPool shutdown complete");
    }

    void ServerWorkerPool::SubmitChunkGeneration(Game::DimensionId dimension,
                                                 Game::Math::ChunkPos chunkPos, int priority) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit chunk generation job - ServerWorkerPool not running");
            return;
        }

        uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            generation = ++m_chunkGenerations[Game::DimensionSlot(dimension)][chunkPos];
        }

        ServerJob job(ServerJobType::CHUNK_GENERATION, chunkPos, dimension);
        job.priority = priority;
        job.generationId = generation;
        job.task = [this, dimension, chunkPos, generation]() {
            ProcessChunkGeneration(dimension, chunkPos, generation);
        };

        EnqueueJob(std::move(job));
        m_stats.jobsSubmitted.fetch_add(1, std::memory_order_relaxed);
    }

    bool ServerWorkerPool::SubmitChunkLoading(Game::DimensionId dimension,
                                              Game::Math::ChunkPos chunkPos, int priority) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit chunk loading job - ServerWorkerPool not running");
            return false;
        }

        const int slot = Game::DimensionSlot(dimension);

        uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            generation = ++m_chunkGenerations[slot][chunkPos];
        }

        ServerJob job(ServerJobType::CHUNK_LOADING, chunkPos, dimension);
        job.priority = priority;
        job.generationId = generation;
        job.task = [this, dimension, chunkPos, generation]() {
            ProcessChunkLoading(dimension, chunkPos, generation);
        };

        if (!EnqueueJob(std::move(job))) {
            // Roll the generation back. The bump above marks every older job
            // for this chunk stale, and a stale job returns WITHOUT sending a
            // result (ProcessChunkLoading) — so leaving the bump in place after
            // a failed enqueue would silently kill an in-flight load and leave
            // the requester waiting on a result that can never arrive. Only
            // undo it if nobody else bumped in the meantime.
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            auto& generations = m_chunkGenerations[slot];
            auto it = generations.find(chunkPos);
            if (it != generations.end() && it->second == generation) {
                --it->second;
            }
            return false;
        }

        m_stats.jobsSubmitted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void ServerWorkerPool::SubmitChunkSaving(Game::DimensionId dimension,
                                             Game::Math::ChunkPos chunkPos,
                                             std::shared_ptr<Game::Chunk> chunk, int priority) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit chunk saving job - ServerWorkerPool not running");
            return;
        }

        if (!chunk) {
            Log::Warning("Cannot submit chunk saving job with null chunk");
            return;
        }

        ServerJob job(ServerJobType::CHUNK_SAVING, chunkPos, dimension);
        job.priority = priority;
        job.task = [this, dimension, chunkPos, chunk]() {
            ProcessChunkSaving(dimension, chunkPos, chunk);
        };

        EnqueueJob(std::move(job));
        m_stats.jobsSubmitted.fetch_add(1, std::memory_order_relaxed);
    }

    void ServerWorkerPool::SubmitWorldIOJob(std::function<void()> task, int priority) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit world I/O job - ServerWorkerPool not running");
            return;
        }

        if (!task) {
            Log::Warning("Cannot submit null world I/O job");
            return;
        }

        ServerJob job(ServerJobType::WORLD_IO, Game::Math::ChunkPos{0, 0});
        job.priority = priority;
        job.task = std::move(task);

        EnqueueJob(std::move(job));
        m_stats.jobsSubmitted.fetch_add(1, std::memory_order_relaxed);
    }

    void ServerWorkerPool::CancelChunkJobs(Game::DimensionId dimension,
                                           Game::Math::ChunkPos chunkPos) {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        auto& generations = m_chunkGenerations[Game::DimensionSlot(dimension)];
        ++generations[chunkPos];
        Log::Debug("Cancelled jobs for %s chunk (%d, %d) (gen %llu)",
                   std::string(Game::DimensionName(dimension)).c_str(),
                   chunkPos.x, chunkPos.z,
                   static_cast<unsigned long long>(generations[chunkPos]));
    }

    void ServerWorkerPool::CancelAllJobs() {
        {
            std::lock_guard<std::mutex> lock(m_jobQueueMutex);
            m_jobQueue.clear();
        }

        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            for (auto& generations : m_chunkGenerations) {
                generations.clear();
            }
        }

        Log::Info("Cancelled all pending server worker jobs");
    }

    size_t ServerWorkerPool::GetPendingJobCount() const {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);
        return m_jobQueue.size();
    }

    size_t ServerWorkerPool::GetActiveJobCount() const {
        return m_activeJobs.load(std::memory_order_relaxed);
    }

    void ServerWorkerPool::SetWorkerCount(size_t count) {
        if (m_running.load()) {
            Log::Warning("Cannot change worker count while ServerWorkerPool is running");
            return;
        }
        m_workerCount = count;
    }

    void ServerWorkerPool::LogStats() const {
        Log::Info("ServerWorkerPool Statistics:");
        Log::Info("  Chunks Generated: %zu", m_stats.chunksGenerated.load());
        Log::Info("  Chunks Loaded: %zu", m_stats.chunksLoaded.load());
        Log::Info("  Chunks Saved: %zu", m_stats.chunksSaved.load());
        Log::Info("  Jobs Submitted: %zu", m_stats.jobsSubmitted.load());
        Log::Info("  Jobs Completed: %zu", m_stats.jobsCompleted.load());
        Log::Info("  Jobs Cancelled: %zu", m_stats.jobsCancelled.load());
        Log::Info("  Jobs Failed: %zu", m_stats.jobsFailed.load());
        Log::Info("  Pending Jobs: %zu", GetPendingJobCount());
        Log::Info("  Active Jobs: %zu", GetActiveJobCount());
    }

    // ========================================================================
    // INTERNAL METHODS
    // ========================================================================

    void ServerWorkerPool::WorkerLoop() {
        PROFILE_THREAD("ServerWorker");
        // Terrain generation: pure throughput, no frame deadline. These are the
        // threads that were starving the main thread — four of them saturating
        // a 4-performance-core machine while the frame waited its turn.
        Core::SetCurrentThreadPriority(Core::ThreadPriorityClass::Throughput);
        Log::Debug("Server worker thread started");

        while (m_running.load()) {
            ServerJob job(ServerJobType::CHUNK_GENERATION, Game::Math::ChunkPos{0, 0});

            // Wait for job or shutdown (DequeueJob blocks until a job is available or shutdown)
            if (DequeueJob(job)) {
                m_activeJobs.fetch_add(1, std::memory_order_relaxed);
                ProcessJob(job);
                m_activeJobs.fetch_sub(1, std::memory_order_relaxed);
            }
            // DequeueJob returns false only on shutdown, so just loop back
        }

        Log::Debug("Server worker thread stopped");
    }

    void ServerWorkerPool::ProcessJob(const ServerJob& job) {
        // Check if job should be cancelled
        if (ShouldCancelJob(job)) {
            m_stats.jobsCancelled.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        try {
            // Execute the job task
            job.task();
            m_stats.jobsCompleted.fetch_add(1, std::memory_order_relaxed);
        }
        catch (const std::exception& e) {
            Log::Error("Server worker job failed: %s", e.what());
            m_stats.jobsFailed.fetch_add(1, std::memory_order_relaxed);
        }
        catch (...) {
            Log::Error("Server worker job failed with unknown exception");
            m_stats.jobsFailed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool ServerWorkerPool::ShouldCancelJob(const ServerJob& job) const {
        // World I/O jobs are never cancelled
        if (job.type == ServerJobType::WORLD_IO) {
            return false;
        }

        return IsGenerationStale(job.dimension, job.chunkPos, job.generationId);
    }

    void ServerWorkerPool::ProcessChunkGeneration(Game::DimensionId dimension,
                                                  Game::Math::ChunkPos chunkPos,
                                                  uint64_t generation) {
        PROFILE_ZONE;
        Game::World* world = WorldForJob(dimension);
        if (!world) {
            Log::Error("Cannot generate chunk - dimension '%s' has no world",
                       std::string(Game::DimensionName(dimension)).c_str());
            return;
        }

        Log::Debug("Generating chunk (%d, %d)", chunkPos.x, chunkPos.z);

        try {
            auto* chunkProvider = world->GetChunkProvider();
            if (!chunkProvider) {
                SendChunkGenResult(dimension, chunkPos, nullptr, false,
                                   "No chunk provider available");
                return;
            }

            // GetChunk routes through cache -> disk -> MyTerrainGenerator (all thread-safe)
            auto chunk = chunkProvider->GetChunk(chunkPos);

            // Check if this job's generation is still current before sending result
            if (IsGenerationStale(dimension, chunkPos, generation)) {
                m_stats.jobsCancelled.fetch_add(1, std::memory_order_relaxed);
                Log::Debug("Chunk (%d, %d) generation stale, discarding result", chunkPos.x, chunkPos.z);
                return;
            }

            if (chunk) {
                SendChunkGenResult(dimension, chunkPos, chunk, true);
                m_stats.chunksGenerated.fetch_add(1, std::memory_order_relaxed);
                Log::Debug("Successfully generated chunk (%d, %d)", chunkPos.x, chunkPos.z);
            } else {
                SendChunkGenResult(dimension, chunkPos, nullptr, false, "Chunk generation failed");
            }
        }
        catch (const std::exception& e) {
            SendChunkGenResult(dimension, chunkPos, nullptr, false,
                               std::string("Exception: ") + e.what());
        }
    }

    void ServerWorkerPool::ProcessChunkLoading(Game::DimensionId dimension,
                                               Game::Math::ChunkPos chunkPos,
                                               uint64_t generation) {
        // Resolved here, not cached: this is the one call that BLOCKS inside
        // ChunkProvider::GetChunk until the server thread pumps THIS
        // dimension's generator, so pointing it at the wrong level would park
        // the worker on a pipeline nobody is feeding.
        Game::World* world = WorldForJob(dimension);
        if (!world) {
            Log::Error("Cannot load chunk - dimension '%s' has no world",
                       std::string(Game::DimensionName(dimension)).c_str());
            return;
        }

        Log::Debug("Loading chunk (%d, %d)", chunkPos.x, chunkPos.z);

        try {
            auto* chunkProvider = world->GetChunkProvider();
            if (!chunkProvider) {
                SendChunkGenResult(dimension, chunkPos, nullptr, false,
                                   "No chunk provider available");
                return;
            }

            // Cache -> disk only. A chunk that is not on disk is handed to
            // the terrain library as a ticketed generation request (MC's
            // ChunkMap model) and its result arrives through
            // IntegratedServer::PumpChunkPipeline — this worker does NOT park
            // inside the library waiting for it. Measured 2026-08-29: twelve
            // workers spent 97-99% of their time in that wait.
            auto chunk = chunkProvider->LoadWithoutGenerating(chunkPos);
            if (!chunk) {
                if (auto* gen = dynamic_cast<Game::MyTerrainGenerator*>(chunkProvider->GetGenerator())) {
                    if (IsGenerationStale(dimension, chunkPos, generation)) {
                        m_stats.jobsCancelled.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    gen->EnqueueGenerationRequest(chunkPos);
                    return;   // result comes from the generation path
                }
                // No library generator: fall back to the blocking path.
                chunk = chunkProvider->GetChunk(chunkPos);
            }

            // Check if this job's generation is still current before sending result
            if (IsGenerationStale(dimension, chunkPos, generation)) {
                m_stats.jobsCancelled.fetch_add(1, std::memory_order_relaxed);
                Log::Debug("Chunk (%d, %d) generation stale, discarding result", chunkPos.x, chunkPos.z);
                return;
            }

            if (chunk) {
                SendChunkGenResult(dimension, chunkPos, chunk, true);
                m_stats.chunksLoaded.fetch_add(1, std::memory_order_relaxed);
                Log::Debug("Successfully loaded chunk (%d, %d)", chunkPos.x, chunkPos.z);
            } else {
                SendChunkGenResult(dimension, chunkPos, nullptr, false,
                                   "Failed to load/generate chunk");
            }
        }
        catch (const std::exception& e) {
            SendChunkGenResult(dimension, chunkPos, nullptr, false,
                               std::string("Exception: ") + e.what());
        }
    }

    void ServerWorkerPool::ProcessChunkSaving(Game::DimensionId dimension,
                                              Game::Math::ChunkPos chunkPos,
                                              std::shared_ptr<Game::Chunk> chunk) {
        Game::World* world = WorldForJob(dimension);
        if (!world || !chunk) {
            Log::Error("Cannot save chunk - missing world or chunk");
            return;
        }

        Log::Debug("Saving chunk (%d, %d) to disk", chunkPos.x, chunkPos.z);

        try {
            // Get chunk provider from world
            auto* chunkProvider = world->GetChunkProvider();
            if (!chunkProvider) {
                Log::Error("Cannot save chunk - no chunk provider available");
                return;
            }

            // TODO: Use actual chunk saver here
            // For now, we'll save through the chunk provider
            chunkProvider->SaveChunk(chunkPos);
            
            m_stats.chunksSaved.fetch_add(1, std::memory_order_relaxed);
            Log::Debug("Successfully saved chunk (%d, %d)", chunkPos.x, chunkPos.z);
        }
        catch (const std::exception& e) {
            Log::Error("Failed to save chunk (%d, %d): %s", chunkPos.x, chunkPos.z, e.what());
        }
    }

    bool ServerWorkerPool::EnqueueJob(ServerJob&& job) {
        std::unique_lock<std::mutex> lock(m_jobQueueMutex);

        // No capacity limit, matching MC: ChunkTaskDispatcher holds one task per
        // scheduled chunk and never refuses one. The bound here is the same one
        // MC has — a chunk can only be requested once while in flight
        // (ServerLevel::pendingChunkLoads, one set per dimension), so depth is
        // bounded by the players' tracking views, ~1k per player.
        //
        // Dropping was worse than it looked: a dropped job produces no result,
        // and the requester treats "requested, no result yet" as permanently in
        // flight, so the chunk needed an out-of-band retry list to come back at
        // all. Accepting the job removes that entire failure mode.
        m_jobQueue.push_back(std::move(job));
        lock.unlock();
        m_jobCondition.notify_one();
        return true;
    }

    // Port of MC ChunkTaskDispatcher's priority ordering, which submits every
    // generation task with `() -> center.getQueueLevel()` — a key re-evaluated
    // at poll time rather than fixed at submit time, because the thing it
    // measures (distance from a player) moves.
    //
    // Ours reads the same way: the job whose chunk is nearest to any player is
    // taken next. Sorting at SUBMIT time instead would be stale the moment the
    // player walked, and would order a burst of a thousand requests by where
    // the player was when the burst began.
    //
    // Linear scan under the lock, exactly like ClientWorkerPool::PollNearestLocked
    // (the in-repo port of CompileTaskDynamicQueue.poll). Polls are bounded by
    // worker throughput, so this is a few hundred distance computations a second.
    bool ServerWorkerPool::DequeueJob(ServerJob& job) {
        std::unique_lock<std::mutex> lock(m_jobQueueMutex);

        m_jobCondition.wait(lock, [this] { return !m_jobQueue.empty() || !m_running.load(); });

        if (m_jobQueue.empty()) {
            return false;
        }

        std::vector<Game::Math::ChunkPos> anchors;
        {
            std::lock_guard<std::mutex> anchorLock(m_anchorMutex);
            anchors = m_anchors;
        }

        size_t best = 0;
        int64_t bestKey = std::numeric_limits<int64_t>::max();
        for (size_t i = 0; i < m_jobQueue.size(); ++i) {
            const ServerJob& candidate = m_jobQueue[i];

            // Saves and world I/O carry no meaningful position; run them ahead
            // of terrain so they cannot be starved behind a streaming burst.
            int64_t key = -1;
            if (candidate.type == ServerJobType::CHUNK_GENERATION ||
                candidate.type == ServerJobType::CHUNK_LOADING) {
                key = std::numeric_limits<int64_t>::max();
                for (const auto& anchor : anchors) {
                    const int64_t dx = candidate.chunkPos.x - anchor.x;
                    const int64_t dz = candidate.chunkPos.z - anchor.z;
                    key = std::min(key, dx * dx + dz * dz);
                }
            }
            if (key < bestKey) {
                bestKey = key;
                best = i;
            }
        }

        job = std::move(m_jobQueue[best]);
        // Swap-and-pop: selection is purely by key, so the container has no
        // meaningful order to preserve.
        if (best != m_jobQueue.size() - 1) {
            m_jobQueue[best] = std::move(m_jobQueue.back());
        }
        m_jobQueue.pop_back();
        return true;
    }

    void ServerWorkerPool::SetAnchors(std::vector<Game::Math::ChunkPos> anchors) {
        std::lock_guard<std::mutex> lock(m_anchorMutex);
        m_anchors = std::move(anchors);
    }

    void ServerWorkerPool::SendChunkGenResult(Game::DimensionId dimension,
                                              Game::Math::ChunkPos chunkPos,
                                              std::shared_ptr<Game::Chunk> chunk, bool success,
                                              const std::string& error) {
        // Stamping the dimension here is what lets the server thread file the
        // result against the right level — the queue is shared by all three.
        Network::ChunkGenResult result(chunkPos, chunk, success, dimension);
        if (!error.empty()) {
            result.errorMessage = error;
        }

        // Send to ChunkGenResultQueue for server thread consumption
        s_chunkGenResultQueue.try_push(std::move(result));
    }
    
    Network::ResultQueue<Network::ChunkGenResult>& ServerWorkerPool::GetChunkGenResultQueue() {
        return s_chunkGenResultQueue;
    }

    bool ServerWorkerPool::IsGenerationStale(Game::DimensionId dimension,
                                             Game::Math::ChunkPos chunkPos,
                                             uint64_t generation) const {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        const auto& generations = m_chunkGenerations[Game::DimensionSlot(dimension)];
        auto it = generations.find(chunkPos);
        if (it == generations.end()) return false;
        return generation != it->second;
    }

    // ========================================================================
    // GLOBAL FUNCTIONS
    // ========================================================================

    void InitializeServerWorkerPool(size_t workerCount) {
        if (g_serverWorkerPool) {
            Log::Warning("ServerWorkerPool already initialized");
            return;
        }

        g_serverWorkerPool = std::make_unique<ServerWorkerPool>(workerCount);
        g_serverWorkerPool->Initialize();
    }

    void ShutdownServerWorkerPool() {
        if (g_serverWorkerPool) {
            g_serverWorkerPool->Shutdown();
            g_serverWorkerPool.reset();
        }
    }

    void SubmitServerChunkGeneration(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                     int priority) {
        if (g_serverWorkerPool) {
            g_serverWorkerPool->SubmitChunkGeneration(dimension, chunkPos, priority);
        }
    }

    void SetServerChunkLoadAnchors(std::vector<Game::Math::ChunkPos> anchors) {
        if (g_serverWorkerPool) {
            g_serverWorkerPool->SetAnchors(std::move(anchors));
        }
    }

    bool SubmitServerChunkLoading(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                  int priority) {
        if (!g_serverWorkerPool) {
            return false;
        }
        return g_serverWorkerPool->SubmitChunkLoading(dimension, chunkPos, priority);
    }

    void SubmitServerChunkSaving(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                 std::shared_ptr<Game::Chunk> chunk, int priority) {
        if (g_serverWorkerPool) {
            g_serverWorkerPool->SubmitChunkSaving(dimension, chunkPos, chunk, priority);
        }
    }

} // namespace Threading