// File: src/server/world/ServerWorkerPool.cpp
#include "ServerWorkerPool.hpp"
#include "common/core/Log.hpp"
#include "common/world/level/World.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "server/IntegratedServer.hpp"
#include "platform/GameDirectory.hpp"
#include <algorithm>

namespace Threading {

    // Global instance
    std::unique_ptr<ServerWorkerPool> g_serverWorkerPool = nullptr;
    
    // Static result queue for chunk generation results
    static Network::ResultQueue<Network::ChunkGenResult> s_chunkGenResultQueue;

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

        // Get world from IntegratedServer
        if (!Server::g_integratedServer) {
            Log::Error("Cannot initialize ServerWorkerPool: IntegratedServer not initialized");
            return;
        }

        m_world = Server::g_integratedServer->GetWorld();
        if (!m_world) {
            Log::Error("Cannot initialize ServerWorkerPool: IntegratedServer has no world");
            return;
        }
        
        // Calculate queue size based on render distance
        int renderDistance = Platform::g_gameSettings.GetRenderDistance();
        size_t requiredQueueSize = (2 * renderDistance + 1) * (2 * renderDistance + 1);
        m_maxQueueSize = static_cast<size_t>(requiredQueueSize * 1.2); // 20% margin
        Log::Info("ServerWorkerPool queue size set to %zu (for render distance %d)", m_maxQueueSize, renderDistance);
        
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

        // Wait for all threads to finish
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

