// File: src/client/renderer/mesh/ClientMeshManager.hpp
#pragma once

#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientWorkerPool.hpp"
#include "common/world/math//WorldMath.hpp"
#include "common/network/MessageQueue.hpp"
#include "common/network/PacketTypes.hpp"
#include "SectionMesh.hpp"
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace Render {

    // Client-side mesh manager (mesh building only, no world access)
    // Coordinates with ClientChunkManager and ClientWorkerPool
    class ClientMeshManager {
    public:
        // Section key for identifying chunk sections
        struct SectionKey {
            ::Game::Math::ChunkPos chunkPos;
            int sectionY;

            bool operator==(const SectionKey& other) const {
                return chunkPos.x == other.chunkPos.x &&
                       chunkPos.z == other.chunkPos.z &&
                       sectionY == other.sectionY;
            }
        };

        struct SectionKeyHash {
            std::size_t operator()(const SectionKey& key) const {
                size_t h = std::hash<int32_t>{}(key.chunkPos.x);
                h ^= std::hash<int32_t>{}(key.chunkPos.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(key.sectionY) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };
    
    public:
        explicit ClientMeshManager();
        ~ClientMeshManager();

        // Non-copyable, non-movable
        ClientMeshManager(const ClientMeshManager&) = delete;
        ClientMeshManager& operator=(const ClientMeshManager&) = delete;

        // ========================================================================
        // LIFECYCLE
        // ========================================================================

        void Initialize(Client::ClientChunkManager* chunkManager);
        void Shutdown();

        // ========================================================================
        // FRAME PROCESSING (Called by ClientThread)
        // ========================================================================

        // Process mesh build results from ClientWorkerPool
        void ProcessMeshBuildResults();

        // Schedule new mesh builds for LOADED chunks
        void ScheduleMeshBuilds(const glm::vec3& playerPosition);

        // Upload completed meshes to GPU within time budget
        void PerformGPUUploads();

        // ========================================================================
        // PLAYER POSITION UPDATES
        // ========================================================================

        void SetPlayerPosition(const glm::vec3& position);
        glm::vec3 GetPlayerPosition() const;

        // ========================================================================
        // MESH SCHEDULING
        // ========================================================================

        // Cancel mesh jobs for chunk (when unloading)
        void CancelMeshJobs(::Game::Math::ChunkPos chunkPos);

        // ========================================================================
        // GPU UPLOAD COORDINATION
        // ========================================================================

        // Check if mesh upload is ready for chunk section
        bool IsMeshUploadReady(::Game::Math::ChunkPos chunkPos, int sectionY) const;

        // Upload mesh build result directly to GPU data storage (stores in atomic pointers for lock-free rendering)
        void UploadMeshResultToGPU(::Game::Math::ChunkPos chunkPos, int sectionY,
                                  const Network::MeshBuildResult::SectionMeshData& meshData);

        // ========================================================================
        // GPU DATA ACCESS
        // ========================================================================

        // Get GPU data for rendering (used by ChunkRenderer)
        const GPUSectionData* GetSectionGPUData(::Game::Math::ChunkPos chunkPos, int sectionY) const;
        
        // Iterate all active sections under shared lock (zero-copy, zero-alloc)
        // Callback receives (const SectionKey&, const GPUSectionData*)
        template<typename Func>
        void ForEachActiveSection(Func&& fn) const {
            std::shared_lock<std::shared_mutex> lock(m_gpuDataMutex);
            for (const auto& key : m_activeSections) {
                auto it = m_gpuData.find(key);
                if (it != m_gpuData.end()) {
                    fn(key, &it->second);
                }
            }
        }
        
        // Remove GPU data for chunk section
        void RemoveSectionGPUData(::Game::Math::ChunkPos chunkPos, int sectionY);

        // Remove GPU data for entire chunk (all 24 sections)
        void RemoveChunkGPUData(::Game::Math::ChunkPos chunkPos);

        // ========================================================================
        // CONFIGURATION
        // ========================================================================

        struct ClientMeshConfig {
            // Time budgets (primary controls)
            float meshBuildBudgetMs = 50.0f;        // Time budget for mesh scheduling per frame (chunks)
            float gpuUploadBudgetMs = 2.0f;         // Time budget for GPU uploads per frame
            
            // Safety caps (rarely hit when budgets are enforced)
            int maxMeshSubmitsPerFrame = 16;        // Safety cap for mesh submissions
            int maxGPUUploadsPerFrame = 8;          // Safety cap for GPU uploads
            int maxPendingBuilds = 128;             // Max pending mesh builds (OOM guard)
            
            // Priority settings
            bool enablePriorityScheduling = true;   // Use distance-based priority
            float highPriorityRadius = 64.0f;       // High priority radius in blocks
        };

        void SetConfig(const ClientMeshConfig& config) { m_config = config; }
        const ClientMeshConfig& GetConfig() const { return m_config; }

        // ========================================================================
        // STATISTICS
        // ========================================================================

        struct ClientMeshStats {
            std::atomic<size_t> meshBuildsScheduled{0};
            std::atomic<size_t> meshBuildsCompleted{0};
            std::atomic<size_t> meshUploadedToGPU{0};
            std::atomic<size_t> meshBuildsCancelled{0};
            std::atomic<size_t> meshBuildsSkipped{0};
            
            // Per-frame counters
            int meshBuildsThisFrame = 0;
            int meshUploadsThisFrame = 0;
            float meshSchedulingTimeMs = 0.0f;
            float gpuUploadTimeMs = 0.0f;
            
            void ResetFrameCounters() {
                meshBuildsThisFrame = 0;
                meshUploadsThisFrame = 0;
                meshSchedulingTimeMs = 0.0f;
                gpuUploadTimeMs = 0.0f;
            }
            
            void Reset() {
                meshBuildsScheduled = meshBuildsCompleted = meshUploadedToGPU = 0;
                meshBuildsCancelled = meshBuildsSkipped = 0;
                ResetFrameCounters();
            }
        };

        const ClientMeshStats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats.Reset(); }
        void LogStats() const;


        // ========================================================================
        // QUEUE ACCESS (for worker pools)
        // ========================================================================
        
        // Get the mesh result queue (used by ClientWorkerPool to submit results)
        static Network::ResultQueue<Network::MeshBuildResult>& GetMeshResultQueue();

        // ========================================================================
        // DEBUG AND UTILITIES
        // ========================================================================

        // Get number of pending mesh builds
        static size_t GetPendingMeshBuildCount();

        // Get number of completed results waiting for upload
        static size_t GetCompletedResultCount();

        // Force mesh rebuild for debugging
        void ForceMeshRebuild(::Game::Math::ChunkPos chunkPos);

        // Clear all mesh data
        static void ClearAllMeshes();


    private:
        // Configuration
        ClientMeshConfig m_config;

        // System references
        Client::ClientChunkManager* m_chunkManager = nullptr;

        // Player position for prioritization
        mutable std::mutex m_playerMutex;
        glm::vec3 m_playerPosition{0.0f};

        // Statistics
        ClientMeshStats m_stats;

        // Frame timing
        std::chrono::steady_clock::time_point m_frameStartTime;

        // ========================================================================
        // GPU DATA STORAGE
        // ========================================================================

        // GPU data storage
        // Use shared_mutex for reader-writer lock pattern
        // Multiple threads can read concurrently, but writes are exclusive
        mutable std::shared_mutex m_gpuDataMutex;
        std::unordered_map<SectionKey, GPUSectionData, SectionKeyHash> m_gpuData;
        
        // Track active sections (sections that have GPU data)
        // This allows ChunkRenderer to iterate only sections with geometry
        std::unordered_set<SectionKey, SectionKeyHash> m_activeSections;


        // ========================================================================
        // INTERNAL METHODS
        // ========================================================================

        // Process single mesh build result
        void ProcessMeshBuildResult(const Network::MeshBuildResult& result);

        // Upload mesh results within time budget
        void UploadMeshResultsWithBudget();

        // Check if chunk needs mesh builds
        bool ChunkNeedsMeshBuild(::Game::Math::ChunkPos chunkPos) const;

        // Get mesh build priority for chunk section
        float CalculateMeshPriority(::Game::Math::ChunkPos chunkPos, int sectionY) const;

        // Check if section is within high priority radius
        bool IsHighPriority(::Game::Math::ChunkPos chunkPos, int sectionY) const;

        // Validate mesh build result
        static bool ValidateMeshBuildResult(const Network::MeshBuildResult& result);

        // Log mesh build activity
        static void LogMeshActivity(const std::string& activity, ::Game::Math::ChunkPos chunkPos, int sectionY = -1);

    };

    // ========================================================================
    // GLOBAL ACCESS
    // ========================================================================

    // Global client mesh manager instance
    extern std::unique_ptr<ClientMeshManager> g_clientMeshManager;

    // Convenience functions
    void InitializeClientMeshManager(Client::ClientChunkManager* chunkManager);
    void ShutdownClientMeshManager();

    // Frame processing functions (called by ClientThread)
    void ProcessClientMeshBuildResults();
    void ScheduleClientMeshBuilds(const glm::vec3& playerPosition);
    void PerformClientGPUUploads();

    // Player position updates
    void SetClientMeshPlayerPosition(const glm::vec3& position);

    // Mesh scheduling
    void CancelClientMeshJobs(::Game::Math::ChunkPos chunkPos);


} // namespace Render