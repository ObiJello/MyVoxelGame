// File: src/render/mesh/MeshManager.cpp
#include "MeshManager.hpp"
#include "../../engine/world/World.hpp"
#include "../../core/Log.hpp"
#include <algorithm>
#include <cmath>

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
        m_stats = {};

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

        // Always process completed results first
        ProcessCompletedResults();

        // Process dirty sections
        std::vector<DirtySectionKey> sectionsToProcess;
        {
            std::lock_guard<std::mutex> lock(m_dirtyMutex);
            if (!m_dirtySections.empty()) {
                sectionsToProcess.reserve(m_dirtySections.size());
                for (const auto& key : m_dirtySections) {
                    sectionsToProcess.push_back(key);
                }
                m_dirtySections.clear();
            }
        }

        if (!sectionsToProcess.empty()) {
            Log::Debug("ProcessPendingRemeshes: Processing %zu dirty sections", sectionsToProcess.size());

            // Submit jobs for dirty sections
            for (const auto& key : sectionsToProcess) {
                bool highPriority = IsHighPriority(key.chunkPos, key.sectionY);
                SubmitMeshJob(key.chunkPos, key.sectionY, highPriority);
                m_stats.jobsSubmittedThisFrame++;
            }
        }
    }

    void MeshManager::Update(float deltaTime) {
        // Update statistics
        m_stats.pendingJobs = GetPendingJobCount();
        m_stats.completedResults = GetCompletedResultCount();
        m_stats.activeSections = m_gpuDataManager.GetSectionCount();
        m_stats.totalGPUMemory = m_gpuDataManager.GetTotalMemoryUsage();
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

        // **FIX**: Increase processing limit to handle backlog faster
        int maxProcessThisFrame = std::max(m_config.maxMeshesPerFrame * 3, 1000000); // Process up to 10 per frame

        while (processed < maxProcessThisFrame) {
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
                m_stats.meshesUploadedThisFrame++;
            }

            processed++;

            // **FIX**: Increase time limit for processing
            auto currentTime = std::chrono::steady_clock::now();
            float elapsedMs = std::chrono::duration<float, std::milli>(currentTime - startTime).count();
            if (elapsedMs >= m_config.maxBuildTimeMs * 2.0f) { // Double the time limit
                break;
            }
        }

        if (processed > 0) {
            auto endTime = std::chrono::steady_clock::now();
            m_stats.uploadTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        }
    }

    void MeshManager::UploadMeshResult(const MeshResult& result) {
        bool success = m_gpuDataManager.UpdateSection(result.chunkPos, result.sectionY, result.mesh);

        if (success) {

        } else {
            Log::Warning("Failed to upload mesh for section (%d, %d, %d)",
                        result.chunkPos.x, result.sectionY, result.chunkPos.z);
        }
    }

    void MeshManager::MeshCompileTask(const MeshJob& job) {
        auto startTime = std::chrono::steady_clock::now();

        // Create result
        MeshResult result(job.chunkPos, job.sectionY);

        try {
            // Check if chunk is loaded
            if (!m_world->IsChunkLoaded(job.chunkPos.x, job.chunkPos.z)) {
                result.success = false;
                EnqueueResult(std::move(result));
                return;
            }

            // Get chunk data through proper interface
            const Game::Chunk* chunk = m_world->GetChunkForMeshing(job.chunkPos.x, job.chunkPos.z);
            if (!chunk) {
                result.success = false;
                EnqueueResult(std::move(result));
                return;
            }

            // Check if section has any blocks
            const Game::ChunkSection* section = chunk->GetSection(job.sectionY);
            if (!section) {
                result.success = true; // Empty mesh is still a success
                result.mesh = SectionMesh(job.chunkPos, job.sectionY);
                EnqueueResult(std::move(result));
                return;
            }

            // Initialize result mesh
            result.mesh = SectionMesh(job.chunkPos, job.sectionY);

            // Actually build the mesh using the mesher
            m_mesher.BuildSectionMesh(*chunk, job.sectionY, result.mesh);
            result.success = true;

        } catch (const std::exception& e) {
            Log::Error("Exception in mesh compile task for section (%d, %d, %d): %s",
                      job.chunkPos.x, job.sectionY, job.chunkPos.z, e.what());
            result.success = false;
        }

        auto endTime = std::chrono::steady_clock::now();
        float buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        // Enqueue result
        EnqueueResult(std::move(result));

        // Update stats (thread-safe)
        m_stats.buildTimeMs += buildTimeMs;
        m_stats.meshesBuiltThisFrame++;
    }

    bool MeshManager::IsHighPriority(Game::Math::ChunkPos chunkPos, int sectionY) const {
        float distance = CalculateDistance(chunkPos, sectionY);
        return distance <= m_config.highPriorityRadius;
    }

    float MeshManager::CalculateDistance(Game::Math::ChunkPos chunkPos, int sectionY) const {
        std::lock_guard<std::mutex> lock(m_playerMutex);

        // Calculate distance from player to section center
        float sectionCenterX = chunkPos.x * Game::Math::CHUNK_SIZE_X + Game::Math::CHUNK_SIZE_X * 0.5f;
        float sectionCenterY = sectionY * Game::Math::SECTION_HEIGHT + Game::Math::SECTION_HEIGHT * 0.5f;
        float sectionCenterZ = chunkPos.z * Game::Math::CHUNK_SIZE_Z + Game::Math::CHUNK_SIZE_Z * 0.5f;

        return std::sqrt((sectionCenterX - m_playerPosition.x) * (sectionCenterX - m_playerPosition.x) +
                        (sectionCenterY - m_playerPosition.y) * (sectionCenterY - m_playerPosition.y) +
                        (sectionCenterZ - m_playerPosition.z) * (sectionCenterZ - m_playerPosition.z));
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