    void ServerWorkerPool::SubmitChunkGeneration(Game::Math::ChunkPos chunkPos, int priority) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit chunk generation job - ServerWorkerPool not running");
            return;
        }

        ServerJob job(ServerJobType::CHUNK_GENERATION, chunkPos);
        job.priority = priority;
        job.task = [this, chunkPos]() { ProcessChunkGeneration(chunkPos); };

        EnqueueJob(std::move(job));
        m_stats.jobsSubmitted.fetch_add(1, std::memory_order_relaxed);
    }

    void ServerWorkerPool::SubmitChunkLoading(Game::Math::ChunkPos chunkPos, int priority) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit chunk loading job - ServerWorkerPool not running");
            return;
        }

        ServerJob job(ServerJobType::CHUNK_LOADING, chunkPos);
        job.priority = priority;
        job.task = [this, chunkPos]() { ProcessChunkLoading(chunkPos); };

        EnqueueJob(std::move(job));
        m_stats.jobsSubmitted.fetch_add(1, std::memory_order_relaxed);
    }

    void ServerWorkerPool::SubmitChunkSaving(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk, int priority) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit chunk saving job - ServerWorkerPool not running");
            return;
        }

        if (!chunk) {
            Log::Warning("Cannot submit chunk saving job with null chunk");
            return;
        }

        ServerJob job(ServerJobType::CHUNK_SAVING, chunkPos);
        job.priority = priority;
        job.task = [this, chunkPos, chunk]() { ProcessChunkSaving(chunkPos, chunk); };

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

    void ServerWorkerPool::CancelChunkJobs(Game::Math::ChunkPos chunkPos) {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_cancelledChunks.insert(chunkPos);
        Log::Debug("Cancelled all jobs for chunk (%d, %d)", chunkPos.x, chunkPos.z);
    }

    void ServerWorkerPool::CancelAllJobs() {
        {
            std::lock_guard<std::mutex> lock(m_jobQueueMutex);
            std::queue<ServerJob> empty;
            std::swap(m_jobQueue, empty);
        }

        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            m_cancelledChunks.clear();
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
        Log::Debug("Server worker thread started");

        while (m_running.load()) {
            ServerJob job(ServerJobType::CHUNK_GENERATION, Game::Math::ChunkPos{0, 0});
            
            // Wait for job or shutdown
            if (DequeueJob(job)) {
                m_activeJobs.fetch_add(1, std::memory_order_relaxed);
                ProcessJob(job);
                m_activeJobs.fetch_sub(1, std::memory_order_relaxed);
            } else {
                // No job available, sleep briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
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

        return IsChunkCancelled(job.chunkPos);
    }

    void ServerWorkerPool::ProcessChunkGeneration(Game::Math::ChunkPos chunkPos) {
        if (!m_world) {
            Log::Error("Cannot generate chunk - no world reference");
            return;
        }

        Log::Debug("Generating chunk (%d, %d)", chunkPos.x, chunkPos.z);

        try {
            auto* chunkProvider = m_world->GetChunkProvider();
            if (!chunkProvider) {
                SendChunkGenResult(chunkPos, nullptr, false, "No chunk provider available");
                return;
            }

            // GetChunk routes through cache -> disk -> MyTerrainGenerator (all thread-safe)
            auto chunk = chunkProvider->GetChunk(chunkPos);

            if (chunk) {
                SendChunkGenResult(chunkPos, chunk, true);
                m_stats.chunksGenerated.fetch_add(1, std::memory_order_relaxed);
                Log::Debug("Successfully generated chunk (%d, %d)", chunkPos.x, chunkPos.z);
            } else {
                SendChunkGenResult(chunkPos, nullptr, false, "Chunk generation failed");
            }
        }
        catch (const std::exception& e) {
            SendChunkGenResult(chunkPos, nullptr, false, std::string("Exception: ") + e.what());
        }
    }

    void ServerWorkerPool::ProcessChunkLoading(Game::Math::ChunkPos chunkPos) {
        if (!m_world) {
            Log::Error("Cannot load chunk - no world reference");
            return;
        }

        Log::Debug("Loading chunk (%d, %d)", chunkPos.x, chunkPos.z);

        try {
            auto* chunkProvider = m_world->GetChunkProvider();
            if (!chunkProvider) {
                SendChunkGenResult(chunkPos, nullptr, false, "No chunk provider available");
                return;
            }

            // GetChunk routes through cache -> disk -> MyTerrainGenerator (all thread-safe)
            auto chunk = chunkProvider->GetChunk(chunkPos);

            if (chunk) {
                SendChunkGenResult(chunkPos, chunk, true);
                m_stats.chunksLoaded.fetch_add(1, std::memory_order_relaxed);
                Log::Debug("Successfully loaded chunk (%d, %d)", chunkPos.x, chunkPos.z);
            } else {
                SendChunkGenResult(chunkPos, nullptr, false, "Failed to load/generate chunk");
            }
        }
        catch (const std::exception& e) {
            SendChunkGenResult(chunkPos, nullptr, false, std::string("Exception: ") + e.what());
        }
    }

    void ServerWorkerPool::ProcessChunkSaving(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk) {
        if (!m_world || !chunk) {
            Log::Error("Cannot save chunk - missing world or chunk");
            return;
        }

        Log::Debug("Saving chunk (%d, %d) to disk", chunkPos.x, chunkPos.z);

        try {
            // Get chunk provider from world
            auto* chunkProvider = m_world->GetChunkProvider();
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

    void ServerWorkerPool::EnqueueJob(ServerJob&& job) {
        std::unique_lock<std::mutex> lock(m_jobQueueMutex);
        
        // Check queue size limit
        if (m_jobQueue.size() >= m_maxQueueSize) {
            Log::Warning("Server worker queue full, dropping job");
            return;
        }

        m_jobQueue.push(std::move(job));
        lock.unlock();
        m_jobCondition.notify_one();
    }

    bool ServerWorkerPool::DequeueJob(ServerJob& job) {
        std::unique_lock<std::mutex> lock(m_jobQueueMutex);
        
        if (m_jobCondition.wait_for(lock, std::chrono::milliseconds(100), 
                                   [this] { return !m_jobQueue.empty() || !m_running.load(); })) {
            if (!m_jobQueue.empty()) {
                job = std::move(m_jobQueue.front());
                m_jobQueue.pop();
                return true;
            }
        }
        
        return false;
    }

    void ServerWorkerPool::SendChunkGenResult(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk, bool success, const std::string& error) {
        Network::ChunkGenResult result(chunkPos, chunk, success);
        if (!error.empty()) {
            result.errorMessage = error;
        }

        // Send to ChunkGenResultQueue for server thread consumption
        s_chunkGenResultQueue.try_push(std::move(result));
    }
    
    Network::ResultQueue<Network::ChunkGenResult>& ServerWorkerPool::GetChunkGenResultQueue() {
        return s_chunkGenResultQueue;
    }

    bool ServerWorkerPool::IsChunkCancelled(Game::Math::ChunkPos chunkPos) const {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        return m_cancelledChunks.find(chunkPos) != m_cancelledChunks.end();
    }

    void ServerWorkerPool::CleanupCancelledChunks() {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_cancelledChunks.clear();
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

    void SubmitServerChunkGeneration(Game::Math::ChunkPos chunkPos, int priority) {
        if (g_serverWorkerPool) {
            g_serverWorkerPool->SubmitChunkGeneration(chunkPos, priority);
        }
    }

    void SubmitServerChunkLoading(Game::Math::ChunkPos chunkPos, int priority) {
        if (g_serverWorkerPool) {
            g_serverWorkerPool->SubmitChunkLoading(chunkPos, priority);
        }
    }

    void SubmitServerChunkSaving(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk, int priority) {
        if (g_serverWorkerPool) {
            g_serverWorkerPool->SubmitChunkSaving(chunkPos, chunk, priority);
        }
    }

} // namespace Threading