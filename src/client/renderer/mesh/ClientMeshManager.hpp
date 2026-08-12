// File: src/client/renderer/mesh/ClientMeshManager.hpp
#pragma once

#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientWorkerPool.hpp"
#include "common/world/math//WorldMath.hpp"
#include "common/network/MessageQueue.hpp"
#include "common/network/PacketTypes.hpp"
#include "SectionMesh.hpp"
#include "ChunkMegaBuffer.hpp"
#include "Mesher.hpp"          // For RenderLayer enum
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

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
                                  const Network::MeshBuildResult::SectionMeshData& meshData,
                                  const VisibilitySet& visSet = VisibilitySet());

        // ========================================================================
        // GPU DATA ACCESS
        // ========================================================================

        // Get GPU data for rendering (used by ChunkRenderer)
        const GPUSectionData* GetSectionGPUData(::Game::Math::ChunkPos chunkPos, int sectionY) const;
        
        // Re-sorts ONE section's translucent quads back-to-front if its point
        // of view has changed. Port of LevelRenderer.scheduleResort (:977) plus
        // RenderSection.resortTransparency (SectionRenderDispatcher.java:309).
        //
        // WHICH sections get here and how often is the caller's decision —
        // ChunkRenderer::ScheduleTranslucentSectionResort owns that policy, the
        // same way MC splits LevelRenderer from RenderSection. Do not call this
        // in a loop over every loaded section: the cost has to be bounded by
        // what is VISIBLE, or it scales with render distance instead of view.
        //
        // Returns true if an index upload was issued (for profiling).
        // See mesh/TranslucentSort.hpp for why sorting is load-bearing here.
        bool ResortTranslucentSection(::Game::Math::ChunkPos chunkPos, int sectionY,
                                      const glm::vec3& cameraPos,
                                      bool blockPosChanged, bool isNearby);

        // Callback receives (const SectionKey&, const GPUSectionData*)
        // Iterates m_gpuData directly — it only contains sections with geometry
        // (empty sections are erased at upload time), eliminating the old
        // m_activeSections set and its redundant per-entry hash lookup.
        template<typename Func>
        void ForEachActiveSection(Func&& fn) const {
            std::shared_lock<std::shared_mutex> lock(m_gpuDataMutex);
            for (const auto& [key, data] : m_gpuData) {
                fn(key, &data);
            }
        }
        
        // Remove GPU data for chunk section
        void RemoveSectionGPUData(::Game::Math::ChunkPos chunkPos, int sectionY);

        // Remove GPU data for entire chunk (all 24 sections)
        void RemoveChunkGPUData(::Game::Math::ChunkPos chunkPos);

        // ========================================================================
        // MEGA-BUFFER ACCESS (for ChunkRenderer multi-draw)
        // ========================================================================

        // Get the mega-buffer for a given render layer (opaque/cutout/translucent)
        ChunkMegaBuffer* GetMegaBuffer(RenderLayer layer);

        // Direct GPU data lookup by section position (for occlusion graph BFS)
        GPUSectionData* GetGPUSectionData(::Game::Math::ChunkPos chunkPos, int sectionY) {
            std::shared_lock<std::shared_mutex> lock(m_gpuDataMutex);
            SectionKey key{chunkPos, sectionY};
            auto it = m_gpuData.find(key);
            return (it != m_gpuData.end()) ? &it->second : nullptr;
        }

        // ========================================================================
        // SHARED BLOCK VAO (GL_ARB_vertex_attrib_binding)
        // ========================================================================

        // Bind the shared block vertex format (call once per frame before any
        // mega-buffer BindSlab calls). GL: binds shared VAO. VK: no-op.
        void BindSharedBlockVAO();

        // ========================================================================
        // CONFIGURATION
        // ========================================================================

        struct ClientMeshConfig {
            // Time budgets (primary controls)
            float meshBuildBudgetMs = 50.0f;        // Time budget for mesh scheduling per frame (chunks)

            // NOTE: gpuUploadBudgetMs / maxGPUUploadsPerFrame / maxPendingBuilds
            // used to live here. The first two throttled the upload drain by CPU
            // time and count — neither tracks what the GPU actually has to move,
            // so the frame paid for the backlog at the swap instead. The third
            // was never referenced by any code at all. Uploads are now bounded
            // at the source by MeshUploadPermits (see MeshUploadPermits.hpp).

            // Safety caps (rarely hit when budgets are enforced)
            int maxMeshSubmitsPerFrame = 16;        // Safety cap for mesh submissions
            
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

        // Get GPU data counts for debug UI
        size_t GetGPUDataCount() const { std::shared_lock<std::shared_mutex> lock(m_gpuDataMutex); return m_gpuData.size(); }
        size_t GetActiveSectionCount() const { std::shared_lock<std::shared_mutex> lock(m_gpuDataMutex); return m_gpuData.size(); }

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
        // SHARED BLOCK VERTEX FORMAT
        // ========================================================================
        //
        // Managed by the render backend. GL: shared VAO with vertex attrib binding.
        // VK: no-op (vertex input is part of pipeline state).
        //
        void CreateSharedBlockVAO();
        void DestroySharedBlockVAO();

        // ========================================================================
        // MEGA-BUFFERS (one per render layer)
        // ========================================================================

        ChunkMegaBuffer m_opaqueMegaBuffer;
        ChunkMegaBuffer m_cutoutMegaBuffer;
        ChunkMegaBuffer m_translucentMegaBuffer;

        // ========================================================================
        // GPU DATA STORAGE
        // ========================================================================

        // GPU data storage
        // Use shared_mutex for reader-writer lock pattern
        // Multiple threads can read concurrently, but writes are exclusive
        mutable std::shared_mutex m_gpuDataMutex;
        using GpuDataMap = std::unordered_map<SectionKey, GPUSectionData, SectionKeyHash>;
        GpuDataMap m_gpuData;

        // Scratch for translucent re-sorting. Render-thread only — the sort
        // runs inline, so these are shared across sections rather than
        // per-worker (MC needs a SectionBufferBuilderPack per worker because
        // its equivalent runs on the chunk-compile pool).
        //
        // The round-robin cursor lives on ChunkRenderer with the visible list
        // it indexes into, mirroring MC's translucencyResortIterationIndex on
        // LevelRenderer.
        std::vector<uint16_t> m_resortIndexScratch;
        std::vector<uint32_t> m_resortOrderScratch;

        // NOTE: m_activeSections was removed — m_gpuData only contains sections
        // with geometry (empty sections are erased at upload time), so iterating
        // m_gpuData directly is equivalent and avoids a redundant hash set.

        // --- Deferred GPUSectionData destruction (tombstones) ---
        // The chunk renderer's cached reachable lists and in-flight BFS
        // results hold raw GPUSectionData pointers. Destroying an object the
        // moment its section dies would force the renderer to invalidate
        // every cached list and rebuild the occlusion BFS synchronously on
        // the main thread (the old MarkSectionDataErased hammer — it fired
        // nearly every frame during movement). Instead, a dying entry is
        // tombstoned: its draw commands are invalidated in place (so stale
        // lists render nothing for it) and its map node is EXTRACTED — which
        // preserves the element's address — into this graveyard. The node is
        // destroyed only once every cached list that could reference it has
        // been rebuilt (see ReclaimGpuDataTombstones / ChunkRenderer::
        // GetMinLiveBuildCounter). MarkSectionDataErased remains only for
        // Shutdown, where m_gpuData.clear() genuinely frees everything.
        struct GpuDataTombstone {
            GpuDataMap::node_type node;
            uint32_t deathCounter;  // renderer prepare-counter at extraction
        };
        std::vector<GpuDataTombstone> m_gpuDataGraveyard;

        // Caller must hold m_gpuDataMutex exclusively.
        void TombstoneGpuDataEntry(GpuDataMap::iterator it);
        // Destroys tombstones no live cached list can reference (per frame).
        void ReclaimGpuDataTombstones();

        // Deferred GPU resource destruction queue.
        // Instead of destroying GPU buffers synchronously during chunk unload
        // (which stalls the main thread), we queue the GPUSectionData for
        // destruction in the next PerformGPUUploads() call, spreading the cost
        // across frames.
        std::vector<GPUSectionData> m_pendingDestroys;
        void ProcessPendingDestroys();


        // ========================================================================
        // INTERNAL METHODS
        // ========================================================================

        // Process single mesh build result
        void ProcessMeshBuildResult(const Network::MeshBuildResult& result);

        // Upload mesh results within time budget
        // Drains every pending mesh result. Bounded by the upload permit pool
        // (MeshUploadPermits), not by a time or count budget — see the comment
        // on the definition and MC's uploadAllPendingUploads.
        void UploadAllPendingResults();

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