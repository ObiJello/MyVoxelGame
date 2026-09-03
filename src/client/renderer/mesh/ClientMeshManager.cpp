// File: src/client/renderer/mesh/ClientMeshManager.cpp
#include "ClientMeshManager.hpp"
#include "client/world/ClientLevel.hpp"
#include "ChunkRenderer.hpp"
#include "MeshUploadPermits.hpp"
#include <cstring>
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

    // Bound-level pointer, owned by ClientLevel (see ClientLevel.hpp).
    ClientMeshManager* g_clientMeshManager = nullptr;
    
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
        // Per-section index buffers for TRANSLUCENT only — MC's layout
        // (CompiledSectionMesh -> SectionBuffers per layer). Re-sorts rewrite
        // this layer's indices constantly, and writing into a shared slab IBO
        // with draws in flight cost 0.18ms/write against a 0.011ms sort.
        m_translucentMegaBuffer.Initialize(256000, 512000, /*perSectionIndexBuffers=*/false);

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
            BumpGpuDataGeneration();
        }

        // Every GPUSectionData was just destroyed — cached reachable lists in
        // the chunk renderer must be discarded before the next world renders.
        if (m_renderer) {
            m_renderer->MarkSectionDataErased();
            m_renderer->MarkVisibleSectionsDirty();
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

        // Return slab ranges freed a few frames ago to their free-lists. This
        // is what stops a re-mesh from overwriting geometry an in-flight frame
        // is still drawing — the one-frame see-through-the-world flash when
        // breaking blocks on Vulkan. Must run every frame: RemoveSection parks
        // ranges here and nothing else ever reclaims them.
        m_opaqueMegaBuffer.RetireFreedRegions();
        m_cutoutMegaBuffer.RetireFreedRegions();
        m_translucentMegaBuffer.RetireFreedRegions();

        // Periodic cleanup: remove empty slabs from mega-buffer pools.
        // With slab pool architecture, this just frees unused GPU memory — no data copy.
        static int compactFrameCounter = 0;
        if (++compactFrameCounter >= 600) {
            compactFrameCounter = 0;
            m_opaqueMegaBuffer.CompactIfNeeded();
            m_cutoutMegaBuffer.CompactIfNeeded();
            m_translucentMegaBuffer.CompactIfNeeded();
        }

        // The results themselves are drained by DrainMeshResults, once per
        // frame for every level, because the queue they sit in is shared.
        m_stats.meshUploadsThisFrame = 0;
        m_stats.gpuUploadTimeMs = 0.0f;
    }

    // ========================================================================
    // PLAYER POSITION UPDATES
    // ========================================================================

    bool ClientMeshManager::ResortTranslucentSection(Game::Math::ChunkPos chunkPos, int sectionY,
                                                     const glm::vec3& cameraPos,
                                                     bool blockPosChanged, bool isNearby) {
        auto it = m_gpuData.find(SectionKey{chunkPos, sectionY});
        if (it == m_gpuData.end()) return false;
        GPUSectionData& gpuData = it->second;

        // MC: section.hasTranslucentGeometry().
        if (gpuData.translucentCentroids.empty()) return false;
        if (!gpuData.translucentDrawCmd.valid) return false;

        const glm::ivec3 origin(
            chunkPos.x * 16,
            Game::Math::WorldCoordinates::SectionCoordsToWorldY(sectionY, 0),
            chunkPos.z * 16);

        const auto pov = TranslucentSort::MakePointOfView(cameraPos, origin);
        const bool povChanged = pov != gpuData.translucencyPov;
        // MC: a one-block camera move only forces a re-sort when the view
        // is axis-aligned (where a single step can reorder quads) or the
        // section is close enough for the error to show.
        const bool blockMoveForces = blockPosChanged && (pov.IsAxisAligned() || isNearby);
        if (!povChanged && !blockMoveForces) return false;

        // Split because the fix differs entirely depending on which half costs:
        // MC runs the SORT on a worker (RenderSection.resortTransparency ->
        // dispatcher.schedule(ResortTransparencyTask)) but uploads on the render
        // thread like we do. So if Resort.Sort dominates, going async is the
        // MC-faithful fix; if Resort.Upload dominates, async buys nothing and the
        // problem is the buffer update instead.
        { PROFILE_ZONE_N("Resort.Sort");
        TranslucentSort::BuildSortedIndices(gpuData.translucentCentroids, cameraPos,
                                            m_resortIndexScratch, m_resortOrderScratch,
                                            m_resortKeyScratch);
        }
        if (m_resortIndexScratch.empty()) return false;

        const MegaBufferSectionKey megaKey{chunkPos, sectionY};
        { PROFILE_ZONE_N("Resort.Upload");
        if (!m_translucentMegaBuffer.UpdateSectionIndices(megaKey,
                                                          m_resortIndexScratch.data(),
                                                          m_resortIndexScratch.size())) {
            return false;
        }
        }
        gpuData.translucencyPov = pov;
        return true;
    }

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

    void ClientMeshManager::RemeshAll() {
        if (!m_chunkManager) return;
        // Collect first: MarkSectionDirty runs renderer/chunk-map code, and
        // ForEachActiveSection holds m_gpuDataMutex for its whole iteration —
        // calling out while holding it invites lock-order trouble.
        std::vector<SectionKey> keys;
        keys.reserve(GetGPUDataCount());
        ForEachActiveSection([&keys](const SectionKey& key, const GPUSectionData*) {
            keys.push_back(key);
        });
        for (const auto& key : keys) {
            m_chunkManager->MarkSectionDirty(key.chunkPos, key.sectionY);
        }
        Log::Info("RemeshAll: marked %zu active sections dirty", keys.size());
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
                if (!result.success) {
                    m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                    // A failed LATEST job would otherwise leave the section
                    // stuck "in flight" forever — see NoteMeshBuildFailed.
                    m_chunkManager->NoteMeshBuildFailed(result.chunkPos, result.sectionY,
                                                        result.generation);
                    return;
                }

                if (!ValidateMeshBuildResult(result)) {
                    m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                    m_chunkManager->NoteMeshBuildFailed(result.chunkPos, result.sectionY,
                                                        result.generation);
                    return;
                }

                if (m_chunkManager && !m_chunkManager->IsChunkLoaded(result.chunkPos)) {
                    m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                // Named for what it does, not "GPUUpload" — that collided with
                // the frame-level phase in PlatformMain and the trace showed
                // the same label at two different depths.
                { PROFILE_ZONE_N("WriteBuffers");
                UploadMeshResultToGPU(result.chunkPos, result.sectionY, result.meshData, result.visibilitySet);
                }

                { PROFILE_ZONE_N("FinalizeUpload");
                m_chunkManager->FinalizeSectionUpload(result.chunkPos, result.sectionY,
                                                      result.neighborMask, result.generation,
                                                      result.paletteGen, result.jobSeq);
                }

                m_stats.meshBuildsCompleted.fetch_add(1, std::memory_order_relaxed);
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
                // Superseded by newer mesh. Still route through
                // NoteMeshBuildFailed: it re-dirties ONLY when the dropped
                // result was somehow the section's latest job (its generation
                // matches meshingVersion), which would otherwise leave the
                // section reading as "in flight" forever with a stale mesh on
                // the GPU. For a genuinely superseded result it is a no-op.
                m_stats.meshBuildsSkipped.fetch_add(1, std::memory_order_relaxed);
                m_chunkManager->NoteMeshBuildFailed(result.chunkPos, result.sectionY,
                                                    result.generation);
                LogMeshActivity("Dropped replaced mesh", result.chunkPos, result.sectionY);
                break;
        }
    }
    
    // Port of SectionRenderDispatcher.uploadAllPendingUploads (:104), which
    // drains its whole queue with no budget of any kind:
    //
    //     while ((upload = this.toUpload.poll()) != null) upload.run();
    //
    // That is only safe because the permit pool already bounded how much could
    // be in the queue. A CPU-millisecond budget here would be measuring the
    // wrong thing anyway: glBufferSubData returns once the driver has staged
    // the copy, so the loop can exit well inside budget having handed the GPU
    // more work than it can retire — and Present pays the difference.
    void ClientMeshManager::DrainMeshResults(
            const std::function<ClientMeshManager*(Game::DimensionId)>& resolve) {
        PROFILE_ZONE_N("DrainUploads");
        int uploadsThisFrame = 0;
        // Managers touched this drain, for the byte plot below (at most one
        // per dimension).
        ClientMeshManager* touched[Game::kDimensionCount] = {};

        auto& meshResultQueue = GetMeshResultQueue();
        auto& permits = GetMeshUploadPermits();

        // QueueDepth is the health metric: with backpressure working it should
        // sit at or below the permit capacity and return to 0 every frame.
        // A rising QueueDepth means a permit is being leaked somewhere.
        const size_t readyAtFrameStart = meshResultQueue.Size();
        PROFILE_PLOT("Upload/QueueDepth", static_cast<int64_t>(readyAtFrameStart));

        // Permits are BANKED here and released once, after the loop.
        //
        // Releasing inside the loop made it self-feeding: each iteration freed a
        // permit, a worker parked on its 250us backoff claimed it instantly,
        // compiled, and pushed a result that this same loop then popped. Queue
        // depth still read <= capacity (the permits were doing their job) while
        // 228 sections went up in ONE frame and MeshUpload hit 21 ms.
        //
        // MC has the same permit span — compile AND upload, verified:
        // SectionRenderDispatcher.runTask:79 `supplyAsync(...).thenCompose(f->f)`
        // flattens onto doTask's uploadFuture (:402), which is runAsync onto the
        // main-thread upload executor (:227, :57), so bufferPool.release(:91)
        // happens only after the render thread drained that upload. What MC has
        // that we lacked is the consecutiveExecutor.schedule hop at :85 — the
        // release is deferred to a scheduled task instead of firing synchronously
        // inside the drain. Banking replicates exactly that.
        //
        // With no permit freed mid-drain, no compile started during the drain can
        // enter the queue, so the loop is provably bounded by
        // queueDepthAtStart + capacity <= 2 x capacity — no arbitrary cap, and no
        // frame-rate coupling (an earlier cap of "what was pending at frame
        // start" pinned throughput to permits x fps, measured 896/s at 56 fps).
        // RAII so an exception out of ProcessMeshBuildResult cannot strand the
        // banked permits — the per-item guard this replaces gave that for free,
        // and losing permits permanently shrinks the pipeline until meshing
        // stops altogether.
        struct BatchPermitRelease {
            MeshUploadPermits& p;
            size_t count = 0;
            ~BatchPermitRelease() { for (size_t i = 0; i < count; ++i) p.Release(); }
        };

        // Scoped so the release happens BEFORE the plots below — otherwise
        // Upload/InFlight would report every permit still held and always read
        // as a full pipeline.
        {
        BatchPermitRelease banked{permits};

        while (true) {
            Network::MeshBuildResult result;
            { PROFILE_ZONE_N("PopResult");
            if (!meshResultQueue.try_pop(result)) {
                break;
            }
            }

            // Banked, not released: ProcessMeshBuildResult returns early on four
            // separate drop paths (build failed, failed validation, chunk
            // unloaded, stale/replaced generation), and every one of them still
            // owes its permit back. Counting before the call covers all of them.
            //
            // Synchronously-compiled sections never took a permit — see
            // MeshBuildResult::holdsUploadPermit.
            if (result.holdsUploadPermit) ++banked.count;

            // Route by the dimension the job was built for. A result for a
            // level that no longer exists is simply dropped — its permit is
            // already banked above.
            ClientMeshManager* target = resolve(result.dimension);
            if (!target) continue;
            touched[Game::DimensionSlot(result.dimension)] = target;

            const auto t0 = std::chrono::steady_clock::now();
            target->ProcessMeshBuildResult(result);
            target->m_stats.gpuUploadTimeMs += std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            uploadsThisFrame++;
            target->m_stats.meshUploadsThisFrame++;
        }

        }  // banked releases here — loop is done, so it cannot be re-fed.

        PROFILE_PLOT("Upload/Sections",  static_cast<int64_t>(uploadsThisFrame));
        PROFILE_PLOT("Upload/InFlight",  static_cast<int64_t>(permits.InFlight()));
        int64_t uploadedBytes = 0;
        for (ClientMeshManager* m : touched) {
            if (!m) continue;
            uploadedBytes += static_cast<int64_t>(
                m->m_opaqueMegaBuffer.ConsumeUploadedBytes() +
                m->m_cutoutMegaBuffer.ConsumeUploadedBytes() +
                m->m_translucentMegaBuffer.ConsumeUploadedBytes());
        }
        PROFILE_PLOT("Upload/Bytes", uploadedBytes);
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
    
    // Float-sized slots per terrain vertex in the MeshBuildResult blobs.
    // 32-byte TerrainVertex = 8 slots (pos3f + uv2f + rgba8 + tile unorm16x4);
    // the tail rides the float vector as opaque bytes (see CopyVertexLayer).
    // This was a literal 6 from the 24-byte era — the greedy-meshing stride
    // bump missed it, and every non-empty mesh failed validation, which
    // NoteMeshBuildFailed then re-dirtied: 10k rebuilds/s, zero uploads.
    static constexpr size_t kTerrainVertexFloatSlots =
        sizeof(Render::TerrainVertex) / sizeof(float);

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
            if (result.meshData.opaqueVertexCount * kTerrainVertexFloatSlots != result.meshData.opaqueVertices.size()) {
                return false; // see kTerrainVertexFloatSlots
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
            if (result.meshData.cutoutVertexCount * kTerrainVertexFloatSlots != result.meshData.cutoutVertices.size()) {
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
            if (result.meshData.translucentVertexCount * kTerrainVertexFloatSlots != result.meshData.translucentVertices.size()) {
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
            // Clear the section's published pointer BEFORE erasing the map entry —
            // the occlusion BFS reads this atomic directly, so leaving it set
            // would dangle into a freed map node.
            auto* sectionInfo = m_chunkManager->GetSectionInfo(chunkPos, sectionY);
            if (sectionInfo) {
                sectionInfo->gpuData.store(nullptr, std::memory_order_release);
            }
            BumpGpuDataGeneration();
            m_chunkManager->NotifyRenderGridSectionUpdated(chunkPos, sectionY, nullptr);

            // Remove from mega-buffers (frees GPU regions)
            MegaBufferSectionKey megaKey{chunkPos, sectionY};
            m_opaqueMegaBuffer.RemoveSection(megaKey);
            m_cutoutMegaBuffer.RemoveSection(megaKey);
            m_translucentMegaBuffer.RemoveSection(megaKey);

            // Plain erase. The renderer's lists carry identity only and
            // resolve live each frame, so a section that goes away simply
            // stops resolving — there is no pointer left to keep alive.
            m_gpuData.erase(it);

            if (m_renderer) {
                m_renderer->MarkVisibleSectionsDirty();
            }
            LogMeshActivity("Removed section GPU data", chunkPos, sectionY);
        }
    }

    size_t ClientMeshManager::ParkChunkGPUData(::Game::Math::ChunkPos chunkPos) {
        std::unique_lock<std::shared_mutex> lock(m_gpuDataMutex);
        size_t bytes = 0;
        for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
            SectionKey key{chunkPos, sectionY};
            auto it = m_gpuData.find(key);
            if (it == m_gpuData.end()) continue;
            const GPUSectionData& d = it->second;
            bytes += (size_t(d.opaqueVertexCount) + d.cutoutVertexCount + d.translucentVertexCount) * 16
                   + (size_t(d.opaqueIndexCount) + d.cutoutIndexCount + d.translucentIndexCount) * 4;
            if (auto* sectionInfo = m_chunkManager->GetSectionInfo(chunkPos, sectionY)) {
                sectionInfo->gpuData.store(nullptr, std::memory_order_release);
            }
            BumpGpuDataGeneration();
            m_chunkManager->NotifyRenderGridSectionUpdated(chunkPos, sectionY, nullptr);
            // Node handle: the GPUSectionData object keeps its address.
            auto node = m_gpuData.extract(it);
            m_parkedGpuData.insert(std::move(node));
        }
        if (m_renderer) m_renderer->MarkVisibleSectionsDirty();
        return bytes;
    }

    bool ClientMeshManager::UnparkChunkGPUData(::Game::Math::ChunkPos chunkPos) {
        std::unique_lock<std::shared_mutex> lock(m_gpuDataMutex);
        bool any = false;
        for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
            SectionKey key{chunkPos, sectionY};
            auto it = m_parkedGpuData.find(key);
            if (it == m_parkedGpuData.end()) continue;
            auto node = m_parkedGpuData.extract(it);
            auto ins = m_gpuData.insert(std::move(node));
            GPUSectionData* data = &ins.position->second;
            if (auto* sectionInfo = m_chunkManager->GetSectionInfo(chunkPos, sectionY)) {
                sectionInfo->gpuData.store(data, std::memory_order_release);
            }
            BumpGpuDataGeneration();
            m_chunkManager->NotifyRenderGridSectionUpdated(chunkPos, sectionY, data);
            any = true;
        }
        if (any && m_renderer) m_renderer->MarkVisibleSectionsDirty();
        return any;
    }

    void ClientMeshManager::DiscardParkedChunkGPUData(::Game::Math::ChunkPos chunkPos) {
        std::unique_lock<std::shared_mutex> lock(m_gpuDataMutex);
        for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
            SectionKey key{chunkPos, sectionY};
            auto it = m_parkedGpuData.find(key);
            if (it == m_parkedGpuData.end()) continue;
            MegaBufferSectionKey megaKey{chunkPos, sectionY};
            m_opaqueMegaBuffer.RemoveSection(megaKey);
            m_cutoutMegaBuffer.RemoveSection(megaKey);
            m_translucentMegaBuffer.RemoveSection(megaKey);
            m_parkedGpuData.erase(it);
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
                BumpGpuDataGeneration();

                m_chunkManager->NotifyRenderGridSectionUpdated(chunkPos, sectionY, nullptr);

                // Plain erase — see RemoveSectionGPUData.
                m_gpuData.erase(it);
            }
        }

        if (m_renderer) {
            m_renderer->MarkVisibleSectionsDirty();
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
        // Whether this section already had a (published) entry — decides
        // tombstone vs plain erase if the new mesh turns out empty below.
        const bool hadEntry = m_gpuData.find(key) != m_gpuData.end();
        GPUSectionData& gpuData = m_gpuData[key];

        // Initialize GPU data
        gpuData.chunkPos = chunkPos;
        gpuData.sectionY = sectionY;

        // Remove existing mega-buffer regions for this section (re-upload)
        MegaBufferSectionKey megaKey{chunkPos, sectionY};
        {
            PROFILE_ZONE_N("Upl.RemoveOld");
            m_opaqueMegaBuffer.RemoveSection(megaKey);
            m_cutoutMegaBuffer.RemoveSection(megaKey);
            m_translucentMegaBuffer.RemoveSection(megaKey);
        }

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
                gpuData.opaqueDrawCmd = {cmd.indexCount, cmd.indexOffset, true, cmd.slabIndex};
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
                gpuData.cutoutDrawCmd = {cmd.indexCount, cmd.indexOffset, true, cmd.slabIndex};
            }
        }
        if (!meshData.translucentVertices.empty() && !meshData.translucentIndices.empty()) {
            PROFILE_ZONE_N("Upl.Translucent");
            m_translucentMegaBuffer.UploadSection(megaKey,
                meshData.translucentVertices.data(),
                meshData.translucentVertexCount,
                meshData.translucentIndices.data(),
                meshData.translucentIndexCount);
            gpuData.translucentVertexCount = static_cast<uint32_t>(meshData.translucentVertexCount);
            gpuData.translucentIndexCount = static_cast<uint32_t>(meshData.translucentIndexCount);
            ChunkMegaBuffer::DrawCommand cmd;
            if (m_translucentMegaBuffer.GetDrawCommand(megaKey, cmd)) {
                gpuData.translucentDrawCmd = {cmd.indexCount, cmd.indexOffset, true, cmd.slabIndex,
                                              m_translucentMegaBuffer.GetSectionIndexBuffer(megaKey)};
            }

            // Quad centroids for back-to-front re-sorting. MC takes the
            // midpoint of vertices 0 and 2 — the quad's diagonal
            // (MeshData.unpackQuadCentroids) — and quad k owns vertices
            // 4k..4k+3, which is what GenerateQuad and FluidMeshBuilder emit.
            // Positions are the first three floats of each vertex.
            {
                // Sorted on the worker (ClientWorkerPool::ConvertSectionMeshToResult);
                // keep the centroids for later re-sorts and record the view.
                static_assert(sizeof(Render::TerrainVertex) == 32,
                              "translucent centroid extraction assumes the 32-byte terrain vertex");
                const size_t quads = meshData.translucentCentroids.size() / 3;
                gpuData.translucentCentroids.resize(quads);
                if (quads > 0) {
                    std::memcpy(gpuData.translucentCentroids.data(),
                                meshData.translucentCentroids.data(),
                                quads * sizeof(glm::vec3));
                }
                gpuData.translucencyPov = TranslucentSort::PointOfView{};
                if (meshData.translucentPovValid) {
                    gpuData.translucencyPov.x = meshData.translucentPovX;
                    gpuData.translucencyPov.y = meshData.translucentPovY;
                    gpuData.translucencyPov.z = meshData.translucentPovZ;
                    gpuData.translucencyPov.valid = true;
                }
            }
        }
        gpuData.lastUploadFrame = 0; // TODO: Add frame counter
        gpuData.needsUpload = false;

        // Skip empty sections entirely to avoid "zombie" entries in m_gpuData
        // that waste map lookup time during rendering. Clear the section's
        // published atomic pointer first — a previous upload of this section
        // may have set it, and the occlusion BFS reads it directly.
        if (!gpuData.HasGeometry()) {
            auto* emptySectionInfo = m_chunkManager->GetSectionInfo(chunkPos, sectionY);
            if (emptySectionInfo) {
                emptySectionInfo->gpuData.store(nullptr, std::memory_order_release);
            }
            BumpGpuDataGeneration();
            auto it = m_gpuData.find(key);
            if (hadEntry) {
                // Had geometry, remeshed to empty: cached reachable lists may
                // still point at it — tombstone (address-preserving) instead
                // of destroying, so no full cache invalidation is needed.
                m_gpuData.erase(it);
            } else {
                // Fresh entry created by this very call — nothing references it.
                m_gpuData.erase(it);
            }
            if (m_renderer) {
                m_renderer->MarkVisibleSectionsDirty();
            }
            return;
        }

        // Store GPU data pointer in the atomic field for lock-free rendering
        auto* sectionInfo = m_chunkManager->GetSectionInfo(chunkPos, sectionY);
        if (sectionInfo) {
            // Store the pointer to the GPU data in the hash map
            GPUSectionData* gpuDataPtr = &m_gpuData[key];

            // Atomically update the GPU data pointer
            GPUSectionData* oldPtr = sectionInfo->gpuData.exchange(gpuDataPtr, std::memory_order_release);
            // A fresh entry changes what the section resolves to — a renderer
            // cache stamped before this must look it up again. A re-upload of
            // an existing entry keeps its address, but bumping anyway costs one
            // frame of lookups and keeps the rule simple: any store, any bump.
            BumpGpuDataGeneration();

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

        if (m_renderer) {
            // MC LevelRenderer.addRecentlyCompiledSection -> schedulePropagationFrom.
            //
            // Note what is deliberately NOT here: MarkVisibleSectionsDirty.
            // A section finishing its compile does not invalidate the occlusion
            // graph, matching MC — its invalidate() has exactly two callers, an
            // 8-block camera move in cullTerrain and needsUpdate(). The graph
            // learns about this section by propagation instead.
            //
            // This call WAS here, and removing it once made Sections/Reachable
            // climb to 5652 and never recover while real draws stayed near
            // 1962/frame. The reason was not the invalidation itself: the
            // visible list cached a GPUSectionData* and a layerMask captured at
            // graph-build time, those went stale on every remesh, and the full
            // rebuild was quietly acting as the list's garbage collector.
            //
            // The list now carries identity only and the draw path resolves
            // live each frame (ChunkRenderer's frustum filter), so a stale entry
            // resolves to null and costs one failed lookup instead of a wrong
            // draw. There is nothing left for a periodic rebuild to collect.
            m_renderer->SchedulePropagationFrom(chunkPos, sectionY);
        }

        LogMeshActivity("Uploaded mesh result to GPU", chunkPos, sectionY);
    }

    // ========================================================================
    // SHARED BLOCK VERTEX FORMAT
    // ========================================================================

    // One vertex format per backend, however many levels exist: the first
    // manager to need it creates it, the last to let go destroys it.
    static int s_sharedVaoRefs = 0;

    void ClientMeshManager::CreateSharedBlockVAO() {
        if (m_holdsSharedVao) return;
        m_holdsSharedVao = true;
        if (s_sharedVaoRefs++ == 0 && g_renderBackend) {
            g_renderBackend->SetupBlockVertexFormat();
        }
    }

    void ClientMeshManager::DestroySharedBlockVAO() {
        if (!m_holdsSharedVao) return;
        m_holdsSharedVao = false;
        if (--s_sharedVaoRefs == 0 && g_renderBackend) {
            g_renderBackend->DestroyBlockVertexFormat();
        }
    }

    void ClientMeshManager::BindSharedBlockVAO() {
        if (g_renderBackend) {
            g_renderBackend->BindBlockVertexFormat();
        }
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

    void ScheduleClientMeshBuilds(const glm::vec3& playerPosition) {
        if (g_clientMeshManager) {
            g_clientMeshManager->ScheduleMeshBuilds(playerPosition);
        }
    }

    void PerformClientGPUUploads() {
        Client::ClientLevels::ForEach([](Client::ClientLevel& level) {
            if (auto* meshes = level.Meshes()) meshes->PerformGPUUploads();
        });
        ClientMeshManager::DrainMeshResults([](Game::DimensionId dimension) -> ClientMeshManager* {
            Client::ClientLevel* level = Client::ClientLevels::Get(dimension);
            return level ? level->Meshes() : nullptr;
        });
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