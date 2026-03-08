// File: src/client/renderer/mesh/ClientMeshManager.cpp
#include "ClientMeshManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "../../world/ClientWorkerPool.hpp"
#include "../backend/RenderBackend.hpp"
#include <algorithm>
#include <chrono>

namespace Render {

    // Implementation of GPUSectionData::DestroyAllResources (declared in SectionMesh.hpp)
    void GPUSectionData::DestroyAllResources(RenderBackend* backend) {
        if (!backend) return;
        if (opaqueMesh != INVALID_MESH)       { backend->DestroyMesh(opaqueMesh);       opaqueMesh = INVALID_MESH; }
        if (opaqueVB != INVALID_BUFFER)       { backend->DestroyBuffer(opaqueVB);       opaqueVB = INVALID_BUFFER; }
        if (opaqueIB != INVALID_BUFFER)       { backend->DestroyBuffer(opaqueIB);       opaqueIB = INVALID_BUFFER; }
        if (cutoutMesh != INVALID_MESH)       { backend->DestroyMesh(cutoutMesh);       cutoutMesh = INVALID_MESH; }
        if (cutoutVB != INVALID_BUFFER)       { backend->DestroyBuffer(cutoutVB);       cutoutVB = INVALID_BUFFER; }
        if (cutoutIB != INVALID_BUFFER)       { backend->DestroyBuffer(cutoutIB);       cutoutIB = INVALID_BUFFER; }
        if (translucentMesh != INVALID_MESH)  { backend->DestroyMesh(translucentMesh);  translucentMesh = INVALID_MESH; }
        if (translucentVB != INVALID_BUFFER)  { backend->DestroyBuffer(translucentVB);  translucentVB = INVALID_BUFFER; }
        if (translucentIB != INVALID_BUFFER)  { backend->DestroyBuffer(translucentIB);  translucentIB = INVALID_BUFFER; }
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
        
        Log::Info("ClientMeshManager initialized successfully");
    }

    void ClientMeshManager::Shutdown() {
        Log::Info("Shutting down ClientMeshManager...");

        // Clean up all GPU data
        {
            std::unique_lock<std::shared_mutex> lock(m_gpuDataMutex);
            for (auto& [key, gpuData] : m_gpuData) {
                gpuData.DestroyAllResources(g_renderBackend.get());
            }
            m_gpuData.clear();
            m_activeSections.clear();
        }
        
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
        if (!m_chunkManager) return;
        
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
                
                // Upload to GPU
                UploadMeshResultToGPU(result.chunkPos, result.sectionY, result.meshData);
                
                // Tell ClientChunkManager the upload completed successfully
                m_chunkManager->FinalizeSectionUpload(result.chunkPos, result.sectionY, result.neighborMask);
                
                // Update statistics
                m_stats.meshBuildsCompleted.fetch_add(1, std::memory_order_relaxed);
                
                LogMeshActivity("Uploaded mesh", result.chunkPos, result.sectionY);
                break;
            }
            
            case Client::MeshApplyAction::Drop_StaleVersion:
                // Version mismatch - will be rescheduled
                m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                LogMeshActivity("Dropped stale mesh", result.chunkPos, result.sectionY);
                break;
                
            case Client::MeshApplyAction::Drop_Unloaded:
                // Chunk/section is gone
                m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                LogMeshActivity("Dropped mesh (unloaded)", result.chunkPos, result.sectionY);
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
            if (result.meshData.opaqueVertexCount * 12 != result.meshData.opaqueVertices.size()) {
                return false; // 12 floats per vertex
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
            if (result.meshData.cutoutVertexCount * 12 != result.meshData.cutoutVertices.size()) {
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
            if (result.meshData.translucentVertexCount * 12 != result.meshData.translucentVertices.size()) {
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
            // Tell the grid this section is gone BEFORE deleting buffers
            m_chunkManager->NotifyRenderGridSectionUpdated(chunkPos, sectionY, nullptr);

            // Clean up GPU resources
            it->second.DestroyAllResources(g_renderBackend.get());

            m_gpuData.erase(it);
            m_activeSections.erase(key);
            LogMeshActivity("Removed section GPU data", chunkPos, sectionY);
        }
    }

    void ClientMeshManager::RemoveChunkGPUData(::Game::Math::ChunkPos chunkPos) {
        std::unique_lock<std::shared_mutex> lock(m_gpuDataMutex);

        // Remove all sections for this chunk
        for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
            SectionKey key{chunkPos, sectionY};
            auto it = m_gpuData.find(key);
            if (it != m_gpuData.end()) {
                it->second.DestroyAllResources(g_renderBackend.get());

                auto* sectionInfo = m_chunkManager->GetSectionInfo(chunkPos, sectionY);
                if (sectionInfo) {
                    sectionInfo->gpuData.store(nullptr, std::memory_order_release);
                }

                m_chunkManager->NotifyRenderGridSectionUpdated(chunkPos, sectionY, nullptr);

                m_gpuData.erase(it);
                m_activeSections.erase(key);
            }
        }

        LogMeshActivity("Removed chunk GPU data", chunkPos);
    }

