// File: src/client/world/ClientWorkerPool.cpp
#include "ClientWorkerPool.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/World.hpp"
#include "../renderer/mesh/Mesher.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include "../renderer/mesh/MeshJobData.hpp"
#include "../renderer/mesh/SnapshotBuilder.hpp"
#include "../renderer/mesh/SnapshotBlockAccess.hpp"
#include <algorithm>
#include <optional>
#include <glm/glm.hpp>

namespace Threading {

    // Global instance
    std::unique_ptr<ClientWorkerPool> g_clientWorkerPool = nullptr;

    ClientWorkerPool::ClientWorkerPool(size_t workerCount)
        : m_workerCount(workerCount) {
        Log::Info("ClientWorkerPool created with %zu workers", workerCount);
    }

    ClientWorkerPool::~ClientWorkerPool() {
        if (m_running.load()) {
            Shutdown();
        }
        Log::Info("ClientWorkerPool destroyed");
    }

    void ClientWorkerPool::Initialize() {
        if (m_running.load()) {
            Log::Warning("ClientWorkerPool already running");
            return;
        }

        m_running.store(true);

        Log::Info("Starting %zu client worker threads...", m_workerCount);

        // Start worker threads
        m_workerThreads.reserve(m_workerCount);
        for (size_t i = 0; i < m_workerCount; ++i) {
            m_workerThreads.emplace_back([this]() { WorkerLoop(); });
        }

        Log::Info("ClientWorkerPool initialized successfully");
    }

    void ClientWorkerPool::Shutdown() {
        if (!m_running.load()) {
            return;
        }

        Log::Info("Shutting down ClientWorkerPool...");

        // Signal all threads to stop
        m_running.store(false);
        m_jobCondition.notify_all();

        // Wait for all threads to finish
        for (auto& thread : m_workerThreads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        m_workerThreads.clear();

        // Clear remaining jobs
        CancelAllJobs();

        // Log final statistics
        LogStats();

        Log::Info("ClientWorkerPool shutdown complete");
    }


    void ClientWorkerPool::SubmitMeshJobWithSnapshot(std::shared_ptr<Client::Render::MeshJobData> snapshot) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit mesh job - ClientWorkerPool not running");
            return;
        }

        if (!snapshot) {
            Log::Warning("Cannot submit mesh job with null snapshot");
            return;
        }

        // Create job with snapshot
        MeshJob job(snapshot);
        EnqueueJob(std::move(job));
        m_stats.meshJobsSubmitted.fetch_add(1, std::memory_order_relaxed);
        
