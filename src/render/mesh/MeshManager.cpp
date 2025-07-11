// File: src/render/mesh/MeshManager.cpp (FIXED)
#include "MeshManager.hpp"
#include "../../engine/world/World.hpp"
#include "../../core/Log.hpp"
#include <algorithm>
#include <cmath>
#include <atomic>

namespace Render {

    // Global mesh manager instance
    std::unique_ptr<MeshManager> g_meshManager = nullptr;

    MeshManager::MeshManager(const MeshManagerConfig& config)
        : m_config(config), m_mesher(MeshConfig{}) {
        Log::Info("MeshManager created with config: maxMeshesPerFrame=%d, maxBuildTimeMs=%.1f",
                  config.maxMeshesPerFrame, config.maxBuildTimeMs);
    }

    MeshManager::~MeshManager() {
        Shutdown();
    }

    void MeshManager::Initialize(Game::World* world) {
        m_world = world;

        // **FIX**: Use atomic initialization for stats
        m_stats.store({});

        // Clear any existing data
        {
            std::lock_guard<std::mutex> lock1(m_jobQueueMutex);
            std::lock_guard<std::mutex> lock2(m_resultQueueMutex);
            std::lock_guard<std::mutex> lock3(m_dirtyMutex);

            m_pendingJobs = {};
            m_highPriorityJobs = {};
            m_completedResults = {};
            m_dirtySections.clear();
        }

        m_gpuDataManager.Clear();

        Log::Info("MeshManager initialized with world reference");
    }

    void MeshManager::Shutdown() {
        if (m_world) {
            Log::Info("Shutting down MeshManager...");

            // Clear all queues and data
            {
                std::lock_guard<std::mutex> lock1(m_jobQueueMutex);
                std::lock_guard<std::mutex> lock2(m_resultQueueMutex);
                std::lock_guard<std::mutex> lock3(m_dirtyMutex);

                m_pendingJobs = {};
                m_highPriorityJobs = {};
                m_completedResults = {};
                m_dirtySections.clear();
            }

            m_gpuDataManager.Clear();
            m_world = nullptr;

            Log::Info("MeshManager shutdown complete");
        }
    }

