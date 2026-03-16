// File: src/client/renderer/mesh/ClientMeshManager.cpp
#include "ClientMeshManager.hpp"
#include "ChunkRenderer.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../../world/ClientWorkerPool.hpp"
#include "../backend/RenderBackend.hpp"
#include <algorithm>
#include <chrono>

namespace Render {

    // Implementation of GPUSectionData::DestroyAllResources (declared in SectionMesh.hpp)
    // No-op: GPU resources are now owned by ChunkMegaBuffer, not per-section handles.
    // Kept for API compatibility with legacy code paths (GPUDataPool, ChunkMeshData).
    void GPUSectionData::DestroyAllResources(RenderBackend* backend) {
        (void)backend;
        opaqueIndexCount = cutoutIndexCount = translucentIndexCount = 0;
        opaqueVertexCount = cutoutVertexCount = translucentVertexCount = 0;
    }

    // Global instance
    std::unique_ptr<ClientMeshManager> g_clientMeshManager = nullptr;
    
    // Static mesh result queue (shared between ClientMeshManager and ClientWorkerPool)
    static Network::ResultQueue<Network::MeshBuildResult> s_meshResultQueue;

    ClientMeshManager::ClientMeshManager() {
        Log::Info("ClientMeshManager created");
    }

    ClientMeshManager::~ClientMeshManager() {
        Shutdown();
        Log::Info("ClientMeshManager destroyed");
    }

    void ClientMeshManager::Initialize(Client::ClientChunkManager* chunkManager) {
        if (!chunkManager) {
            Log::Error("Cannot initialize ClientMeshManager with null chunk manager");
            return;
        }

        m_chunkManager = chunkManager;

        // Reset statistics
        m_stats.Reset();

        // Initialize player position
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            m_playerPosition = glm::vec3(0.0f, 67.0f, 0.0f);
        }

        // Initialize mega-buffer slab pools (one pool per render layer).
        // Slab sizes: fixed-size GPU buffers that never grow (new slabs allocated when full).
        m_opaqueMegaBuffer.Initialize(512000, 1024000);   // 512K verts/slab (~12MB each)
        m_cutoutMegaBuffer.Initialize(256000, 512000);     // 256K verts/slab (~6MB each)
        m_translucentMegaBuffer.Initialize(256000, 512000);

        // Create the shared block VAO (GL_ARB_vertex_attrib_binding).
        // One VAO defines the vertex format; mega-buffer VBOs are switched at
        // render time with glBindVertexBuffer — no GPU pipeline flush.
        CreateSharedBlockVAO();

