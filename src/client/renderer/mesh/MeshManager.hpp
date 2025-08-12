/* File: src/render/mesh/MeshManager.hpp
#pragma once

#include "SectionMesh.hpp"
#include "ChunkMeshData.hpp"
#include "Mesher.hpp"
#include "../../core/JobSystem.hpp"
#include "../../game/WorldMath.hpp"
#include <unordered_set>
#include <queue>
#include <mutex>
#include <future>
#include <chrono>

namespace Render {

    // Forward declarations
    class World;

    // Mesh compilation job data
    struct MeshJob {
        Game::Math::ChunkPos chunkPos;
        int sectionY;
        bool isHighPriority = false;
        std::chrono::steady_clock::time_point submitTime;

        MeshJob(Game::Math::ChunkPos pos, int secY, bool highPri = false)
            : chunkPos(pos), sectionY(secY), isHighPriority(highPri)
            , submitTime(std::chrono::steady_clock::now()) {}
    };

    // Completed mesh result
    struct MeshResult {
        Game::Math::ChunkPos chunkPos;
        int sectionY;
        SectionMesh mesh;
        bool success = false;
        std::chrono::steady_clock::time_point completeTime;

        MeshResult(Game::Math::ChunkPos pos, int secY)
            : chunkPos(pos), sectionY(secY)
            , completeTime(std::chrono::steady_clock::now()) {}
    };

    // Mesh manager configuration
    struct MeshManagerConfig {
        int maxMeshesPerFrame = 2;          // Limit GPU uploads per frame
        float maxBuildTimeMs = 5.0f;        // Max time per frame for mesh building
        int maxPendingJobs = 1000;          // Queue size limit
        bool enableAsyncBuilding = true;    // Use background threads
        bool enableFrustumCulling = true;   // Only mesh visible sections

        // Priority settings
        float highPriorityRadius = 3.0f;    // High priority radius
        int maxHighPriorityJobs = 50;       // Limit high priority queue
    };

    // Statistics for debugging/optimization
    struct MeshStats {
        // Per-frame counters
        int meshesBuiltThisFrame = 0;
        int meshesUploadedThisFrame = 0;
        int jobsSubmittedThisFrame = 0;

        // Timing
        float buildTimeMs = 0.0f;
        float uploadTimeMs = 0.0f;

        // Queue stats
        size_t pendingJobs = 0;
        size_t completedResults = 0;

        // Memory usage
        size_t totalGPUMemory = 0;
        size_t activeSections = 0;

        void Reset() {
            meshesBuiltThisFrame = 0;
            meshesUploadedThisFrame = 0;
            jobsSubmittedThisFrame = 0;
            buildTimeMs = 0.0f;
            uploadTimeMs = 0.0f;
        }
    };

    // Main mesh system coordinator
    class MeshManager {
    public:
        explicit MeshManager(const MeshManagerConfig& config = MeshManagerConfig{});
        ~MeshManager();

        // Lifecycle
        void Initialize(Game::World* world);
        void Shutdown();

        // Called by world thread when blocks change
        void MarkSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY);
        void MarkChunkDirty(Game::Math::ChunkPos chunkPos);

        // Called each frame on render thread
        void ProcessPendingRemeshes();
        void Update(float deltaTime);

        // Player position updates for prioritization
        void SetPlayerPosition(const glm::vec3& position);

        // Configuration
        void SetConfig(const MeshManagerConfig& config) { m_config = config; }
        const MeshManagerConfig& GetConfig() const { return m_config; }

        // Access GPU data for rendering
        const GPUDataManager& GetGPUDataManager() const { return m_gpuDataManager; }
        const GPUSectionData* GetSectionGPUData(Game::Math::ChunkPos chunkPos, int sectionY) const;

        // Statistics
        const MeshStats& GetStats() const { return m_stats; }
        void ResetFrameStats() { m_stats.Reset(); }

        // Debug and utilities
        void ClearAllMeshes();
        void ForceRemeshRadius(const glm::vec3& center, float radius);
        size_t GetPendingJobCount() const;
        size_t GetCompletedResultCount() const;

    private:
        MeshManagerConfig m_config;
        MeshStats m_stats;
        Game::World* m_world = nullptr;

        // Threading and job management
        mutable std::mutex m_jobQueueMutex;
        mutable std::mutex m_resultQueueMutex;
        std::queue<MeshJob> m_pendingJobs;
        std::queue<MeshJob> m_highPriorityJobs;
        std::queue<MeshResult> m_completedResults;

        // Mesh processing
        Mesher m_mesher;
        GPUDataManager m_gpuDataManager;

        // Player tracking for prioritization
        glm::vec3 m_playerPosition{0.0f};
        mutable std::mutex m_playerMutex;

        // Dirty tracking - use struct instead of bit packing to avoid coordinate corruption
        struct DirtySectionKey {
            Game::Math::ChunkPos chunkPos;
            int sectionY;

            bool operator==(const DirtySectionKey& other) const {
                return chunkPos.x == other.chunkPos.x &&
                       chunkPos.z == other.chunkPos.z &&
                       sectionY == other.sectionY;
            }
        };

        struct DirtySectionKeyHash {
            std::size_t operator()(const DirtySectionKey& key) const {
                return std::hash<int32_t>{}(key.chunkPos.x) ^
                       (std::hash<int32_t>{}(key.chunkPos.z) << 1) ^
                       (std::hash<int>{}(key.sectionY) << 2);
            }
        };

        mutable std::mutex m_dirtyMutex;
        std::unordered_set<DirtySectionKey, DirtySectionKeyHash> m_dirtySections;

        // Frame timing
        std::chrono::steady_clock::time_point m_frameStartTime;

        // Internal methods
        void SubmitMeshJob(Game::Math::ChunkPos chunkPos, int sectionY, bool highPriority = false);
        void ProcessCompletedResults();
        void UploadMeshResult(const MeshResult& result);

        // Job processing (called by worker threads)
        void MeshCompileTask(const MeshJob& job);

        // Priority and distance calculations
        bool IsHighPriority(Game::Math::ChunkPos chunkPos, int sectionY) const;
        float CalculateDistance(Game::Math::ChunkPos chunkPos, int sectionY) const;

        // Queue management
        void EnqueueJob(const MeshJob& job);
        bool DequeueJob(MeshJob& job);
        void EnqueueResult(MeshResult&& result);
        bool DequeueResult(MeshResult& result);
    };

    // Global mesh manager instance
    extern std::unique_ptr<MeshManager> g_meshManager;

    // Utility functions for integration
    void InitializeMeshSystem(Game::World* world, const MeshManagerConfig& config = MeshManagerConfig{});
    void ShutdownMeshSystem();
    void UpdateMeshSystem(float deltaTime);

    // Convenience functions for world integration
    void MarkWorldSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY);
    void MarkWorldChunkDirty(Game::Math::ChunkPos chunkPos);
    void SetMeshSystemPlayerPosition(const glm::vec3& position);

} // namespace Render*/