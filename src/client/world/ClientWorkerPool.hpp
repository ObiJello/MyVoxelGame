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
#include <optional>
#include <unordered_set>
#include <vector>
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
        
        Game::Math::ChunkPos chunkPos;
        int sectionY;
        // Distance at SUBMIT time. Retained for stats/debug only — job selection
        // re-derives distance against the live camera at poll time, the way MC's
        // CompileTaskDynamicQueue.poll(cameraPos) does.
        float priority = 0.0f;
        std::chrono::steady_clock::time_point submitTime;

        // MC's CompileTask.isRecompile — true when this section already has a
        // compiled mesh on screen, false for its first ever compile. Drives the
        // recompile quota in ClientWorkerPool::PollNearestLocked so re-meshes of
        // terrain you can already see cannot starve terrain you cannot.
        bool isRecompile = false;

        // Constructor
        MeshJob(std::shared_ptr<Client::Render::MeshJobData> snap)
            : snapshot(std::move(snap))
            , chunkPos(snapshot->chunkPos)
            , sectionY(snapshot->sectionY)
            , priority(snapshot->distanceToPlayer)
            , submitTime(snapshot->submitTime)
            , isRecompile(snapshot->jobType != Client::Render::MeshJobType::Initial) {}
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

        // Compile a section on the CALLING thread and push the result straight
        // to the render thread's upload queue, skipping the compile queue and
        // the worker pool entirely. This is MC's
        // SectionRenderDispatcher.rebuildSectionSync / RenderSection.compileSync,
        // used by the "Semi Blocking" and "Fully Blocking" chunk-builder modes
        // so a block the player just placed is visible the same frame.
        //
        // MAIN THREAD ONLY, and it costs main-thread milliseconds by design —
        // that is the trade those two modes exist to make. The result carries
        // holdsUploadPermit = false because no permit was acquired for it.
        void BuildMeshJobSync(std::shared_ptr<Client::Render::MeshJobData> snapshot);
        
        // Legacy methods - DEPRECATED (not thread-safe)

        // ========================================================================
        // JOB CANCELLATION
        // ========================================================================

        // Cancel all mesh jobs for a specific chunk
        void CancelMeshJob(Game::Math::ChunkPos chunkPos);

        // Cancel mesh jobs for a specific chunk section
        void CancelMeshJob(Game::Math::ChunkPos chunkPos, int sectionY);

        // Remove chunk from cancelled set (call when chunk is reloaded)
        void UncancelChunk(Game::Math::ChunkPos chunkPos);

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

        // Pending compile queue — MC's SectionRenderDispatcher.compileQueue
        // (a CompileTaskDynamicQueue). Deliberately NOT a priority_queue: the
        // ordering key is distance to the camera, and the camera moves, so a
        // heap ordered at insertion time goes stale the moment the player walks.
        // MC linear-scans for the nearest task on every poll instead, against
        // the CURRENT camera position, and so do we — see PollNearestLocked.
        //
        // This queue is the backlog. Nothing upstream is allowed to cap it to
        // the size of the upload permit pool; the pool gates EXECUTION in
        // WorkerLoop, not admission. That separation is the whole point — see
        // the header comment on ClientChunkManager::ScheduleMeshBuildsWithSnapshots.
        mutable std::mutex m_jobQueueMutex;
        std::vector<MeshJob> m_jobQueue;
        std::condition_variable m_jobCondition;
        bool m_prioritizationEnabled = true;

        // MC's CompileTaskDynamicQueue.MAX_RECOMPILE_QUOTA. After this many
        // consecutive recompiles the queue forces an initial compile through,
        // even if a recompile is nearer. Without it, the constant re-meshing of
        // already-visible sections (our measured ~2.2x remesh factor) can starve
        // first-time compiles indefinitely and the world stops filling in.
        static constexpr int kMaxRecompileQuota = 2;
        int m_recompileQuota = kMaxRecompileQuota;

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

        // Port of MC's CompileTaskDynamicQueue.poll(Vec3): drop cancelled
        // entries, then take the nearest job to `cameraPos`, subject to the
        // recompile quota. Caller MUST hold m_jobQueueMutex.
        std::optional<MeshJob> PollNearestLocked(const glm::vec3& cameraPos);

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

    // Player position updates
    void SetClientWorkerPlayerPosition(const glm::vec3& position);

    // Job cancellation
    void CancelClientMeshJob(Game::Math::ChunkPos chunkPos);
    void CancelClientMeshJob(Game::Math::ChunkPos chunkPos, int sectionY);
    void UncancelClientMeshChunk(Game::Math::ChunkPos chunkPos);

    // World reference management  
    void SetClientWorkerWorld(Game::World* world);

} // namespace Threading