        Log::Info("ClientMeshManager initialized successfully");
    }

    void ClientMeshManager::Shutdown() {
        Log::Info("Shutting down ClientMeshManager...");

        // Destroy the shared block VAO before mega-buffers (clean teardown order)
        DestroySharedBlockVAO();

        // Shut down mega-buffers (frees all GPU resources they own)
        m_opaqueMegaBuffer.Shutdown();
        m_cutoutMegaBuffer.Shutdown();
        m_translucentMegaBuffer.Shutdown();

        // Clean up GPU data tracking (no per-section GPU resources to destroy anymore)
        {
            std::unique_lock<std::shared_mutex> lock(m_gpuDataMutex);
            m_gpuData.clear();
        }

        // Clear any pending destroys (mega-buffers already cleaned up)
        m_pendingDestroys.clear();

        // Log final statistics
        LogStats();

        m_chunkManager = nullptr;

        Log::Info("ClientMeshManager shutdown complete");
    }

    // ========================================================================
    // FRAME PROCESSING
    // ========================================================================

    void ClientMeshManager::ProcessMeshBuildResults() {
        if (!m_chunkManager) return;

        auto startTime = std::chrono::steady_clock::now();

        // Drain all completed mesh build results
        auto& meshResultQueue = GetMeshResultQueue();
        auto results = meshResultQueue.DrainAll();

        for (const auto& result : results) {
            ProcessMeshBuildResult(result);
        }

        meshResultQueue.ResetProcessedCount();

        // Record timing
        auto endTime = std::chrono::steady_clock::now();
        float processingTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        if (!results.empty()) {
            Log::Debug("Processed %zu mesh build results in %.2fms", results.size(), processingTime);
        }
    }

    void ClientMeshManager::ScheduleMeshBuilds(const glm::vec3& playerPosition) {
        if (!m_chunkManager) return;

        SetPlayerPosition(playerPosition);

        // No timer — run every frame. Buffer pool backpressure in
        // ScheduleMeshBuildsWithSnapshots limits submissions naturally.
        auto startTime = std::chrono::steady_clock::now();
        m_chunkManager->ScheduleMeshBuildsWithSnapshots(playerPosition);

        auto endTime = std::chrono::steady_clock::now();
        m_stats.meshSchedulingTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    void ClientMeshManager::PerformGPUUploads() {
        PROFILE_ZONE;
        if (!m_chunkManager) return;

        // Destroy any GPU resources that were deferred during chunk unloads
        ProcessPendingDestroys();

        // Periodic cleanup: remove empty slabs from mega-buffer pools.
        // With slab pool architecture, this just frees unused GPU memory — no data copy.
        static int compactFrameCounter = 0;
        if (++compactFrameCounter >= 600) {
            compactFrameCounter = 0;
            m_opaqueMegaBuffer.CompactIfNeeded();
            m_cutoutMegaBuffer.CompactIfNeeded();
            m_translucentMegaBuffer.CompactIfNeeded();
        }

        m_stats.meshUploadsThisFrame = 0;
        auto startTime = std::chrono::steady_clock::now();

        UploadMeshResultsWithBudget();
        
        // Record timing
        auto endTime = std::chrono::steady_clock::now();
        float uploadTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        m_stats.gpuUploadTimeMs = uploadTime;
        
        if (uploadTime > m_config.gpuUploadBudgetMs && m_stats.meshUploadsThisFrame > 0) {
            Log::Debug("GPU uploads exceeded budget: %.2fms > %.2fms (uploaded %d meshes)", 
                      uploadTime, m_config.gpuUploadBudgetMs, m_stats.meshUploadsThisFrame);
        }
    }

    // ========================================================================
    // PLAYER POSITION UPDATES
    // ========================================================================

    void ClientMeshManager::SetPlayerPosition(const glm::vec3& position) {
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            m_playerPosition = position;
        }
        
        // Update ClientWorkerPool player position for prioritization
        Threading::SetClientWorkerPlayerPosition(position);
    }

    glm::vec3 ClientMeshManager::GetPlayerPosition() const {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        return m_playerPosition;
    }

    // ========================================================================
    // MESH SCHEDULING
    // ========================================================================

    void ClientMeshManager::CancelMeshJobs(::Game::Math::ChunkPos chunkPos) {
        Threading::CancelClientMeshJob(chunkPos);
        m_stats.meshBuildsCancelled.fetch_add(1, std::memory_order_relaxed);
        LogMeshActivity("Cancelled mesh jobs", chunkPos);
    }

    // ========================================================================
    // GPU UPLOAD COORDINATION
    // ========================================================================

    // ========================================================================
    // STATISTICS
    // ========================================================================

    void ClientMeshManager::LogStats() const {
        Log::Info("ClientMeshManager Statistics:");
        Log::Info("  Mesh Builds Scheduled: %zu", m_stats.meshBuildsScheduled.load());
        Log::Info("  Mesh Builds Completed: %zu", m_stats.meshBuildsCompleted.load());
        Log::Info("  Meshes Uploaded to GPU: %zu", m_stats.meshUploadedToGPU.load());
        Log::Info("  Mesh Builds Cancelled: %zu", m_stats.meshBuildsCancelled.load());
        Log::Info("  Mesh Builds Skipped: %zu", m_stats.meshBuildsSkipped.load());
        Log::Info("  Pending Builds: %zu", GetPendingMeshBuildCount());
        Log::Info("  Completed Results: %zu", GetCompletedResultCount());
    }


    void ClientMeshManager::LogMeshActivity(const std::string& activity, ::Game::Math::ChunkPos chunkPos, int sectionY) {
        if (sectionY >= 0) {
            Log::Debug("%s for chunk (%d, %d) section %d",
                      activity.c_str(), chunkPos.x, chunkPos.z, sectionY);
        } else {
            Log::Debug("%s for chunk (%d, %d)", activity.c_str(), chunkPos.x, chunkPos.z);
        }
    }

    // ========================================================================
    // DEBUG AND UTILITIES
    // ========================================================================

    size_t ClientMeshManager::GetPendingMeshBuildCount() {
        return Threading::g_clientWorkerPool ? 
               Threading::g_clientWorkerPool->GetPendingJobCount() : 0;
    }

    size_t ClientMeshManager::GetCompletedResultCount() {
        return GetMeshResultQueue().Size();
    }

    void ClientMeshManager::ForceMeshRebuild(::Game::Math::ChunkPos chunkPos) {
        CancelMeshJobs(chunkPos);
        // Mark all sections as dirty instead of direct scheduling
        if (m_chunkManager) {
            m_chunkManager->MarkChunkDirty(chunkPos);
        }
        LogMeshActivity("Forced mesh rebuild", chunkPos);
    }

    void ClientMeshManager::ClearAllMeshes() {
        if (Threading::g_clientWorkerPool) {
            Threading::g_clientWorkerPool->CancelAllJobs();
        }
        
        // Clear completed results
        GetMeshResultQueue().Clear();
        
        Log::Info("Cleared all mesh data");
    }
    
    Network::ResultQueue<Network::MeshBuildResult>& ClientMeshManager::GetMeshResultQueue() {
        return s_meshResultQueue;
    }

    // ========================================================================
    // INTERNAL METHODS
    // ========================================================================
    
    void ClientMeshManager::ProcessMeshBuildResult(const Network::MeshBuildResult& result) {
        PROFILE_ZONE_N("ProcessMeshResult");
        // Let ClientChunkManager decide whether to accept or drop this result
        auto decision = m_chunkManager->AcceptMeshResult(result);
        
        switch (decision.action) {
            case Client::MeshApplyAction::Upload: {
                // Check if the build succeeded
                if (!result.success) {
                    Log::Warning("Failed mesh build for chunk (%d, %d) section %d",
                                result.chunkPos.x, result.chunkPos.z, result.sectionY);
                    m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                // Validate result
                if (!ValidateMeshBuildResult(result)) {
                    Log::Error("Invalid mesh build result for chunk (%d, %d) section %d",
                              result.chunkPos.x, result.chunkPos.z, result.sectionY);
                    m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                // Final check: chunk may have been unloaded between AcceptMeshResult and here
                if (m_chunkManager && !m_chunkManager->IsChunkLoaded(result.chunkPos)) {
                    Log::Debug("Chunk (%d, %d) unloaded before GPU upload, skipping",
                              result.chunkPos.x, result.chunkPos.z);
                    m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                // Upload to GPU
                UploadMeshResultToGPU(result.chunkPos, result.sectionY, result.meshData, result.visibilitySet);
                
                // Tell ClientChunkManager the upload completed successfully
                m_chunkManager->FinalizeSectionUpload(result.chunkPos, result.sectionY, result.neighborMask);
                
                // Update statistics
                m_stats.meshBuildsCompleted.fetch_add(1, std::memory_order_relaxed);
                
                LogMeshActivity("Uploaded mesh", result.chunkPos, result.sectionY);
                break;
            }
            
            case Client::MeshApplyAction::Drop_StaleVersion:
                m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                Log::Debug("MESH DROPPED STALE: chunk (%d,%d) section %d gen=%u",
                         result.chunkPos.x, result.chunkPos.z, result.sectionY, result.generation);
                break;

            case Client::MeshApplyAction::Drop_Unloaded:
                m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                Log::Debug("MESH DROPPED UNLOADED: chunk (%d,%d) section %d",
                         result.chunkPos.x, result.chunkPos.z, result.sectionY);
                break;
                
            case Client::MeshApplyAction::Drop_Replaced:
                // Superseded by newer mesh
                m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                LogMeshActivity("Dropped replaced mesh", result.chunkPos, result.sectionY);
                break;
        }
    }
    
    void ClientMeshManager::UploadMeshResultsWithBudget() {
        auto startTime = std::chrono::steady_clock::now();
        int uploadsThisFrame = 0;

        auto& meshResultQueue = GetMeshResultQueue();

        while (uploadsThisFrame < m_config.maxGPUUploadsPerFrame) {
            auto currentTime = std::chrono::steady_clock::now();
            float elapsedMs = std::chrono::duration<float, std::milli>(currentTime - startTime).count();
            if (elapsedMs >= m_config.gpuUploadBudgetMs) {
                break;
            }

            Network::MeshBuildResult result;
            if (!meshResultQueue.try_pop(result)) {
                break;
            }

            // Use the SAME processing path as ProcessMeshBuildResults()
            // This ensures AcceptMeshResult() validation and FinalizeSectionUpload() are called,
            // preventing sections from getting stuck in MESHING state with dirty=true forever
            ProcessMeshBuildResult(result);
            uploadsThisFrame++;
            m_stats.meshUploadsThisFrame++;
        }
    }
    
    bool ClientMeshManager::ChunkNeedsMeshBuild(::Game::Math::ChunkPos chunkPos) const {
        auto* chunk = m_chunkManager->GetChunk(chunkPos);
        if (!chunk || chunk->state != Client::ChunkState::LOADED) {
            return false;
        }
        
        // Check if any sections are dirty
        return !chunk->dirtySections.empty();
    }
    
    float ClientMeshManager::CalculateMeshPriority(::Game::Math::ChunkPos chunkPos, int sectionY) const {
        glm::vec3 playerPos = GetPlayerPosition();
        
        // Calculate center of section in world space
        float sectionWorldY = static_cast<float>(sectionY * 16 + Config::MinY + 8);
        glm::vec3 sectionCenter(
            chunkPos.x * 16.0f + 8.0f,
            sectionWorldY,
            chunkPos.z * 16.0f + 8.0f
        );
        
        // Calculate distance from player
        float distance = glm::length(sectionCenter - playerPos);
        
        // Invert distance for priority (closer = higher priority)
        // Add small epsilon to avoid division by zero
        return 1000.0f / (distance + 1.0f);
    }
    
    bool ClientMeshManager::IsHighPriority(::Game::Math::ChunkPos chunkPos, int sectionY) const {
        glm::vec3 playerPos = GetPlayerPosition();
        
        // Calculate center of section in world space
        float sectionWorldY = static_cast<float>(sectionY * 16 + Config::MinY + 8);
        glm::vec3 sectionCenter(
            chunkPos.x * 16.0f + 8.0f,
            sectionWorldY,
            chunkPos.z * 16.0f + 8.0f
        );
        
        // Check if within high priority radius
        float distance = glm::length(sectionCenter - playerPos);
        return distance <= m_config.highPriorityRadius;
    }
    
    bool ClientMeshManager::ValidateMeshBuildResult(const Network::MeshBuildResult& result) {
        // Validate section index
        if (result.sectionY < 0 || result.sectionY >= Game::Math::SECTIONS_PER_CHUNK) {
            return false;
        }
        
        // Check if mesh is completely empty (all air) - this is valid
        bool hasOpaque = !result.meshData.opaqueVertices.empty();
        bool hasCutout = !result.meshData.cutoutVertices.empty();
        bool hasTranslucent = !result.meshData.translucentVertices.empty();
        
        if (!hasOpaque && !hasCutout && !hasTranslucent) {
            // Empty mesh is valid (section is all air)
            return true;
        }
        
        // Validate each non-empty layer's consistency
        // Opaque layer
        if (hasOpaque) {
            if (result.meshData.opaqueVertices.empty() != result.meshData.opaqueIndices.empty()) {
                return false;
            }
            if (result.meshData.opaqueVertexCount * 6 != result.meshData.opaqueVertices.size()) {
                return false; // 6 float-sized slots per vertex
            }
            if (result.meshData.opaqueIndexCount != result.meshData.opaqueIndices.size()) {
                return false;
            }
        }
        
        // Cutout layer
        if (hasCutout) {
            if (result.meshData.cutoutVertices.empty() != result.meshData.cutoutIndices.empty()) {
                return false;
            }
            if (result.meshData.cutoutVertexCount * 6 != result.meshData.cutoutVertices.size()) {
                return false;
            }
            if (result.meshData.cutoutIndexCount != result.meshData.cutoutIndices.size()) {
                return false;
            }
        }
        
        // Translucent layer
        if (hasTranslucent) {
            if (result.meshData.translucentVertices.empty() != result.meshData.translucentIndices.empty()) {
                return false;
            }
            if (result.meshData.translucentVertexCount * 6 != result.meshData.translucentVertices.size()) {
                return false;
            }
            if (result.meshData.translucentIndexCount != result.meshData.translucentIndices.size()) {
                return false;
            }
        }
        
        return true;
    }

    // ========================================================================
    // GPU DATA ACCESS
    // ========================================================================

    const GPUSectionData* ClientMeshManager::GetSectionGPUData(::Game::Math::ChunkPos chunkPos, int sectionY) const {
        // Use shared_lock for concurrent reads - much faster than exclusive lock
        std::shared_lock<std::shared_mutex> lock(m_gpuDataMutex);
        
        SectionKey key{chunkPos, sectionY};
        auto it = m_gpuData.find(key);
        if (it != m_gpuData.end()) {
            return &it->second;
        }
        
        return nullptr;
    }

    void ClientMeshManager::RemoveSectionGPUData(::Game::Math::ChunkPos chunkPos, int sectionY) {
        std::unique_lock<std::shared_mutex> lock(m_gpuDataMutex);

        SectionKey key{chunkPos, sectionY};
        auto it = m_gpuData.find(key);
        if (it != m_gpuData.end()) {
            // Tell the grid this section is gone
            m_chunkManager->NotifyRenderGridSectionUpdated(chunkPos, sectionY, nullptr);

            // Remove from mega-buffers (frees GPU regions)
            MegaBufferSectionKey megaKey{chunkPos, sectionY};
            m_opaqueMegaBuffer.RemoveSection(megaKey);
            m_cutoutMegaBuffer.RemoveSection(megaKey);
            m_translucentMegaBuffer.RemoveSection(megaKey);

            m_gpuData.erase(it);
            LogMeshActivity("Removed section GPU data", chunkPos, sectionY);
        }
    }

    void ClientMeshManager::RemoveChunkGPUData(::Game::Math::ChunkPos chunkPos) {
        std::unique_lock<std::shared_mutex> lock(m_gpuDataMutex);

        // With mega-buffers, RemoveSection is O(1) (free-list update only),
        // so no deferred destroy queue is needed.
        for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
            SectionKey key{chunkPos, sectionY};
            auto it = m_gpuData.find(key);
            if (it != m_gpuData.end()) {
                // Remove from mega-buffers (frees GPU regions, O(1))
                MegaBufferSectionKey megaKey{chunkPos, sectionY};
                m_opaqueMegaBuffer.RemoveSection(megaKey);
                m_cutoutMegaBuffer.RemoveSection(megaKey);
                m_translucentMegaBuffer.RemoveSection(megaKey);

                // Clear references
                auto* sectionInfo = m_chunkManager->GetSectionInfo(chunkPos, sectionY);
                if (sectionInfo) {
                    sectionInfo->gpuData.store(nullptr, std::memory_order_release);
                }

                m_chunkManager->NotifyRenderGridSectionUpdated(chunkPos, sectionY, nullptr);

                m_gpuData.erase(it);
            }
        }

        // Notify renderer that visible sections need rebuilding
        if (Render::g_chunkRenderer) {
            Render::g_chunkRenderer->MarkVisibleSectionsDirty();
        }

        LogMeshActivity("Removed chunk GPU data from mega-buffers", chunkPos);
    }

    void ClientMeshManager::ProcessPendingDestroys() {
        PROFILE_ZONE;
        // With mega-buffers, per-section GPU resource cleanup is handled by
        // RemoveSection (O(1) free-list update). The deferred destroy queue
        // is no longer needed, so just clear any remaining entries.
        m_pendingDestroys.clear();
    }

    void ClientMeshManager::UploadMeshResultToGPU(::Game::Math::ChunkPos chunkPos, int sectionY,
                                                 const Network::MeshBuildResult::SectionMeshData& meshData,
                                                 const VisibilitySet& visSet) {
        PROFILE_ZONE;
        std::unique_lock<std::shared_mutex> lock(m_gpuDataMutex);

        // Validate section index
        if (sectionY < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) {
            Log::Error("Invalid section Y %d for chunk (%d, %d)", sectionY, chunkPos.x, chunkPos.z);
            return;
        }

        SectionKey key{chunkPos, sectionY};
        GPUSectionData& gpuData = m_gpuData[key];

        // Initialize GPU data
        gpuData.chunkPos = chunkPos;
        gpuData.sectionY = sectionY;

        // Remove existing mega-buffer regions for this section (re-upload)
        MegaBufferSectionKey megaKey{chunkPos, sectionY};
        m_opaqueMegaBuffer.RemoveSection(megaKey);
        m_cutoutMegaBuffer.RemoveSection(megaKey);
        m_translucentMegaBuffer.RemoveSection(megaKey);

        // Reset counts and cached draw commands before re-upload
        gpuData.opaqueIndexCount = 0;
        gpuData.opaqueVertexCount = 0;
        gpuData.cutoutIndexCount = 0;
        gpuData.cutoutVertexCount = 0;
        gpuData.translucentIndexCount = 0;
        gpuData.translucentVertexCount = 0;
        gpuData.opaqueDrawCmd = {};
        gpuData.cutoutDrawCmd = {};
        gpuData.translucentDrawCmd = {};
        gpuData.visibilitySet = visSet;

        // Upload each non-empty layer into its mega-buffer and cache draw commands
        if (!meshData.opaqueVertices.empty() && !meshData.opaqueIndices.empty()) {
            m_opaqueMegaBuffer.UploadSection(megaKey,
                meshData.opaqueVertices.data(),
                meshData.opaqueVertexCount,
                meshData.opaqueIndices.data(),
                meshData.opaqueIndexCount);
            gpuData.opaqueVertexCount = static_cast<uint32_t>(meshData.opaqueVertexCount);
            gpuData.opaqueIndexCount = static_cast<uint32_t>(meshData.opaqueIndexCount);
            // Cache draw command to avoid per-frame hash lookup in RenderLayerPass
            ChunkMegaBuffer::DrawCommand cmd;
            if (m_opaqueMegaBuffer.GetDrawCommand(megaKey, cmd)) {
                gpuData.opaqueDrawCmd = {static_cast<int32_t>(cmd.indexCount),
                                         cmd.indexByteOffset, cmd.baseVertex, true, cmd.slabIndex};
            }
        }
        if (!meshData.cutoutVertices.empty() && !meshData.cutoutIndices.empty()) {
            m_cutoutMegaBuffer.UploadSection(megaKey,
                meshData.cutoutVertices.data(),
                meshData.cutoutVertexCount,
                meshData.cutoutIndices.data(),
                meshData.cutoutIndexCount);
            gpuData.cutoutVertexCount = static_cast<uint32_t>(meshData.cutoutVertexCount);
            gpuData.cutoutIndexCount = static_cast<uint32_t>(meshData.cutoutIndexCount);
            ChunkMegaBuffer::DrawCommand cmd;
            if (m_cutoutMegaBuffer.GetDrawCommand(megaKey, cmd)) {
                gpuData.cutoutDrawCmd = {static_cast<int32_t>(cmd.indexCount),
                                         cmd.indexByteOffset, cmd.baseVertex, true, cmd.slabIndex};
            }
        }
        if (!meshData.translucentVertices.empty() && !meshData.translucentIndices.empty()) {
            m_translucentMegaBuffer.UploadSection(megaKey,
                meshData.translucentVertices.data(),
                meshData.translucentVertexCount,
                meshData.translucentIndices.data(),
                meshData.translucentIndexCount);
            gpuData.translucentVertexCount = static_cast<uint32_t>(meshData.translucentVertexCount);
            gpuData.translucentIndexCount = static_cast<uint32_t>(meshData.translucentIndexCount);
            ChunkMegaBuffer::DrawCommand cmd;
            if (m_translucentMegaBuffer.GetDrawCommand(megaKey, cmd)) {
                gpuData.translucentDrawCmd = {static_cast<int32_t>(cmd.indexCount),
                                              cmd.indexByteOffset, cmd.baseVertex, true, cmd.slabIndex};
            }
        }

        // Update metadata
        gpuData.lastUploadFrame = 0; // TODO: Add frame counter
        gpuData.needsUpload = false;

        // Skip empty sections entirely to avoid "zombie" entries in m_gpuData
        // that waste map lookup time during rendering.
        if (!gpuData.HasGeometry()) {
            m_gpuData.erase(key);
            return;
        }

        // Store GPU data pointer in the atomic field for lock-free rendering
        auto* sectionInfo = m_chunkManager->GetSectionInfo(chunkPos, sectionY);
        if (sectionInfo) {
            // Store the pointer to the GPU data in the hash map
            GPUSectionData* gpuDataPtr = &m_gpuData[key];

            // Atomically update the GPU data pointer
            GPUSectionData* oldPtr = sectionInfo->gpuData.exchange(gpuDataPtr, std::memory_order_release);

            // If there was old data, mark it for deferred deletion
            if (oldPtr && oldPtr != gpuDataPtr) {
                // For now, we're reusing the same hash map entry, so this shouldn't happen
                Log::Debug("Replacing existing GPU data for chunk (%d, %d) section %d",
                           chunkPos.x, chunkPos.z, sectionY);
            }

            // IMPORTANT: publish the pointer to the render grid so BuildDrawLists can see it
            m_chunkManager->NotifyRenderGridSectionUpdated(chunkPos, sectionY, gpuDataPtr);
        } else {
            Log::Warning("Could not find SectionInfo for chunk (%d, %d) section %d to store GPU data pointer",
                       chunkPos.x, chunkPos.z, sectionY);
        }

        // Update statistics
        m_stats.meshUploadedToGPU.fetch_add(1, std::memory_order_relaxed);
        m_stats.meshUploadsThisFrame++;

        // Notify renderer that visible sections need rebuilding
        if (Render::g_chunkRenderer) {
            Render::g_chunkRenderer->MarkVisibleSectionsDirty();
        }

        LogMeshActivity("Uploaded mesh result to GPU", chunkPos, sectionY);
    }

    // ========================================================================
    // SHARED BLOCK VAO (GL_ARB_vertex_attrib_binding)
    // ========================================================================

    void ClientMeshManager::CreateSharedBlockVAO() {
        m_hasVertexAttribBinding = (GLAD_GL_ARB_vertex_attrib_binding != 0);

        glGenVertexArrays(1, &m_sharedBlockVAO);
        glBindVertexArray(m_sharedBlockVAO);

        if (m_hasVertexAttribBinding) {
            // Windows/Linux: vertex format decoupled from buffer binding.
            // VBO switching uses glBindVertexBuffer — cheapest possible path.
            // Matches Minecraft's VertexArrayCache.Separate (VertexArrayCache.java:86-152).
            glVertexAttribFormat(0, 3, GL_FLOAT, GL_FALSE, 0);
            glVertexAttribBinding(0, 0);
            glVertexAttribFormat(1, 2, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
            glVertexAttribBinding(1, 0);
            glVertexAttribFormat(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 5 * sizeof(float));
            glVertexAttribBinding(2, 0);
            Log::Info("Shared block VAO created (GL_ARB_vertex_attrib_binding)");
        } else {
            // macOS (OpenGL 4.1, no ARB_vertex_attrib_binding):
            // VBO switching re-sets glVertexAttribPointer — still avoids VAO switch.
            // Matches Minecraft's VertexArrayCache.Emulated (VertexArrayCache.java:25-84).
            Log::Info("Shared block VAO created (emulated, no ARB_vertex_attrib_binding)");
        }

        // Enable attributes once — both paths share this
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);
    }

    void ClientMeshManager::DestroySharedBlockVAO() {
        if (m_sharedBlockVAO) {
            glDeleteVertexArrays(1, &m_sharedBlockVAO);
            m_sharedBlockVAO = 0;
        }
    }

    void ClientMeshManager::BindSharedBlockVAO() {
        glBindVertexArray(m_sharedBlockVAO);
    }

    // ========================================================================
    // MEGA-BUFFER ACCESS
    // ========================================================================

    ChunkMegaBuffer* ClientMeshManager::GetMegaBuffer(RenderLayer layer) {
        switch (layer) {
            case RenderLayer::Opaque:      return &m_opaqueMegaBuffer;
            case RenderLayer::Cutout:      return &m_cutoutMegaBuffer;
            case RenderLayer::Translucent: return &m_translucentMegaBuffer;
            default: return nullptr;
        }
    }

    // ========================================================================
    // GLOBAL FUNCTIONS
    // ========================================================================

    void InitializeClientMeshManager(Client::ClientChunkManager* chunkManager) {
        if (g_clientMeshManager) {
            Log::Warning("ClientMeshManager already initialized");
            return;
        }

        g_clientMeshManager = std::make_unique<ClientMeshManager>();
        g_clientMeshManager->Initialize(chunkManager);
    }

    void ShutdownClientMeshManager() {
        if (g_clientMeshManager) {
            g_clientMeshManager->Shutdown();
            g_clientMeshManager.reset();
        }
    }

    void ProcessClientMeshBuildResults() {
        if (g_clientMeshManager) {
            g_clientMeshManager->ProcessMeshBuildResults();
        }
    }

    void ScheduleClientMeshBuilds(const glm::vec3& playerPosition) {
        if (g_clientMeshManager) {
            g_clientMeshManager->ScheduleMeshBuilds(playerPosition);
        }
    }

    void PerformClientGPUUploads() {
        if (g_clientMeshManager) {
            g_clientMeshManager->PerformGPUUploads();
        }
    }

    void SetClientMeshPlayerPosition(const glm::vec3& position) {
        if (g_clientMeshManager) {
            g_clientMeshManager->SetPlayerPosition(position);
        }
    }


    void CancelClientMeshJobs(::Game::Math::ChunkPos chunkPos) {
        if (g_clientMeshManager) {
            g_clientMeshManager->CancelMeshJobs(chunkPos);
        }
    }

} // namespace Render