// File: src/server/world/ServerWorkerPool.hpp
#pragma once

#include "common/core/JobSystem.hpp"
#include "common/network/MessageQueue.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"
#include <array>
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

        // Which world `chunkPos` is in. The pool is process-wide and serves
        // every dimension, so the job — not the pool — is what names the
        // level; see ServerWorkerPool::ProcessChunkLoading.
        Game::DimensionId dimension = Game::DimensionId::Overworld;

        std::function<void()> task;
        int priority = 0; // Higher = more important
        uint64_t generationId{0}; // Generation ID for cancellation tracking
        std::chrono::steady_clock::time_point submitTime;

        ServerJob(ServerJobType jobType, Game::Math::ChunkPos pos,
                  Game::DimensionId dim = Game::DimensionId::Overworld)
            : type(jobType), chunkPos(pos), dimension(dim)
            , submitTime(std::chrono::steady_clock::now()) {}
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
        void SubmitChunkGeneration(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                   int priority = 0);

        // Submit chunk loading job (from disk). Always accepted — the queue has
        // no capacity limit, matching MC's ChunkTaskDispatcher. Returns false
        // only when the pool is not running.
        bool SubmitChunkLoading(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                int priority = 0);

        // Submit chunk saving job
        void SubmitChunkSaving(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                               std::shared_ptr<Game::Chunk> chunk, int priority = 0);

        // Submit generic world I/O job
        void SubmitWorldIOJob(std::function<void()> task, int priority = 0);

        // ========================================================================
        // JOB MANAGEMENT
        // ========================================================================

        // Cancel all jobs for a specific chunk
        void CancelChunkJobs(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos);

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
        // Post a load/generation result (any thread). Public since the
        // ticket-driven generation path (IntegratedServer) produces results
        // from conversion jobs rather than from ProcessChunkLoading.
        void SendChunkGenResult(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                std::shared_ptr<Game::Chunk> chunk, bool success,
                                const std::string& error = "");

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

        // Job cancellation via generation IDs, ONE MAP PER DIMENSION (indexed
        // by Game::DimensionSlot). A single ChunkPos-keyed map would make
        // Nether (0,0) and Overworld (0,0) share a counter: submitting one
        // would bump the other's generation, the in-flight job would see
        // itself as stale, and a stale job returns WITHOUT sending a result —
        // so the requester's pending entry would never clear and that chunk
        // would never load again for the rest of the session.
        mutable std::mutex m_cancelMutex;
        std::array<std::unordered_map<Game::Math::ChunkPos, uint64_t, Game::Math::ChunkPosHash>,
                   Game::kDimensionCount> m_chunkGenerations;

        // NO cached world pointer. One pool serves every dimension, so the
        // world is resolved from the job's DimensionId at execution time; a
        // cached one would pin every worker to whichever level happened to
        // exist when Initialize ran.

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
        void ProcessChunkGeneration(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                    uint64_t generation);
        void ProcessChunkLoading(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                 uint64_t generation);
        void ProcessChunkSaving(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                std::shared_ptr<Game::Chunk> chunk);

        // Job queue management. EnqueueJob returns false when the queue is at
        // capacity and the job was dropped.
        bool EnqueueJob(ServerJob&& job);
        bool DequeueJob(ServerJob& job);

        // Result handling


        // Check if a job's generation is stale
        bool IsGenerationStale(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                               uint64_t generation) const;
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
    void SubmitServerChunkGeneration(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                     int priority = 0);
    bool SubmitServerChunkLoading(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                  int priority = 0);

    // Player chunk positions the pool orders queued chunk work against.
    //
    // Deliberately NOT per-dimension: the anchors only order the queue, and a
    // chunk in another dimension at the same x/z is exactly as urgent as this
    // one — both are somewhere a player is standing.
    void SetServerChunkLoadAnchors(std::vector<Game::Math::ChunkPos> anchors);
    void SubmitServerChunkSaving(Game::DimensionId dimension, Game::Math::ChunkPos chunkPos,
                                 std::shared_ptr<Game::Chunk> chunk, int priority = 0);

} // namespace Threading