    void ClientMeshManager::UploadMeshResultToGPU(::Game::Math::ChunkPos chunkPos, int sectionY, 
                                                 const Network::MeshBuildResult::SectionMeshData& meshData) {
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
        
        // Clean up existing GPU resources if allocated
        if (gpuData.IsUploaded()) {
            gpuData.DestroyAllResources(g_renderBackend.get());
        }

        // Set index/vertex counts
        if (!meshData.opaqueVertices.empty() && !meshData.opaqueIndices.empty()) {
            gpuData.opaqueIndexCount = static_cast<uint32_t>(meshData.opaqueIndices.size());
            gpuData.opaqueVertexCount = static_cast<uint32_t>(meshData.opaqueVertexCount);
        }
        if (!meshData.cutoutVertices.empty() && !meshData.cutoutIndices.empty()) {
            gpuData.cutoutIndexCount = static_cast<uint32_t>(meshData.cutoutIndices.size());
            gpuData.cutoutVertexCount = static_cast<uint32_t>(meshData.cutoutVertexCount);
        }
        if (!meshData.translucentVertices.empty() && !meshData.translucentIndices.empty()) {
            gpuData.translucentIndexCount = static_cast<uint32_t>(meshData.translucentIndices.size());
            gpuData.translucentVertexCount = static_cast<uint32_t>(meshData.translucentVertexCount);
        }

        // Create GPU resources through the render backend
        if (g_renderBackend) {
            auto blockLayout = GetBlockVertexLayout();

            // Opaque backend resources
            if (!meshData.opaqueVertices.empty() && !meshData.opaqueIndices.empty()) {
                gpuData.opaqueVB = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                    meshData.opaqueVertices.size() * sizeof(float), meshData.opaqueVertices.data());
                gpuData.opaqueIB = g_renderBackend->CreateBuffer(BufferUsage::Index,
                    meshData.opaqueIndices.size() * sizeof(uint32_t), meshData.opaqueIndices.data());
                gpuData.opaqueMesh = g_renderBackend->CreateMesh(gpuData.opaqueVB, gpuData.opaqueIB, blockLayout);
            }

            // Cutout backend resources
            if (!meshData.cutoutVertices.empty() && !meshData.cutoutIndices.empty()) {
                gpuData.cutoutVB = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                    meshData.cutoutVertices.size() * sizeof(float), meshData.cutoutVertices.data());
                gpuData.cutoutIB = g_renderBackend->CreateBuffer(BufferUsage::Index,
                    meshData.cutoutIndices.size() * sizeof(uint32_t), meshData.cutoutIndices.data());
                gpuData.cutoutMesh = g_renderBackend->CreateMesh(gpuData.cutoutVB, gpuData.cutoutIB, blockLayout);
            }

            // Translucent backend resources
            if (!meshData.translucentVertices.empty() && !meshData.translucentIndices.empty()) {
                gpuData.translucentVB = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                    meshData.translucentVertices.size() * sizeof(float), meshData.translucentVertices.data());
                gpuData.translucentIB = g_renderBackend->CreateBuffer(BufferUsage::Index,
                    meshData.translucentIndices.size() * sizeof(uint32_t), meshData.translucentIndices.data());
                gpuData.translucentMesh = g_renderBackend->CreateMesh(gpuData.translucentVB, gpuData.translucentIB, blockLayout);
            }
        }

        // Update metadata
        gpuData.lastUploadFrame = 0; // TODO: Add frame counter
        gpuData.needsUpload = false;
        
        // Add to active sections set if it has any geometry
        if (gpuData.HasGeometry()) {
            m_activeSections.insert(key);
        } else {
            // Remove from active sections if no geometry
            m_activeSections.erase(key);
        }
        
        // NEW: Store GPU data pointer in the atomic field for lock-free rendering
        auto* sectionInfo = m_chunkManager->GetSectionInfo(chunkPos, sectionY);
        if (sectionInfo) {
            // Store the pointer to the GPU data in the hash map
            GPUSectionData* gpuDataPtr = &m_gpuData[key];
            
            // Atomically update the GPU data pointer
            GPUSectionData* oldPtr = sectionInfo->gpuData.exchange(gpuDataPtr, std::memory_order_release);
            
            // If there was old data, mark it for deferred deletion
            if (oldPtr && oldPtr != gpuDataPtr) {
                // For now, we're reusing the same hash map entry, so this shouldn't happen
                Log::Warning("Replacing existing GPU data for chunk (%d, %d) section %d", 
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
        
        LogMeshActivity("Uploaded mesh result to GPU", chunkPos, sectionY);
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