// File: src/render/mesh/MeshManager.hpp (FIXED)
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
#include <atomic>

#include "world/World.hpp"

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
        int meshRadius = 8;                 // Distance around player to mesh
        bool enableAsyncBuilding = true;    // Use background threads
        bool enableFrustumCulling = true;   // Only mesh visible sections

        // Priority settings
        float highPriorityRadius = 3.0f;    // High priority radius
        int maxHighPriorityJobs = 50;       // Limit high priority queue
    };

    // **FIXED**: Thread-safe statistics using atomics
    class ThreadSafeMeshStats {
    private:
        std::atomic<int> m_meshesBuiltThisFrame{0};
        std::atomic<int> m_meshesUploadedThisFrame{0};
        std::atomic<int> m_jobsSubmittedThisFrame{0};
        std::atomic<float> m_buildTimeMs{0.0f};
        std::atomic<float> m_uploadTimeMs{0.0f};
        std::atomic<size_t> m_pendingJobs{0};
        std::atomic<size_t> m_completedResults{0};
        std::atomic<size_t> m_totalGPUMemory{0};
        std::atomic<size_t> m_activeSections{0};

    public:
        // Atomic operations for thread-safe updates
        void fetch_add_meshesBuiltThisFrame(int value) { m_meshesBuiltThisFrame.fetch_add(value); }
        void fetch_add_meshesUploadedThisFrame(int value) { m_meshesUploadedThisFrame.fetch_add(value); }
        void fetch_add_jobsSubmittedThisFrame(int value) { m_jobsSubmittedThisFrame.fetch_add(value); }
        void fetch_add_buildTimeMs(float value) {
            float current = m_buildTimeMs.load();
            while (!m_buildTimeMs.compare_exchange_weak(current, current + value)) {}
        }
        void fetch_set_uploadTimeMs(float value) { m_uploadTimeMs.store(value); }

        // Getters (thread-safe reads)
        int getMeshesBuiltThisFrame() const { return m_meshesBuiltThisFrame.load(); }
        int getMeshesUploadedThisFrame() const { return m_meshesUploadedThisFrame.load(); }
        int getJobsSubmittedThisFrame() const { return m_jobsSubmittedThisFrame.load(); }
        float getBuildTimeMs() const { return m_buildTimeMs.load(); }
        float getUploadTimeMs() const { return m_uploadTimeMs.load(); }
        size_t getPendingJobs() const { return m_pendingJobs.load(); }
        size_t getCompletedResults() const { return m_completedResults.load(); }
        size_t getTotalGPUMemory() const { return m_totalGPUMemory.load(); }
        size_t getActiveSections() const { return m_activeSections.load(); }

        // Setters for non-accumulating values
        void setPendingJobs(size_t value) { m_pendingJobs.store(value); }
        void setCompletedResults(size_t value) { m_completedResults.store(value); }
        void setTotalGPUMemory(size_t value) { m_totalGPUMemory.store(value); }
        void setActiveSections(size_t value) { m_activeSections.store(value); }

        // Reset frame counters
        void Reset() {
            m_meshesBuiltThisFrame.store(0);
            m_meshesUploadedThisFrame.store(0);
            m_jobsSubmittedThisFrame.store(0);
            m_buildTimeMs.store(0.0f);
            m_uploadTimeMs.store(0.0f);
        }

        // For backward compatibility, provide a non-atomic struct view
        struct MeshStats {
            int meshesBuiltThisFrame;
            int meshesUploadedThisFrame;
            int jobsSubmittedThisFrame;
            float buildTimeMs;
            float uploadTimeMs;
            size_t pendingJobs;
            size_t completedResults;
            size_t totalGPUMemory;
            size_t activeSections;
        };

        MeshStats load() const {
            return MeshStats{
                m_meshesBuiltThisFrame.load(),
                m_meshesUploadedThisFrame.load(),
                m_jobsSubmittedThisFrame.load(),
                m_buildTimeMs.load(),
                m_uploadTimeMs.load(),
                m_pendingJobs.load(),
                m_completedResults.load(),
                m_totalGPUMemory.load(),
                m_activeSections.load()
            };
        }

        void store(const MeshStats& stats) {
            m_pendingJobs.store(stats.pendingJobs);
            m_completedResults.store(stats.completedResults);
            m_totalGPUMemory.store(stats.totalGPUMemory);
            m_activeSections.store(stats.activeSections);
            // Note: frame counters and times are not set here as they're accumulated
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

        // **FIXED**: Thread-safe statistics access
        ThreadSafeMeshStats::MeshStats GetStats() const { return m_stats.load(); }
        void ResetFrameStats() { m_stats.Reset(); }

        // Debug and utilities
        void ClearAllMeshes();
        void ForceRemeshRadius(const glm::vec3& center, float radius);
        size_t GetPendingJobCount() const;
        size_t GetCompletedResultCount() const;

    private:
        MeshManagerConfig m_config;
        ThreadSafeMeshStats m_stats;  // **FIXED**: Thread-safe stats
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

        // Player tracking for prioritization - **FIXED**: Better synchronization
        glm::vec3 m_playerPosition{0.0f};
        mutable std::mutex m_playerMutex;

        // **FIXED**: Improved dirty tracking with better hash distribution
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
                // **FIXED**: Better hash distribution to avoid clustering
                std::size_t h1 = std::hash<int32_t>{}(key.chunkPos.x);
                std::size_t h2 = std::hash<int32_t>{}(key.chunkPos.z);
                std::size_t h3 = std::hash<int>{}(key.sectionY);

                // Mix the hash values better to avoid clustering
                h1 ^= h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
                h1 ^= h3 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
                return h1;
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

        // Limits and cleanup
        void EnforceJobLimits();
        void CleanupOldResults();
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

} // namespace Render