        // Log::Debug("Submitted snapshot mesh job for chunk (%d, %d) section %d, priority=%.1f, highPri=%s",
        //           snapshot->chunkPos.x, snapshot->chunkPos.z, snapshot->sectionY, 
        //           snapshot->distanceToPlayer, snapshot->isHighPriority ? "true" : "false");
    }
    
    void ClientWorkerPool::SubmitMeshJob(Game::Math::ChunkPos chunkPos, int sectionY, 
                                       std::shared_ptr<Game::Chunk> chunkData, float priority) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit mesh job - ClientWorkerPool not running");
            return;
        }

        if (!chunkData) {
            Log::Warning("Cannot submit mesh job with null chunk data");
            return;
        }
        
        // Since we can't use SnapshotBuilder directly (it requires ClientChunk and ClientChunkManager),
        // we'll create a simple snapshot manually for this legacy path
        
        auto* section = chunkData->GetSection(sectionY);
        if (!section) {
            Log::Warning("Cannot submit mesh job - section %d not found in chunk (%d, %d)", 
                        sectionY, chunkPos.x, chunkPos.z);
            return;
        }
        
        // Create snapshot manually
        auto snapshot = std::make_shared<Client::Render::MeshJobData>(chunkPos, sectionY);
        
        // Copy block data
        snapshot->sectionData.isEmpty = true;
        snapshot->sectionData.sectionY = sectionY;
        
        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    int index = y * 256 + z * 16 + x;
                    Game::BlockID block = static_cast<Game::BlockID>(section->blocks[index]);
                    snapshot->sectionData.blocks[index] = block;
                    
                    if (block != Game::BlockID::Air) {
                        snapshot->sectionData.isEmpty = false;
                    }
                }
            }
        }
        
        // Fill light data with default values
        std::fill(snapshot->sectionData.lightData.begin(), snapshot->sectionData.lightData.end(), 0xFF);
        
        // For the legacy path, we'll skip neighbor copying (they'll be empty/air)
        // This is a limitation of the deprecated method
        for (auto& neighbor : snapshot->sectionData.neighbors) {
            std::fill(neighbor.begin(), neighbor.end(), Game::BlockID::Air);
        }
        
        // Calculate priority
        glm::vec3 playerPos = GetPlayerPosition();
        if (m_prioritizationEnabled) {
            priority = CalculatePriority(chunkPos, sectionY, playerPos);
        }
        
        snapshot->distanceToPlayer = priority;
        snapshot->isHighPriority = priority > 500.0f; // Close chunks are high priority
        snapshot->submitTime = std::chrono::steady_clock::now();
        
        // Submit using snapshot
        SubmitMeshJobWithSnapshot(snapshot);
    }

    void ClientWorkerPool::SubmitChunkMeshJobs(Game::Math::ChunkPos chunkPos, 
                                             std::shared_ptr<Game::Chunk> chunkData, 
                                             const glm::vec3& playerPosition) {
        if (!chunkData) {
            Log::Warning("Cannot submit chunk mesh jobs with null chunk data");
            return;
        }

        // Submit mesh jobs for all sections that have data
        for (int sectionIndex = 0; sectionIndex < Game::Math::SECTIONS_PER_CHUNK; ++sectionIndex) {
            const auto* section = chunkData->GetSection(sectionIndex);
            if (section) {
                // Check if section has any non-air blocks (simple check)
                bool hasBlocks = false;
                for (size_t blockIndex = 0; blockIndex < section->blocks.size() && !hasBlocks; ++blockIndex) {
                    if (section->blocks[blockIndex] != static_cast<uint16_t>(Game::BlockID::Air)) {
                        hasBlocks = true;
                    }
                }
                
                if (hasBlocks) {
                    float priority = 0.0f;
                    
                    if (m_prioritizationEnabled) {
                        priority = CalculatePriority(chunkPos, sectionIndex, playerPosition);
                    }
                    
                    SubmitMeshJob(chunkPos, sectionIndex, chunkData, priority);
                }
            }
        }
    }

    void ClientWorkerPool::CancelMeshJob(Game::Math::ChunkPos chunkPos) {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_cancelledChunks.insert(chunkPos);
        Log::Debug("Cancelled all mesh jobs for chunk (%d, %d)", chunkPos.x, chunkPos.z);
    }

    void ClientWorkerPool::CancelMeshJob(Game::Math::ChunkPos chunkPos, int sectionY) {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        SectionKey key{chunkPos, sectionY};
        m_cancelledSections.insert(key);
        Log::Debug("Cancelled mesh job for chunk (%d, %d) section %d", chunkPos.x, chunkPos.z, sectionY);
    }

    void ClientWorkerPool::CancelAllJobs() {
        {
            std::lock_guard<std::mutex> lock(m_jobQueueMutex);
            std::priority_queue<MeshJob> empty;
            std::swap(m_jobQueue, empty);
        }

        {
            std::lock_guard<std::mutex> lock(m_cancelMutex);
            m_cancelledSections.clear();
            m_cancelledChunks.clear();
        }

        Log::Info("Cancelled all pending client worker jobs");
    }

    size_t ClientWorkerPool::GetPendingJobCount() const {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);
        return m_jobQueue.size();
    }

    size_t ClientWorkerPool::GetActiveJobCount() const {
        return m_activeJobs.load(std::memory_order_relaxed);
    }

    void ClientWorkerPool::DrainCompletedResults() {
        // Results are automatically drained by the client render thread
        // through ClientMeshManager::GetMeshResultQueue()
        // This is just for statistics and cleanup
        auto& meshResultQueue = Render::ClientMeshManager::GetMeshResultQueue();
        meshResultQueue.ResetProcessedCount();
    }

    void ClientWorkerPool::SetWorkerCount(size_t count) {
        if (m_running.load()) {
            Log::Warning("Cannot change worker count while ClientWorkerPool is running");
            return;
        }
        m_workerCount = count;
    }

    void ClientWorkerPool::SetPlayerPosition(const glm::vec3& position) {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        m_playerPosition = position;
    }

    glm::vec3 ClientWorkerPool::GetPlayerPosition() const {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        return m_playerPosition;
    }

    void ClientWorkerPool::LogStats() const {
        Log::Info("ClientWorkerPool Statistics:");
        Log::Info("  Mesh Jobs Submitted: %zu", m_stats.meshJobsSubmitted.load());
        Log::Info("  Mesh Jobs Completed: %zu", m_stats.meshJobsCompleted.load());
        Log::Info("  Mesh Jobs Cancelled: %zu", m_stats.meshJobsCancelled.load());
        Log::Info("  Mesh Jobs Failed: %zu", m_stats.meshJobsFailed.load());
        Log::Info("  Sections Built: %zu", m_stats.sectionsBuilt.load());
        Log::Info("  Vertices Generated: %zu", m_stats.verticesGenerated.load());
        Log::Info("  Indices Generated: %zu", m_stats.indicesGenerated.load());
        Log::Info("  Pending Jobs: %zu", GetPendingJobCount());
        Log::Info("  Active Jobs: %zu", GetActiveJobCount());
    }

    // ========================================================================
    // INTERNAL METHODS
    // ========================================================================

    void ClientWorkerPool::WorkerLoop() {
        // Log::Debug("WORKER: Client worker thread started");
        
        static std::atomic<uint64_t> jobCounter{0};

        while (m_running.load()) {
            // Use optional to avoid creating deprecated MeshJob unnecessarily
            std::optional<MeshJob> jobOpt;
            
            {
                std::unique_lock<std::mutex> lock(m_jobQueueMutex);
                
                if (m_jobCondition.wait_for(lock, std::chrono::milliseconds(100), 
                                           [this] { return !m_jobQueue.empty() || !m_running.load(); })) {
                    if (!m_jobQueue.empty()) {
                        jobOpt = std::move(const_cast<MeshJob&>(m_jobQueue.top()));
                        m_jobQueue.pop();
                    }
                }
            }
            
            if (jobOpt.has_value()) {
                uint64_t jobNum = jobCounter.fetch_add(1);
                const auto& job = jobOpt.value();
                // Log::Debug("WORKER: Job %llu dequeued - chunk (%d, %d) section %d", 
                //           jobNum, job.chunkPos.x, job.chunkPos.z, job.sectionY);
                
                m_activeJobs.fetch_add(1, std::memory_order_relaxed);
                ProcessMeshJob(job);
                m_activeJobs.fetch_sub(1, std::memory_order_relaxed);
                
                // Log::Debug("WORKER: Job %llu completed", jobNum);
            }
        }

        Log::Debug("Client worker thread stopped");
    }

    void ClientWorkerPool::ProcessMeshJob(const MeshJob& job) {
        // Check if job should be cancelled
        if (ShouldCancelJob(job)) {
            m_stats.meshJobsCancelled.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        try {
            // Build the section mesh
            Network::MeshBuildResult result = BuildSectionMesh(job);
            
            // Send result to client render thread
            SendMeshResult(std::move(result));
            
            m_stats.meshJobsCompleted.fetch_add(1, std::memory_order_relaxed);
            
            if (result.success) {
                m_stats.sectionsBuilt.fetch_add(1, std::memory_order_relaxed);
                m_stats.verticesGenerated.fetch_add(result.meshData.vertexCount, std::memory_order_relaxed);
                m_stats.indicesGenerated.fetch_add(result.meshData.indexCount, std::memory_order_relaxed);
            }
        }
        catch (const std::exception& e) {
            Log::Error("Client mesh job failed: %s", e.what());
            m_stats.meshJobsFailed.fetch_add(1, std::memory_order_relaxed);
            
            // Send failed result
            Network::MeshBuildResult failedResult(job.chunkPos, job.sectionY);
            failedResult.success = false;
            if (job.snapshot) {
                failedResult.generation = job.snapshot->generation;
            }
            SendMeshResult(std::move(failedResult));
        }
        catch (...) {
            Log::Error("Client mesh job failed with unknown exception");
            m_stats.meshJobsFailed.fetch_add(1, std::memory_order_relaxed);
            
            // Send failed result
            Network::MeshBuildResult failedResult(job.chunkPos, job.sectionY);
            failedResult.success = false;
            if (job.snapshot) {
                failedResult.generation = job.snapshot->generation;
            }
            SendMeshResult(std::move(failedResult));
        }
    }

    bool ClientWorkerPool::ShouldCancelJob(const MeshJob& job) const {
        // Check both section and chunk cancellation
        return IsSectionCancelled(job.chunkPos, job.sectionY) || IsChunkCancelled(job.chunkPos);
    }

    Network::MeshBuildResult ClientWorkerPool::BuildSectionMesh(const MeshJob& job) {
        Network::MeshBuildResult result(job.chunkPos, job.sectionY);
        
        // Check if we have snapshot
        if (!job.snapshot) {
            Log::Warning("BuildSectionMesh: No section snapshot data");
            result.success = false;
            return result;
        }
        
        // Set generation from snapshot for version checking
        result.generation = job.snapshot->generation;
        
        // Fast path for empty sections
        if (job.snapshot->sectionData.isEmpty) {
            result.success = true; // Empty section is valid, just no geometry
            return result;
        }
        
        // Build mesh using the real Mesher with snapshot data
        // Create read-only block access adapter
        Client::Render::SnapshotBlockAccess blockAccess(
            job.snapshot->sectionData,
            job.chunkPos,
            job.sectionY
        );
        
        // Use the real Mesher
        Render::Mesher mesher;
        Render::SectionMesh sectionMesh;
        mesher.BuildSectionMesh(blockAccess, job.chunkPos, job.sectionY, sectionMesh);
        
        // Convert SectionMesh to MeshBuildResult format
        result = ConvertSectionMeshToResult(sectionMesh, job.chunkPos, job.sectionY);
        result.generation = job.snapshot->generation;  // Restore generation after conversion
        result.success = true;
        
        return result;
    }

    void ClientWorkerPool::EnqueueJob(MeshJob&& job) {
        std::unique_lock<std::mutex> lock(m_jobQueueMutex);
        
        // Check queue size limit
        if (m_jobQueue.size() >= m_maxQueueSize) {
            // Don't spam warnings for every dropped job
            static std::chrono::steady_clock::time_point lastWarning;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastWarning).count() >= 1) {
                Log::Warning("Client worker queue full (%zu/%zu), dropping mesh jobs", 
                           m_jobQueue.size(), m_maxQueueSize);
                lastWarning = now;
            }
            m_stats.meshJobsCancelled.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        m_jobQueue.push(std::move(job));
        lock.unlock();
        m_jobCondition.notify_one();
    }

    // DequeueJob method removed - logic is now integrated directly into WorkerLoop

    float ClientWorkerPool::CalculatePriority(Game::Math::ChunkPos chunkPos, int sectionY, const glm::vec3& playerPos) const {
        // Calculate distance from player to section center
        glm::vec3 sectionCenter(
            chunkPos.x * 16.0f + 8.0f,
            sectionY * 16.0f + 8.0f,
            chunkPos.z * 16.0f + 8.0f
        );
        
        float distance = glm::distance(playerPos, sectionCenter);
        
        // Higher priority = smaller distance (closer to player)
        // Use 1000.0f as base priority and subtract distance
        return std::max(0.0f, 1000.0f - distance);
    }

    bool ClientWorkerPool::IsSectionCancelled(Game::Math::ChunkPos chunkPos, int sectionY) const {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        SectionKey key{chunkPos, sectionY};
        return m_cancelledSections.find(key) != m_cancelledSections.end();
    }

    bool ClientWorkerPool::IsChunkCancelled(Game::Math::ChunkPos chunkPos) const {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        return m_cancelledChunks.find(chunkPos) != m_cancelledChunks.end();
    }

    void ClientWorkerPool::CleanupCancelledSections() {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_cancelledSections.clear();
        m_cancelledChunks.clear();
    }

    void ClientWorkerPool::SendMeshResult(Network::MeshBuildResult&& result) {
        // Send to MeshResultQueue for client render thread consumption
        Render::ClientMeshManager::GetMeshResultQueue().Enqueue(std::move(result));
        Render::ClientMeshManager::GetMeshResultQueue().IncrementProcessed();
    }

    Network::MeshBuildResult ClientWorkerPool::ConvertSectionMeshToResult(const Render::SectionMesh& sectionMesh,
                                                                          Game::Math::ChunkPos chunkPos, int sectionY) {
        Network::MeshBuildResult result(chunkPos, sectionY);
        
        // Convert each layer from Vertex format to flat float arrays
        
        // Opaque layer
        if (!sectionMesh.opaqueVerts.empty()) {
            result.meshData.opaqueVertices.reserve(sectionMesh.opaqueVerts.size() * 12); // 12 floats per vertex
            for (const auto& vertex : sectionMesh.opaqueVerts) {
                // Position (3 floats)
                result.meshData.opaqueVertices.push_back(vertex.pos.x);
                result.meshData.opaqueVertices.push_back(vertex.pos.y);
                result.meshData.opaqueVertices.push_back(vertex.pos.z);
                // Normal (3 floats)
                result.meshData.opaqueVertices.push_back(vertex.nrm.x);
                result.meshData.opaqueVertices.push_back(vertex.nrm.y);
                result.meshData.opaqueVertices.push_back(vertex.nrm.z);
                // UV (2 floats)
                result.meshData.opaqueVertices.push_back(vertex.uv.x);
                result.meshData.opaqueVertices.push_back(vertex.uv.y);
                // Color (4 floats)
                result.meshData.opaqueVertices.push_back(vertex.color.r);
                result.meshData.opaqueVertices.push_back(vertex.color.g);
                result.meshData.opaqueVertices.push_back(vertex.color.b);
                result.meshData.opaqueVertices.push_back(vertex.color.a);
            }
            result.meshData.opaqueIndices = sectionMesh.opaqueIdxs;
            result.meshData.opaqueVertexCount = sectionMesh.opaqueVerts.size();
            result.meshData.opaqueIndexCount = sectionMesh.opaqueIdxs.size();
        }
        
        // Cutout layer
        if (!sectionMesh.cutoutVerts.empty()) {
            result.meshData.cutoutVertices.reserve(sectionMesh.cutoutVerts.size() * 12);
            for (const auto& vertex : sectionMesh.cutoutVerts) {
                // Position (3 floats)
                result.meshData.cutoutVertices.push_back(vertex.pos.x);
                result.meshData.cutoutVertices.push_back(vertex.pos.y);
                result.meshData.cutoutVertices.push_back(vertex.pos.z);
                // Normal (3 floats)
                result.meshData.cutoutVertices.push_back(vertex.nrm.x);
                result.meshData.cutoutVertices.push_back(vertex.nrm.y);
                result.meshData.cutoutVertices.push_back(vertex.nrm.z);
                // UV (2 floats)
                result.meshData.cutoutVertices.push_back(vertex.uv.x);
                result.meshData.cutoutVertices.push_back(vertex.uv.y);
                // Color (4 floats)
                result.meshData.cutoutVertices.push_back(vertex.color.r);
                result.meshData.cutoutVertices.push_back(vertex.color.g);
                result.meshData.cutoutVertices.push_back(vertex.color.b);
                result.meshData.cutoutVertices.push_back(vertex.color.a);
            }
            result.meshData.cutoutIndices = sectionMesh.cutoutIdxs;
            result.meshData.cutoutVertexCount = sectionMesh.cutoutVerts.size();
            result.meshData.cutoutIndexCount = sectionMesh.cutoutIdxs.size();
        }
        
        // Translucent layer
        if (!sectionMesh.translucentVerts.empty()) {
            result.meshData.translucentVertices.reserve(sectionMesh.translucentVerts.size() * 12);
            for (const auto& vertex : sectionMesh.translucentVerts) {
                // Position (3 floats)
                result.meshData.translucentVertices.push_back(vertex.pos.x);
                result.meshData.translucentVertices.push_back(vertex.pos.y);
                result.meshData.translucentVertices.push_back(vertex.pos.z);
                // Normal (3 floats)
                result.meshData.translucentVertices.push_back(vertex.nrm.x);
                result.meshData.translucentVertices.push_back(vertex.nrm.y);
                result.meshData.translucentVertices.push_back(vertex.nrm.z);
                // UV (2 floats)
                result.meshData.translucentVertices.push_back(vertex.uv.x);
                result.meshData.translucentVertices.push_back(vertex.uv.y);
                // Color (4 floats)
                result.meshData.translucentVertices.push_back(vertex.color.r);
                result.meshData.translucentVertices.push_back(vertex.color.g);
                result.meshData.translucentVertices.push_back(vertex.color.b);
                result.meshData.translucentVertices.push_back(vertex.color.a);
            }
            result.meshData.translucentIndices = sectionMesh.translucentIdxs;
            result.meshData.translucentVertexCount = sectionMesh.translucentVerts.size();
            result.meshData.translucentIndexCount = sectionMesh.translucentIdxs.size();
        }
        
        // Legacy compatibility - use opaque data as default
        if (!result.meshData.opaqueVertices.empty()) {
            result.meshData.vertices = result.meshData.opaqueVertices;
            result.meshData.indices = result.meshData.opaqueIndices;
            result.meshData.vertexCount = result.meshData.opaqueVertexCount;
            result.meshData.indexCount = result.meshData.opaqueIndexCount;
        } else if (!result.meshData.cutoutVertices.empty()) {
            result.meshData.vertices = result.meshData.cutoutVertices;
            result.meshData.indices = result.meshData.cutoutIndices;
            result.meshData.vertexCount = result.meshData.cutoutVertexCount;
            result.meshData.indexCount = result.meshData.cutoutIndexCount;
        } else if (!result.meshData.translucentVertices.empty()) {
            result.meshData.vertices = result.meshData.translucentVertices;
            result.meshData.indices = result.meshData.translucentIndices;
            result.meshData.vertexCount = result.meshData.translucentVertexCount;
            result.meshData.indexCount = result.meshData.translucentIndexCount;
        }
        
        return result;
    }

    // ========================================================================
    // GLOBAL FUNCTIONS
    // ========================================================================

    void InitializeClientWorkerPool(size_t workerCount) {
        if (g_clientWorkerPool) {
            Log::Warning("ClientWorkerPool already initialized");
            return;
        }

        g_clientWorkerPool = std::make_unique<ClientWorkerPool>(workerCount);
        g_clientWorkerPool->Initialize();
    }

    void ShutdownClientWorkerPool() {
        if (g_clientWorkerPool) {
            g_clientWorkerPool->Shutdown();
            g_clientWorkerPool.reset();
        }
    }

    void SubmitClientMeshJob(Game::Math::ChunkPos chunkPos, int sectionY, 
                           std::shared_ptr<Game::Chunk> chunkData, float priority) {
        if (g_clientWorkerPool) {
            g_clientWorkerPool->SubmitMeshJob(chunkPos, sectionY, chunkData, priority);
        }
    }

    void SubmitClientChunkMeshJobs(Game::Math::ChunkPos chunkPos, 
                                 std::shared_ptr<Game::Chunk> chunkData, 
                                 const glm::vec3& playerPosition) {
        if (g_clientWorkerPool) {
            g_clientWorkerPool->SubmitChunkMeshJobs(chunkPos, chunkData, playerPosition);
        }
    }

    void SetClientWorkerPlayerPosition(const glm::vec3& position) {
        if (g_clientWorkerPool) {
            g_clientWorkerPool->SetPlayerPosition(position);
        }
    }

    void CancelClientMeshJob(Game::Math::ChunkPos chunkPos) {
        if (g_clientWorkerPool) {
            g_clientWorkerPool->CancelMeshJob(chunkPos);
        }
    }

    void CancelClientMeshJob(Game::Math::ChunkPos chunkPos, int sectionY) {
        if (g_clientWorkerPool) {
            g_clientWorkerPool->CancelMeshJob(chunkPos, sectionY);
        }
    }

    void SetClientWorkerWorld(Game::World* world) {
        if (g_clientWorkerPool) {
            g_clientWorkerPool->SetWorld(world);
        }
    }

} // namespace Threading