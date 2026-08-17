// File: src/client/world/ClientWorkerPool.cpp
#include "ClientWorkerPool.hpp"
#include "common/core/Log.hpp"
#include "common/core/ThreadPriority.hpp"
#include "common/core/Config.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/World.hpp"
#include "../renderer/mesh/Mesher.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include "../renderer/mesh/MeshUploadPermits.hpp"
#include "../renderer/mesh/MeshJobData.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>
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


    bool ClientWorkerPool::SubmitMeshJobWithSnapshot(std::shared_ptr<Client::Render::MeshJobData> snapshot) {
        if (!m_running.load()) {
            Log::Warning("Cannot submit mesh job - ClientWorkerPool not running");
            return false;
        }

        if (!snapshot) {
            Log::Warning("Cannot submit mesh job with null snapshot");
            return false;
        }

        // Create job with snapshot
        MeshJob job(snapshot);
        if (EnqueueJob(std::move(job))) {
            m_stats.meshJobsSubmitted.fetch_add(1, std::memory_order_relaxed);
            // Log::Debug("Submitted snapshot mesh job for chunk (%d, %d) section %d, priority=%.1f, highPri=%s",
            //           snapshot->chunkPos.x, snapshot->chunkPos.z, snapshot->sectionY, 
            //           snapshot->distanceToPlayer, snapshot->isHighPriority ? "true" : "false");
            return true;
        }
        return false;
    }

    void ClientWorkerPool::BuildMeshJobSync(std::shared_ptr<Client::Render::MeshJobData> snapshot) {
        if (!snapshot) return;
        PROFILE_ZONE_N("MeshSyncCompile");

        MeshJob job(std::move(snapshot));
        m_stats.meshJobsSubmitted.fetch_add(1, std::memory_order_relaxed);

        try {
            Network::MeshBuildResult result = BuildSectionMesh(job);
            // No permit was acquired for this section — it never entered the
            // compile queue. See MeshBuildResult::holdsUploadPermit.
            result.holdsUploadPermit = false;

            const bool succeeded = result.success;
            const size_t verts = result.meshData.GetTotalVertexCount();
            const size_t indices = result.meshData.GetTotalIndexCount();

            SendMeshResult(std::move(result));

            m_stats.meshJobsCompleted.fetch_add(1, std::memory_order_relaxed);
            if (succeeded) {
                m_stats.sectionsBuilt.fetch_add(1, std::memory_order_relaxed);
                m_stats.verticesGenerated.fetch_add(verts, std::memory_order_relaxed);
                m_stats.indicesGenerated.fetch_add(indices, std::memory_order_relaxed);
            }
        }
        catch (const std::exception& e) {
            Log::Error("Synchronous mesh build failed: %s", e.what());
            m_stats.meshJobsFailed.fetch_add(1, std::memory_order_relaxed);
            // Still send a failed result, or the section stays stuck in MESHING.
            Network::MeshBuildResult failedResult(job.chunkPos, job.sectionY);
            failedResult.success = false;
            failedResult.holdsUploadPermit = false;
            if (job.snapshot) failedResult.generation = job.snapshot->generation;
            SendMeshResult(std::move(failedResult));
        }
    }

    void ClientWorkerPool::CancelMeshJob(Game::Math::ChunkPos chunkPos) {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_cancelledChunks.insert(chunkPos);
        Log::Debug("Cancelled all mesh jobs for chunk (%d, %d)", chunkPos.x, chunkPos.z);
    }

    void ClientWorkerPool::UncancelChunk(Game::Math::ChunkPos chunkPos) {
        std::lock_guard<std::mutex> lock(m_cancelMutex);
        m_cancelledChunks.erase(chunkPos);
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
            m_jobQueue.clear();
            m_recompileQuota = kMaxRecompileQuota;
        }

        // NO permit releases here. A queued job holds no permit — WorkerLoop
        // acquires one only when it actually starts the job, so discarding the
        // queue cannot leak a slot. (Before the queue was decoupled from the
        // pool this loop released one permit per discarded job; doing that now
        // would over-release and inflate the pool past its capacity.) Jobs
        // already in flight still hold theirs and still funnel through
        // SendMeshResult, which is unchanged.

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
        PROFILE_THREAD("MeshWorker");
        // Feeds the frame: a section that finishes late is a hole in the world,
        // so these outrank terrain generation but still yield to the main thread.
        Core::SetCurrentThreadPriority(Core::ThreadPriorityClass::Elevated);
        // Log::Debug("WORKER: Client worker thread started");

        static std::atomic<uint64_t> jobCounter{0};

        auto& permits = ::Render::GetMeshUploadPermits();

        while (m_running.load()) {
            // Use optional to avoid creating deprecated MeshJob unnecessarily
            std::optional<MeshJob> jobOpt;

            // 1. Wait for work WITHOUT holding a permit. Blocking here with a
            //    permit in hand would idle a pipeline slot for up to 5 ms.
            {
                std::unique_lock<std::mutex> lock(m_jobQueueMutex);
                m_jobCondition.wait_for(lock, std::chrono::milliseconds(5),
                                        [this] { return !m_jobQueue.empty() || !m_running.load(); });
                if (!m_running.load()) break;
                if (m_jobQueue.empty()) continue;
            }

            // 2. Claim a pipeline slot. This is MC's runTask() gate —
            //    `if (!this.bufferPool.isEmpty())` before polling the queue.
            //    The permit spans compile AND upload and comes back on the
            //    render thread once the section has been uploaded, so a slow
            //    upload stage throttles compilation automatically.
            //
            //    THIS is the only place meshing is rate-limited. It used to be
            //    taken by the scheduler before a job was even queued, which made
            //    throughput `permits x scheduler passes/s` — i.e. proportional
            //    to frame rate, because the scheduler runs once per frame. That
            //    is what made a slow backend render a visibly smaller disc of
            //    world than a fast one.
            if (!permits.TryAcquire()) {
                // Pipeline full. The work stays queued and stays sorted by
                // distance, so nothing is lost or reordered by backing off.
                std::this_thread::sleep_for(std::chrono::microseconds(250));
                continue;
            }

            // 3. Take the nearest job to where the camera is RIGHT NOW.
            const glm::vec3 cameraPos = GetPlayerPosition();
            {
                std::lock_guard<std::mutex> lock(m_jobQueueMutex);
                jobOpt = PollNearestLocked(cameraPos);
            }

            if (!jobOpt.has_value()) {
                // Raced another worker for the last entry, or everything left in
                // the queue was cancelled. Hand the slot straight back.
                permits.Release();
                continue;
            }

            {
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
        PROFILE_ZONE;
        // Check if job should be cancelled
        // Per-task cancellation check (Minecraft-style)
        if (job.snapshot && job.snapshot->IsCancelled()) {
            m_stats.meshJobsCancelled.fetch_add(1, std::memory_order_relaxed);
            // Send failed result so section doesn't stay stuck in MESHING
            Network::MeshBuildResult failedResult(job.chunkPos, job.sectionY);
            failedResult.success = false;
            failedResult.generation = job.snapshot->generation;
            SendMeshResult(std::move(failedResult));
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
                m_stats.verticesGenerated.fetch_add(result.meshData.GetTotalVertexCount(), std::memory_order_relaxed);
                m_stats.indicesGenerated.fetch_add(result.meshData.GetTotalIndexCount(), std::memory_order_relaxed);
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
        
        // Copy neighbor mask from snapshot (computed on main thread where chunk presence is known)
        result.neighborMask = job.snapshot->neighborMask;
        
        // Handle based on job type
        if (job.snapshot->jobType == Client::Render::MeshJobType::BorderOnly) {
            // BorderOnly job - just compute neighbor mask (already done) and return empty geometry
            result.success = true;
            return result;
        }
        
        // Full mesh job - check if section is empty
        if (job.snapshot->region.CentreIsEmpty()) {
            result.success = true; // Empty section is valid, just no geometry
            return result;
        }
        
        // Build mesh using the real Mesher, feeding the snapshot directly —
        // the fast path fills the mesher's block cache with memcpys from the
        // snapshot's flat arrays instead of per-block virtual GetBlock calls.
        Render::Mesher mesher;
        Render::SectionMesh sectionMesh;
        mesher.BuildSectionMesh(job.snapshot->region, job.chunkPos, job.sectionY, sectionMesh);
        
        // Convert SectionMesh to MeshBuildResult format
        result = ConvertSectionMeshToResult(sectionMesh, job.chunkPos, job.sectionY);
        result.generation = job.snapshot->generation;  // Restore generation after conversion
        result.neighborMask = job.snapshot->neighborMask;  // Restore neighbor mask after conversion
        result.success = true;
        
        return result;
    }

    bool ClientWorkerPool::EnqueueJob(MeshJob&& job) {
        std::unique_lock<std::mutex> lock(m_jobQueueMutex);

        // No capacity limit. MC's compile queue (CompileTaskDynamicQueue) has
        // none either, and for the same reason it does not need one: a section
        // is marked not-dirty when it is scheduled, so depth is bounded by the
        // number of DIRTY VISIBLE sections, not by anything unbounded.
        //
        // Execution is gated one stage down by the buffer pool
        // (SectionRenderDispatcher.runTask -> our MeshUploadPermits), which is
        // where throughput is supposed to be limited. A cap here limited
        // ADMISSION instead, which is the mistake this file's scheduler comment
        // warns about at length.
        m_jobQueue.push_back(std::move(job));
        lock.unlock();
        m_jobCondition.notify_one();
        return true;
    }

    // Port of MC CompileTaskDynamicQueue.poll(Vec3) — see
    // minecraft_code/decompiled_net/minecraft/client/renderer/chunk/CompileTaskDynamicQueue.java
    //
    // Two things make this a linear scan rather than a sorted container:
    //   1. The key is distance to the CAMERA, which moves every frame. Anything
    //      ordered at insertion time is stale by the time it is polled, and the
    //      player walking away from a queued section would not reorder it.
    //   2. The recompile quota needs the best candidate of EACH kind, which a
    //      single-ordered structure cannot give you in one pop.
    //
    // Cost is O(queue) per poll under the lock, exactly as MC does it. Polls are
    // bounded by pipeline throughput (a poll only happens after a permit is
    // acquired), not by frame rate, so this is a few thousand distance
    // computations per second at most.
    std::optional<MeshJob> ClientWorkerPool::PollNearestLocked(const glm::vec3& cameraPos) {
        // Phase 1: drop cancelled entries (MC does this inline via
        // iterator.remove()). Swap-and-pop rather than a shifting erase — the
        // queue has no meaningful order, since selection is purely by distance.
        //
        // Dropping without producing a result is safe, and only because both
        // Cancel() sites guarantee no one is still waiting on this job:
        // ClientChunkManager::UnloadChunk cancels while the chunk is going away
        // (the section is about to stop existing), and
        // ScheduleMeshBuildsWithSnapshots cancels because the section was
        // re-dirtied and a REPLACEMENT job was just queued for it. A job polled
        // and then cancelled is a different case — ProcessMeshJob still sends a
        // failed result for that one, so the section cannot stick in MESHING.
        for (size_t i = 0; i < m_jobQueue.size(); ) {
            if (m_jobQueue[i].snapshot && m_jobQueue[i].snapshot->IsCancelled()) {
                if (i != m_jobQueue.size() - 1) {       // guard self-move-assign
                    m_jobQueue[i] = std::move(m_jobQueue.back());
                }
                m_jobQueue.pop_back();
                m_stats.meshJobsCancelled.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++i;
            }
        }
        if (m_jobQueue.empty()) return std::nullopt;

        // Phase 2: nearest initial compile and nearest recompile, separately.
        size_t bestInitial   = SIZE_MAX;
        size_t bestRecompile = SIZE_MAX;
        float  bestInitialDistSq   = std::numeric_limits<float>::max();
        float  bestRecompileDistSq = std::numeric_limits<float>::max();

        for (size_t i = 0; i < m_jobQueue.size(); ++i) {
            const MeshJob& job = m_jobQueue[i];

            // Section centre in world space. Y is attenuated by 0.1 (squared:
            // 0.01) — our deliberate deviation from MC's plain distSqr, kept
            // from the previous scheduler: this world is 384 blocks tall, so
            // without it a section 300 blocks overhead outranks one 200 blocks
            // out at eye level, which is not what the player is looking at.
            const float dx = job.chunkPos.x * 16.0f + 8.0f - cameraPos.x;
            const float dz = job.chunkPos.z * 16.0f + 8.0f - cameraPos.z;
            const float dy = (-64.0f + job.sectionY * 16.0f + 8.0f) - cameraPos.y;
            const float distSq = dx * dx + dz * dz + dy * dy * 0.01f;

            if (!job.isRecompile) {
                if (distSq < bestInitialDistSq) { bestInitialDistSq = distSq; bestInitial = i; }
            } else {
                if (distSq < bestRecompileDistSq) { bestRecompileDistSq = distSq; bestRecompile = i; }
            }
        }

        // Phase 3: MC's arbitration. Take the initial compile unless a recompile
        // is BOTH nearer AND the quota still allows one; running out of quota
        // forces an initial compile through regardless of distance.
        const bool hasInitial   = (bestInitial   != SIZE_MAX);
        const bool hasRecompile = (bestRecompile != SIZE_MAX);

        size_t chosen;
        if (!hasRecompile || (hasInitial && (m_recompileQuota <= 0 ||
                                             !(bestRecompileDistSq < bestInitialDistSq)))) {
            m_recompileQuota = kMaxRecompileQuota;
            chosen = bestInitial;
        } else {
            --m_recompileQuota;
            chosen = bestRecompile;
        }
        if (chosen == SIZE_MAX) return std::nullopt;

        MeshJob job = std::move(m_jobQueue[chosen]);
        if (chosen != m_jobQueue.size() - 1) {          // guard self-move-assign
            m_jobQueue[chosen] = std::move(m_jobQueue.back());
        }
        m_jobQueue.pop_back();
        return job;
    }

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
        auto& queue = Render::ClientMeshManager::GetMeshResultQueue();
        // Read before the push: try_push may move from `result`.
        const bool holdsPermit = result.holdsUploadPermit;
        if (!queue.try_push(std::move(result))) {
            // Queue was full, so the result is gone and the render thread will
            // never see it — release the permit here or the pipeline loses a
            // slot permanently. With the permit pool sized below the queue's
            // capacity this should be unreachable; it is handled anyway
            // because the failure mode is a slow starvation that would be
            // very hard to trace back to here.
            //
            // Synchronously-compiled results never took a permit, so there is
            // nothing to hand back for those.
            if (holdsPermit) ::Render::GetMeshUploadPermits().Release();
            return;
        }
        queue.IncrementProcessed();
    }

    // Bulk-copy a Vertex array into a flat float array using memcpy instead of
    // individual push_back calls per vertex. The Vertex struct layout
    // (vec3 pos, vec2 uv, uint32 packedColor) is 6 float-sized slots per vertex
    // (3 float pos + 2 float UV + 1 uint32 packed color).
    static void CopyVertexLayer(const std::vector<Render::Vertex>& verts,
                                std::vector<float>& outFloats) {
        static_assert(sizeof(Render::Vertex) == 6 * sizeof(float),
                      "Vertex layout changed — update CopyVertexLayer");

        const size_t floatCount = verts.size() * 6;
        outFloats.resize(floatCount);
        std::memcpy(outFloats.data(), verts.data(), floatCount * sizeof(float));
    }

    Network::MeshBuildResult ClientWorkerPool::ConvertSectionMeshToResult(const Render::SectionMesh& sectionMesh,
                                                                          Game::Math::ChunkPos chunkPos, int sectionY) {
        Network::MeshBuildResult result(chunkPos, sectionY);

        // Opaque layer
        if (!sectionMesh.opaqueVerts.empty()) {
            CopyVertexLayer(sectionMesh.opaqueVerts, result.meshData.opaqueVertices);
            result.meshData.opaqueIndices = sectionMesh.opaqueIdxs;
            result.meshData.opaqueVertexCount = sectionMesh.opaqueVerts.size();
            result.meshData.opaqueIndexCount = sectionMesh.opaqueIdxs.size();
        }

        // Cutout layer
        if (!sectionMesh.cutoutVerts.empty()) {
            CopyVertexLayer(sectionMesh.cutoutVerts, result.meshData.cutoutVertices);
            result.meshData.cutoutIndices = sectionMesh.cutoutIdxs;
            result.meshData.cutoutVertexCount = sectionMesh.cutoutVerts.size();
            result.meshData.cutoutIndexCount = sectionMesh.cutoutIdxs.size();
        }

        // Translucent layer
        if (!sectionMesh.translucentVerts.empty()) {
            CopyVertexLayer(sectionMesh.translucentVerts, result.meshData.translucentVertices);
            result.meshData.translucentIndices = sectionMesh.translucentIdxs;
            result.meshData.translucentVertexCount = sectionMesh.translucentVerts.size();
            result.meshData.translucentIndexCount = sectionMesh.translucentIdxs.size();
        }

        // Occlusion culling data
        result.visibilitySet = sectionMesh.visibilitySet;

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

    void UncancelClientMeshChunk(Game::Math::ChunkPos chunkPos) {
        if (g_clientWorkerPool) {
            g_clientWorkerPool->UncancelChunk(chunkPos);
        }
    }

    void SetClientWorkerWorld(Game::World* world) {
        if (g_clientWorkerPool) {
            g_clientWorkerPool->SetWorld(world);
        }
    }

} // namespace Threading