    void MeshManager::MarkSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY) {
        if (sectionY < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) {
            return;
        }

        DirtySectionKey key{chunkPos, sectionY};

        {
            std::lock_guard<std::mutex> lock(m_dirtyMutex);
            m_dirtySections.insert(key);
        }

        Log::Debug("Marked section (%d, %d, %d) dirty", chunkPos.x, sectionY, chunkPos.z);
    }

    void MeshManager::MarkChunkDirty(Game::Math::ChunkPos chunkPos) {
        // Mark all sections in the chunk dirty
        for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
            MarkSectionDirty(chunkPos, sectionY);
        }
    }

    void MeshManager::ProcessPendingRemeshes() {
        if (!m_world) return;

        m_frameStartTime = std::chrono::steady_clock::now();

        // **FIX**: Better dirty section processing
        std::vector<DirtySectionKey> sectionsToProcess;
        {
            std::lock_guard<std::mutex> lock(m_dirtyMutex);
            if (m_dirtySections.empty()) {
                return; // Early exit if no work
            }

            sectionsToProcess.reserve(m_dirtySections.size());
            for (const auto& key : m_dirtySections) {
                sectionsToProcess.push_back(key);
            }
            m_dirtySections.clear();
        }

        Log::Debug("ProcessPendingRemeshes: Processing %zu dirty sections", sectionsToProcess.size());

        // **FIX**: Filter sections to only include loaded chunks
        std::vector<DirtySectionKey> validSections;
        validSections.reserve(sectionsToProcess.size());

        for (const auto& key : sectionsToProcess) {
            if (m_world->IsChunkLoaded(key.chunkPos.x, key.chunkPos.z)) {
                validSections.push_back(key);
            } else {
                // **FIX**: Re-queue sections for chunks that aren't loaded yet
                Log::Debug("Chunk (%d, %d) not loaded yet, re-queuing section %d",
                          key.chunkPos.x, key.chunkPos.z, key.sectionY);

                // Re-add to dirty queue to try again later
                {
                    std::lock_guard<std::mutex> lock(m_dirtyMutex);
                    m_dirtySections.insert(key);
                }
            }
        }

        // Submit jobs for valid sections only
        for (const auto& key : validSections) {
            bool highPriority = IsHighPriority(key.chunkPos, key.sectionY);
            SubmitMeshJob(key.chunkPos, key.sectionY, highPriority);

            // **FIX**: Thread-safe stats update
            m_stats.fetch_add_jobsSubmittedThisFrame(1);
        }

        // Process completed mesh results
        ProcessCompletedResults();
    }

    void MeshManager::Update(float deltaTime) {
        // **FIX**: Thread-safe stats access using setter methods
        m_stats.setPendingJobs(GetPendingJobCount());
        m_stats.setCompletedResults(GetCompletedResultCount());
        m_stats.setActiveSections(m_gpuDataManager.GetSectionCount());
        m_stats.setTotalGPUMemory(m_gpuDataManager.GetTotalMemoryUsage());

        // Cleanup old results periodically
        static float cleanupTimer = 0.0f;
        cleanupTimer += deltaTime;
        if (cleanupTimer >= 1.0f) {
            CleanupOldResults();
            cleanupTimer = 0.0f;
        }

        // Enforce job limits
        EnforceJobLimits();
    }

    void MeshManager::SetPlayerPosition(const glm::vec3& position) {
        std::lock_guard<std::mutex> lock(m_playerMutex);
        m_playerPosition = position;
    }

    const GPUSectionData* MeshManager::GetSectionGPUData(Game::Math::ChunkPos chunkPos, int sectionY) const {
        return m_gpuDataManager.GetSectionData(chunkPos, sectionY);
    }

    void MeshManager::ClearAllMeshes() {
        {
            std::lock_guard<std::mutex> lock1(m_jobQueueMutex);
            std::lock_guard<std::mutex> lock2(m_resultQueueMutex);
            std::lock_guard<std::mutex> lock3(m_dirtyMutex);

            m_pendingJobs = {};
            m_highPriorityJobs = {};
            m_completedResults = {};
            m_dirtySections.clear();
        }

        m_gpuDataManager.Clear();
        Log::Info("Cleared all meshes and queues");
    }

    void MeshManager::ForceRemeshRadius(const glm::vec3& center, float radius) {
        if (!m_world) return;

        // Calculate chunk range
        int centerChunkX = static_cast<int>(std::floor(center.x / Game::Math::CHUNK_SIZE_X));
        int centerChunkZ = static_cast<int>(std::floor(center.z / Game::Math::CHUNK_SIZE_Z));
        int chunkRadius = static_cast<int>(std::ceil(radius / Game::Math::CHUNK_SIZE_X));

        int marked = 0;
        for (int dz = -chunkRadius; dz <= chunkRadius; ++dz) {
            for (int dx = -chunkRadius; dx <= chunkRadius; ++dx) {
                Game::Math::ChunkPos chunkPos{centerChunkX + dx, centerChunkZ + dz};

                // Check if chunk is within radius
                float chunkCenterX = chunkPos.x * Game::Math::CHUNK_SIZE_X + Game::Math::CHUNK_SIZE_X * 0.5f;
                float chunkCenterZ = chunkPos.z * Game::Math::CHUNK_SIZE_Z + Game::Math::CHUNK_SIZE_Z * 0.5f;
                float distance = std::sqrt((chunkCenterX - center.x) * (chunkCenterX - center.x) +
                                         (chunkCenterZ - center.z) * (chunkCenterZ - center.z));

                if (distance <= radius) {
                    MarkChunkDirty(chunkPos);
                    marked++;
                }
            }
        }

        Log::Info("Force remeshed %d chunks within radius %.1f of (%.1f, %.1f, %.1f)",
                  marked, radius, center.x, center.y, center.z);
    }

    size_t MeshManager::GetPendingJobCount() const {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);
        return m_pendingJobs.size() + m_highPriorityJobs.size();
    }

    size_t MeshManager::GetCompletedResultCount() const {
        std::lock_guard<std::mutex> lock(m_resultQueueMutex);
        return m_completedResults.size();
    }

    // Private methods
    void MeshManager::SubmitMeshJob(Game::Math::ChunkPos chunkPos, int sectionY, bool highPriority) {
        if (!m_config.enableAsyncBuilding) {
            // Process immediately on main thread
            MeshJob job(chunkPos, sectionY, highPriority);
            MeshCompileTask(job);
            return;
        }

        // Create job and enqueue
        MeshJob job(chunkPos, sectionY, highPriority);
        EnqueueJob(job);

        // Submit to thread pool
        JobSystem::g_ThreadPool.Enqueue([this, job]() {
            this->MeshCompileTask(job);
        });
    }

    void MeshManager::ProcessCompletedResults() {
        auto startTime = std::chrono::steady_clock::now();
        int processed = 0;

        while (processed < m_config.maxMeshesPerFrame) {
            // Try to get a result from the queue
            std::optional<MeshResult> result;

            {
                std::lock_guard<std::mutex> lock(m_resultQueueMutex);

                if (m_completedResults.empty()) {
                    break; // No more results
                }

                // Move the result out of the queue
                result = std::move(m_completedResults.front());
                m_completedResults.pop();
            }

            // Process the result outside the lock
            if (result && result->success) {
                UploadMeshResult(*result);

                // **FIX**: Thread-safe stats update
                m_stats.fetch_add_meshesUploadedThisFrame(1);
            }

            processed++;

            // Check time limit
            auto currentTime = std::chrono::steady_clock::now();
            float elapsedMs = std::chrono::duration<float, std::milli>(currentTime - startTime).count();
            if (elapsedMs >= m_config.maxBuildTimeMs) {
                break;
            }
        }

        if (processed > 0) {
            auto endTime = std::chrono::steady_clock::now();
            float uploadTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
            m_stats.fetch_set_uploadTimeMs(uploadTime);
        }
    }

    void MeshManager::UploadMeshResult(const MeshResult& result) {
        auto startTime = std::chrono::steady_clock::now();

        bool success = m_gpuDataManager.UpdateSection(result.chunkPos, result.sectionY, result.mesh);

        auto endTime = std::chrono::steady_clock::now();
        float uploadTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        if (success) {
            Log::Debug("Uploaded mesh for section (%d, %d, %d) in %.2f ms",
                      result.chunkPos.x, result.sectionY, result.chunkPos.z, uploadTimeMs);
        } else {
            Log::Warning("Failed to upload mesh for section (%d, %d, %d)",
                        result.chunkPos.x, result.sectionY, result.chunkPos.z);
        }
    }

    void MeshManager::MeshCompileTask(const MeshJob& job) {
        auto startTime = std::chrono::steady_clock::now();

        // Create result
        MeshResult result(job.chunkPos, job.sectionY);

        // **DEBUG**: Log when we start building a mesh
        Log::Debug("Starting mesh build for section (%d, %d, %d)",
                  job.chunkPos.x, job.sectionY, job.chunkPos.z);

        try {
            // **FIX**: Better chunk loading check with retry logic
            if (!m_world->IsChunkLoaded(job.chunkPos.x, job.chunkPos.z)) {
                Log::Debug("Chunk (%d, %d) not loaded, job will be retried later",
                          job.chunkPos.x, job.chunkPos.z);

                // **FIX**: Don't mark as failed - re-queue the section as dirty
                {
                    std::lock_guard<std::mutex> lock(m_dirtyMutex);
                    m_dirtySections.insert({job.chunkPos, job.sectionY});
                }

                result.success = false;
                EnqueueResult(std::move(result));
                return;
            }

            // **FIXED**: Get chunk data through proper interface with additional safety
            const Game::Chunk* chunk = m_world->GetChunkForMeshing(job.chunkPos.x, job.chunkPos.z);
            if (!chunk) {
                Log::Debug("Could not get chunk data for meshing (%d, %d), will retry",
                          job.chunkPos.x, job.chunkPos.z);

                // **FIX**: Re-queue instead of failing
                {
                    std::lock_guard<std::mutex> lock(m_dirtyMutex);
                    m_dirtySections.insert({job.chunkPos, job.sectionY});
                }

                result.success = false;
                EnqueueResult(std::move(result));
                return;
            }

            // **DEBUG**: Check if section has any blocks
            const Game::ChunkSection* section = chunk->GetSection(job.sectionY);
            if (!section) {
                Log::Debug("Section %d in chunk (%d, %d) is null, creating empty mesh",
                          job.sectionY, job.chunkPos.x, job.chunkPos.z);
                result.success = true; // Empty mesh is still a success
                result.mesh = SectionMesh(job.chunkPos, job.sectionY);
                EnqueueResult(std::move(result));
                return;
            }

            // **DEBUG**: Count non-air blocks in section
            int nonAirBlocks = 0;
            for (int x = 0; x < 16; ++x) {
                for (int y = 0; y < 16; ++y) {
                    for (int z = 0; z < 16; ++z) {
                        if (section->GetBlockID(x, y, z) != Game::BlockID::Air) {
                            nonAirBlocks++;
                        }
                    }
                }
            }

            Log::Debug("Section (%d, %d, %d) has %d non-air blocks",
                      job.chunkPos.x, job.sectionY, job.chunkPos.z, nonAirBlocks);

            // Initialize result mesh
            result.mesh = SectionMesh(job.chunkPos, job.sectionY);

            // **CRITICAL**: Actually build the mesh using your mesher
            m_mesher.BuildSectionMesh(*chunk, job.sectionY, result.mesh);
            result.success = true;

            // **DEBUG**: Log mesh stats for debugging
            size_t totalVerts = result.mesh.GetTotalVertexCount();
            size_t totalIndices = result.mesh.GetTotalIndexCount();

            if (totalVerts > 0) {
                Log::Info("Built mesh for section (%d, %d, %d): %zu vertices, %zu indices (%d blocks)",
                         job.chunkPos.x, job.sectionY, job.chunkPos.z, totalVerts, totalIndices, nonAirBlocks);
            } else if (nonAirBlocks > 0) {
                Log::Warning("Section (%d, %d, %d) has %d blocks but produced 0 vertices!",
                            job.chunkPos.x, job.sectionY, job.chunkPos.z, nonAirBlocks);
            } else {
                Log::Debug("Section (%d, %d, %d) is empty, no mesh generated",
                          job.chunkPos.x, job.sectionY, job.chunkPos.z);
            }

        } catch (const std::exception& e) {
            Log::Error("Exception in mesh compile task for section (%d, %d, %d): %s",
                      job.chunkPos.x, job.sectionY, job.chunkPos.z, e.what());
            result.success = false;
        }

        auto endTime = std::chrono::steady_clock::now();
        float buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        // Enqueue result
        EnqueueResult(std::move(result));

        // **FIX**: Thread-safe stats update
        m_stats.fetch_add_buildTimeMs(buildTimeMs);
        m_stats.fetch_add_meshesBuiltThisFrame(1);

        Log::Debug("Completed mesh build for section (%d, %d, %d) in %.2f ms",
                  job.chunkPos.x, job.sectionY, job.chunkPos.z, buildTimeMs);
    }

    bool MeshManager::IsHighPriority(Game::Math::ChunkPos chunkPos, int sectionY) const {
        float distance = CalculateDistance(chunkPos, sectionY);
        return distance <= m_config.highPriorityRadius;
    }

    float MeshManager::CalculateDistance(Game::Math::ChunkPos chunkPos, int sectionY) const {
        // **FIX**: Better thread safety for player position
        glm::vec3 playerPos;
        {
            std::lock_guard<std::mutex> lock(m_playerMutex);
            playerPos = m_playerPosition;
        }

        // Calculate distance from player to section center
        float sectionCenterX = chunkPos.x * Game::Math::CHUNK_SIZE_X + Game::Math::CHUNK_SIZE_X * 0.5f;
        float sectionCenterY = sectionY * Game::Math::SECTION_HEIGHT + Game::Math::SECTION_HEIGHT * 0.5f;
        float sectionCenterZ = chunkPos.z * Game::Math::CHUNK_SIZE_Z + Game::Math::CHUNK_SIZE_Z * 0.5f;

        return std::sqrt((sectionCenterX - playerPos.x) * (sectionCenterX - playerPos.x) +
                        (sectionCenterY - playerPos.y) * (sectionCenterY - playerPos.y) +
                        (sectionCenterZ - playerPos.z) * (sectionCenterZ - playerPos.z));
    }

    void MeshManager::EnqueueJob(const MeshJob& job) {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);

        if (job.isHighPriority) {
            m_highPriorityJobs.push(job);
        } else {
            m_pendingJobs.push(job);
        }
    }

    bool MeshManager::DequeueJob(MeshJob& job) {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);

        // Check high priority queue first
        if (!m_highPriorityJobs.empty()) {
            job = m_highPriorityJobs.front();
            m_highPriorityJobs.pop();
            return true;
        }

        // Then check normal priority queue
        if (!m_pendingJobs.empty()) {
            job = m_pendingJobs.front();
            m_pendingJobs.pop();
            return true;
        }

        return false;
    }

    void MeshManager::EnqueueResult(MeshResult&& result) {
        std::lock_guard<std::mutex> lock(m_resultQueueMutex);
        m_completedResults.push(std::move(result));
    }

    bool MeshManager::DequeueResult(MeshResult& result) {
        std::lock_guard<std::mutex> lock(m_resultQueueMutex);

        if (m_completedResults.empty()) {
            return false;
        }

        result = std::move(m_completedResults.front());
        m_completedResults.pop();
        return true;
    }

    void MeshManager::EnforceJobLimits() {
        std::lock_guard<std::mutex> lock(m_jobQueueMutex);

        // **FIX**: More robust limit enforcement
        size_t totalJobs = m_pendingJobs.size() + m_highPriorityJobs.size();

        while (totalJobs > m_config.maxPendingJobs) {
            if (!m_pendingJobs.empty()) {
                m_pendingJobs.pop();
                totalJobs--;
            } else if (!m_highPriorityJobs.empty()) {
                m_highPriorityJobs.pop();
                totalJobs--;
            } else {
                break;
            }
        }

        // Limit high priority jobs
        while (m_highPriorityJobs.size() > m_config.maxHighPriorityJobs) {
            m_highPriorityJobs.pop();
        }
    }

    void MeshManager::CleanupOldResults() {
        std::lock_guard<std::mutex> lock(m_resultQueueMutex);

        // Remove results older than 5 seconds to prevent memory buildup
        auto cutoffTime = std::chrono::steady_clock::now() - std::chrono::seconds(5);

        std::queue<MeshResult> filteredResults;
        while (!m_completedResults.empty()) {
            auto& result = m_completedResults.front();
            if (result.completeTime >= cutoffTime) {
                filteredResults.push(std::move(result));
            }
            m_completedResults.pop();
        }

        m_completedResults = std::move(filteredResults);
    }

    // Global utility functions
    void InitializeMeshSystem(Game::World* world, const MeshManagerConfig& config) {
        if (g_meshManager) {
            Log::Warning("Mesh system already initialized, shutting down first");
            ShutdownMeshSystem();
        }

        g_meshManager = std::make_unique<MeshManager>(config);
        g_meshManager->Initialize(world);

        Log::Info("Mesh system initialized");
    }

    void ShutdownMeshSystem() {
        if (g_meshManager) {
            g_meshManager->Shutdown();
            g_meshManager.reset();
            Log::Info("Mesh system shutdown");
        }
    }

    void UpdateMeshSystem(float deltaTime) {
        if (g_meshManager) {
            g_meshManager->ProcessPendingRemeshes();
            g_meshManager->Update(deltaTime);
        }
    }

    void MarkWorldSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY) {
        if (g_meshManager) {
            g_meshManager->MarkSectionDirty(chunkPos, sectionY);
        }
    }

    void MarkWorldChunkDirty(Game::Math::ChunkPos chunkPos) {
        if (g_meshManager) {
            g_meshManager->MarkChunkDirty(chunkPos);
        }
    }

    void SetMeshSystemPlayerPosition(const glm::vec3& position) {
        if (g_meshManager) {
            g_meshManager->SetPlayerPosition(position);
        }
    }

} // namespace Render