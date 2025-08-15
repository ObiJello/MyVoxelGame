// File: src/client/world/ClientWorkerPool.hpp
#pragma once

#include "common/core/JobSystem.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/math/WorldMath.hpp"
#include "../renderer/mesh/SectionMesh.hpp"
#include <functional>
#include <memory>
#include <atomic>
#include <unordered_set>
#include <queue>

// Forward declarations
namespace Game {
    class Chunk;
    class World;
}

// Include MeshJobData definition - needed for MeshJob constructor
#include "../renderer/mesh/MeshJobData.hpp"

namespace Threading {
    
    // Client-side mesh building job (uses snapshot for thread safety)
    struct MeshJob {
        // Section snapshot data
        std::shared_ptr<Client::Render::MeshJobData> snapshot;
        
        // Fields for priority queue
        Game::Math::ChunkPos chunkPos;
        int sectionY;
        float priority = 0.0f;
        std::chrono::steady_clock::time_point submitTime;

        // Constructor
        MeshJob(std::shared_ptr<Client::Render::MeshJobData> snap)
            : snapshot(std::move(snap))
            , chunkPos(snapshot->chunkPos)
            , sectionY(snapshot->sectionY)
            , priority(snapshot->distanceToPlayer)
            , submitTime(snapshot->submitTime) {}

        // For priority queue (higher priority = more important)
        bool operator<(const MeshJob& other) const {
            return priority > other.priority;
        }
    };

    // Client worker pool dedicated ONLY to mesh building
    // Results are sent to MeshResultQueue for client render thread consumption
    class ClientWorkerPool {
    public:
        explicit ClientWorkerPool(size_t workerCount = 2);
        ~ClientWorkerPool();

        // Non-copyable, non-movable
        ClientWorkerPool(const ClientWorkerPool&) = delete;
        ClientWorkerPool& operator=(ClientWorkerPool&) = delete;

        // ========================================================================
        // LIFECYCLE
        // ========================================================================

        void Initialize();
        void Shutdown();
        bool IsRunning() const { return m_running.load(); }

        // Set world reference for cross-chunk neighbor access during meshing
        void SetWorld(Game::World* world) { m_world = world; }

        // ========================================================================
        // MESH JOB SUBMISSION
        // ========================================================================

        // Submit mesh building job using snapshot (THREAD-SAFE)
        // Returns true if job was successfully queued, false if queue is full
        bool SubmitMeshJobWithSnapshot(std::shared_ptr<Client::Render::MeshJobData> snapshot);
        
        // Legacy methods - DEPRECATED (not thread-safe)
        void SubmitMeshJob(Game::Math::ChunkPos chunkPos, int sectionY, 
                          std::shared_ptr<Game::Chunk> chunkData, float priority = 0.0f);

        void SubmitChunkMeshJobs(Game::Math::ChunkPos chunkPos, 
                               std::shared_ptr<Game::Chunk> chunkData, 
                               const glm::vec3& playerPosition);

        // ========================================================================
        // JOB CANCELLATION
        // ========================================================================

        // Cancel all mesh jobs for a specific chunk
        void CancelMeshJob(Game::Math::ChunkPos chunkPos);

        // Cancel mesh jobs for a specific chunk section
        void CancelMeshJob(Game::Math::ChunkPos chunkPos, int sectionY);

        // Cancel all pending jobs
        void CancelAllJobs();

        // ========================================================================
        // QUEUE MANAGEMENT
        // ========================================================================

        size_t GetPendingJobCount() const;
        size_t GetActiveJobCount() const;

        // Clear completed results to prevent memory buildup
        void DrainCompletedResults();

        // ========================================================================
        // CONFIGURATION
        // ========================================================================

        void SetMaxQueueSize(size_t maxSize) { m_maxQueueSize = maxSize; }
        size_t GetMaxQueueSize() const { return m_maxQueueSize; }

        void SetWorkerCount(size_t count);
        size_t GetWorkerCount() const { return m_workerThreads.size(); }

        // Enable/disable mesh job prioritization
        void SetPrioritizationEnabled(bool enabled) { m_prioritizationEnabled = enabled; }
        bool IsPrioritizationEnabled() const { return m_prioritizationEnabled; }

        // ========================================================================
        // PLAYER POSITION UPDATES
        // ========================================================================

        // Update player position for priority calculations
        void SetPlayerPosition(const glm::vec3& position);
        glm::vec3 GetPlayerPosition() const;

        // ========================================================================
        // STATISTICS
        // ========================================================================

