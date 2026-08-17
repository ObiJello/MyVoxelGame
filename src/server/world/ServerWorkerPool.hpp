// File: src/server/world/ServerWorkerPool.hpp
#pragma once

#include "common/core/JobSystem.hpp"
#include "common/network/MessageQueue.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/math/WorldMath.hpp"
#include <functional>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace Game {
    class Chunk;
    class World;
}

namespace Threading {

    // Forward declarations
    class IChunkGenerator;
    class IChunkLoader;

    // Job types for server worker threads
    enum class ServerJobType {
        CHUNK_GENERATION,
        CHUNK_LOADING,
        CHUNK_SAVING,
        WORLD_IO
    };

    // Server-side work job
    struct ServerJob {
        ServerJobType type;
        Game::Math::ChunkPos chunkPos;
        std::function<void()> task;
        int priority = 0; // Higher = more important
        uint64_t generationId{0}; // Generation ID for cancellation tracking
        std::chrono::steady_clock::time_point submitTime;

        ServerJob(ServerJobType jobType, Game::Math::ChunkPos pos)
            : type(jobType), chunkPos(pos), submitTime(std::chrono::steady_clock::now()) {}
    };

    // Server worker pool dedicated to chunk I/O and generation
    // Results are sent to ChunkGenResultQueue for server thread consumption
    class ServerWorkerPool {
    public:
        explicit ServerWorkerPool(size_t workerCount = 4);
        ~ServerWorkerPool();

        // Non-copyable, non-movable
        ServerWorkerPool(const ServerWorkerPool&) = delete;
        ServerWorkerPool& operator=(const ServerWorkerPool&) = delete;

        // ========================================================================
        // LIFECYCLE
        // ========================================================================

        void Initialize();
        void Shutdown();
        bool IsRunning() const { return m_running.load(); }

        // ========================================================================
        // JOB SUBMISSION
        // ========================================================================

        // Submit chunk generation job
        void SubmitChunkGeneration(Game::Math::ChunkPos chunkPos, int priority = 0);

        // Submit chunk loading job (from disk). Always accepted — the queue has
        // no capacity limit, matching MC's ChunkTaskDispatcher. Returns false
        // only when the pool is not running.
        bool SubmitChunkLoading(Game::Math::ChunkPos chunkPos, int priority = 0);

        // Submit chunk saving job
        void SubmitChunkSaving(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk, int priority = 0);

        // Submit generic world I/O job
        void SubmitWorldIOJob(std::function<void()> task, int priority = 0);

        // ========================================================================
        // JOB MANAGEMENT
        // ========================================================================

        // Cancel all jobs for a specific chunk
        void CancelChunkJobs(Game::Math::ChunkPos chunkPos);

        // Cancel all pending jobs
        void CancelAllJobs();

        // Get job queue statistics
        size_t GetPendingJobCount() const;
        size_t GetActiveJobCount() const;

        // ========================================================================
        // CONFIGURATION
        // ========================================================================

        // Refresh the player positions DequeueJob prioritises against.
        void SetAnchors(std::vector<Game::Math::ChunkPos> anchors);

        void SetWorkerCount(size_t count);
        size_t GetWorkerCount() const { return m_workerThreads.size(); }

        // ========================================================================
        // QUEUE ACCESS (for server thread to consume results)
        // ========================================================================
        
        // Get the chunk generation result queue (used by server thread to consume results)
        static Network::ResultQueue<Network::ChunkGenResult>& GetChunkGenResultQueue();

        // ========================================================================
        // STATISTICS
        // ========================================================================

        struct ServerWorkerStats {
            std::atomic<size_t> chunksGenerated{0};
            std::atomic<size_t> chunksLoaded{0};
            std::atomic<size_t> chunksSaved{0};
            std::atomic<size_t> jobsSubmitted{0};
            std::atomic<size_t> jobsCompleted{0};
            std::atomic<size_t> jobsCancelled{0};
            std::atomic<size_t> jobsFailed{0};

            void Reset() {
                chunksGenerated = chunksLoaded = chunksSaved = 0;
                jobsSubmitted = jobsCompleted = jobsCancelled = jobsFailed = 0;
            }
        };

        const ServerWorkerStats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats.Reset(); }

        void LogStats() const;

    private:
        // Worker thread management
        std::vector<std::thread> m_workerThreads;
        std::atomic<bool> m_running{false};
        size_t m_workerCount;

        // Player chunk positions, refreshed each server tick. DequeueJob picks
        // the queued chunk nearest to any of them — MC re-evaluates its queue
        // level at poll time for the same reason: the players move, so a key
        // fixed at submit time is stale by the time the job is taken.
        mutable std::mutex m_anchorMutex;
        std::vector<Game::Math::ChunkPos> m_anchors;

        // Job queue
        mutable std::mutex m_jobQueueMutex;
        // Vector, not a queue: DequeueJob selects by distance to the nearest
        // player rather than by insertion order (MC ChunkTaskDispatcher's
        // queue-level priority), so there is no FIFO order to preserve.
        std::vector<ServerJob> m_jobQueue;
        std::condition_variable m_jobCondition;

        // Job cancellation via generation IDs
        mutable std::mutex m_cancelMutex;
        std::unordered_map<Game::Math::ChunkPos, uint64_t, Game::Math::ChunkPosHash> m_chunkGenerations;

        // World reference
        Game::World* m_world = nullptr;

        // Statistics
        ServerWorkerStats m_stats;

        // Active job tracking
        std::atomic<size_t> m_activeJobs{0};

        // ========================================================================
        // INTERNAL METHODS
        // ========================================================================

        // Worker thread main loop
        void WorkerLoop();

        // Job processing
        void ProcessJob(const ServerJob& job);
        bool ShouldCancelJob(const ServerJob& job) const;

        // Specific job handlers
        void ProcessChunkGeneration(Game::Math::ChunkPos chunkPos, uint64_t generation);
        void ProcessChunkLoading(Game::Math::ChunkPos chunkPos, uint64_t generation);
        void ProcessChunkSaving(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk);

        // Job queue management. EnqueueJob returns false when the queue is at
        // capacity and the job was dropped.
        bool EnqueueJob(ServerJob&& job);
        bool DequeueJob(ServerJob& job);

        // Result handling
        void SendChunkGenResult(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk, bool success, const std::string& error = "");

        // Check if a job's generation is stale
        bool IsGenerationStale(Game::Math::ChunkPos chunkPos, uint64_t generation) const;
    };

    // ========================================================================
    // GLOBAL ACCESS
    // ========================================================================

    // Global server worker pool instance
    extern std::unique_ptr<ServerWorkerPool> g_serverWorkerPool;

    // Convenience functions
    void InitializeServerWorkerPool(size_t workerCount = 4);
    void ShutdownServerWorkerPool();
    
    // Direct job submission
    void SubmitServerChunkGeneration(Game::Math::ChunkPos chunkPos, int priority = 0);
    bool SubmitServerChunkLoading(Game::Math::ChunkPos chunkPos, int priority = 0);

    // Player chunk positions the pool orders queued chunk work against.
    void SetServerChunkLoadAnchors(std::vector<Game::Math::ChunkPos> anchors);
    void SubmitServerChunkSaving(Game::Math::ChunkPos chunkPos, std::shared_ptr<Game::Chunk> chunk, int priority = 0);

} // namespace Threading