        struct ClientWorkerStats {
            std::atomic<size_t> meshJobsSubmitted{0};
            std::atomic<size_t> meshJobsCompleted{0};
            std::atomic<size_t> meshJobsCancelled{0};
            std::atomic<size_t> meshJobsFailed{0};
            std::atomic<size_t> sectionsBuilt{0};
            std::atomic<size_t> verticesGenerated{0};
            std::atomic<size_t> indicesGenerated{0};

            void Reset() {
                meshJobsSubmitted = meshJobsCompleted = meshJobsCancelled = meshJobsFailed = 0;
                sectionsBuilt = verticesGenerated = indicesGenerated = 0;
            }
        };

        const ClientWorkerStats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats.Reset(); }

        void LogStats() const;

    private:
        // Worker thread management
        std::vector<std::thread> m_workerThreads;
        std::atomic<bool> m_running{false};
        size_t m_workerCount;

        // Job queue (priority queue for distance-based prioritization)
        mutable std::mutex m_jobQueueMutex;
        std::priority_queue<MeshJob> m_jobQueue;
        std::condition_variable m_jobCondition;
        size_t m_maxQueueSize = 2048;  // Increased for better throughput
        bool m_prioritizationEnabled = true;

        // Job cancellation tracking
        struct SectionKey {
            Game::Math::ChunkPos chunkPos;
            int sectionY;

            bool operator==(const SectionKey& other) const {
                return chunkPos.x == other.chunkPos.x && 
                       chunkPos.z == other.chunkPos.z && 
                       sectionY == other.sectionY;
            }
        };

        struct SectionKeyHash {
            std::size_t operator()(const SectionKey& key) const {
                return std::hash<int32_t>{}(key.chunkPos.x) ^
                       (std::hash<int32_t>{}(key.chunkPos.z) << 1) ^
                       (std::hash<int>{}(key.sectionY) << 2);
            }
        };

        mutable std::mutex m_cancelMutex;
        std::unordered_set<SectionKey, SectionKeyHash> m_cancelledSections;
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_cancelledChunks;

        // Player position for priority calculations
        mutable std::mutex m_playerMutex;
        glm::vec3 m_playerPosition{0.0f};

        // Statistics
        ClientWorkerStats m_stats;

        // Active job tracking
        std::atomic<size_t> m_activeJobs{0};

        // World reference for cross-chunk neighbor access during meshing
        Game::World* m_world = nullptr;

        // ========================================================================
        // INTERNAL METHODS
        // ========================================================================

        // Worker thread main loop
        void WorkerLoop();

        // Job processing
        void ProcessMeshJob(const MeshJob& job);
        bool ShouldCancelJob(const MeshJob& job) const;

        // Mesh building
        Network::MeshBuildResult BuildSectionMesh(const MeshJob& job);

        // Job queue management
        bool EnqueueJob(MeshJob&& job);

        // Priority calculation
        float CalculatePriority(Game::Math::ChunkPos chunkPos, int sectionY, const glm::vec3& playerPos) const;

        // Cancellation management
        bool IsSectionCancelled(Game::Math::ChunkPos chunkPos, int sectionY) const;
        bool IsChunkCancelled(Game::Math::ChunkPos chunkPos) const;
        void CleanupCancelledSections();

        // Result handling
        void SendMeshResult(Network::MeshBuildResult&& result);
        
        // Convert SectionMesh to MeshBuildResult format
        Network::MeshBuildResult ConvertSectionMeshToResult(const Render::SectionMesh& sectionMesh,
                                                           Game::Math::ChunkPos chunkPos, int sectionY);
    };

    // ========================================================================
    // GLOBAL ACCESS
    // ========================================================================

    // Global client worker pool instance
    extern std::unique_ptr<ClientWorkerPool> g_clientWorkerPool;

    // Convenience functions
    void InitializeClientWorkerPool(size_t workerCount = 2);
    void ShutdownClientWorkerPool();

    // Direct job submission
    void SubmitClientMeshJob(Game::Math::ChunkPos chunkPos, int sectionY, 
                           std::shared_ptr<Game::Chunk> chunkData, float priority = 0.0f);
    void SubmitClientChunkMeshJobs(Game::Math::ChunkPos chunkPos, 
                                 std::shared_ptr<Game::Chunk> chunkData, 
                                 const glm::vec3& playerPosition);

    // Player position updates
    void SetClientWorkerPlayerPosition(const glm::vec3& position);

    // Job cancellation
    void CancelClientMeshJob(Game::Math::ChunkPos chunkPos);
    void CancelClientMeshJob(Game::Math::ChunkPos chunkPos, int sectionY);

    // World reference management  
    void SetClientWorkerWorld(Game::World* world);

} // namespace Threading