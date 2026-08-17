// File: src/client/renderer/mesh/ChunkRenderer.cpp
#include "ChunkRenderer.hpp"
#include "ChunkMegaBuffer.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "../backend/RenderBackend.hpp"
#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif
#include "../environment/EnvironmentState.hpp"
#include "common/core/Features.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "platform/GameDirectory.hpp"
#include "client/network/NetworkClient.hpp"
#include "../../world/ClientChunkManager.hpp"
#include <algorithm>
#include <chrono>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>

namespace Render {

    // Global chunk renderer instance
    std::unique_ptr<ChunkRenderer> g_chunkRenderer = nullptr;

    // Runtime toggle for the per-pass GPU timers (Debug UI → Render Controls).
    // DEFAULT OFF ON BOTH BACKENDS, for two different reasons.
    //
    // OpenGL: on Apple's GL driver glEndQuery forces a full flush costing
    // ~2.3ms per call — three passes is ~7ms/frame, far more than the passes
    // themselves. Enable briefly, read the numbers, turn it off.
    //
    // Vulkan: the queries are cheap (vkCmdWriteTimestamp records a stamp,
    // results are polled non-blocking) but the NUMBERS ARE NOT MEANINGFUL on
    // Metal, so do not trust the F3 readout. Measured 2026-08: with 2 draws in
    // the frame the cutout pass reported 40us; with 6695 draws it reported
    // 59us. A ~3000x change in work moved the reading by 1.5x — it is not
    // measuring GPU execution.
    //
    // The reason is architectural, not a bug in our code. Apple GPUs are
    // tile-based deferred: inside a render pass all geometry is binned first
    // and fragment shading happens per-tile at the END of the pass, so there
    // is no instant at which "the opaque pass has finished on the GPU" is a
    // physical fact for a timestamp to capture. Metal can only sample counters
    // at encoder boundaries, and our entire frame is one render pass / one
    // encoder. Splitting the frame into three render passes to measure it
    // would cost more than it could ever reveal.
    //
    // To measure GPU cost on this backend, use (Vk.QueueSubmit + Vk.FenceWait)
    // as the whole-frame GPU proxy — the two are one shared wait and their SUM
    // is stable (measured Pearson r = -0.778 between them, constant sum across
    // every bucket) — or use Instruments' Metal System Trace for real
    // per-encoder attribution.
    //
    // Disabling stops NEW queries; pending ones still drain so nothing leaks.
    bool g_enableGpuPassTimers = false;

    // Static portal clip plane (no clipping by default).
    glm::vec4 ChunkRenderer::s_portalClipPlane{0.0f};

    // MC's "close" radius for translucency re-sorting — the 32 handed to
    // SectionTree.visitNodes in SectionOcclusionGraph.addSectionsInFrustum:89.
    // Sections inside it are re-checked every frame; the rest ride the sweep.
    static constexpr float kTranslucentNearRadius   = 32.0f;
    static constexpr float kTranslucentNearRadiusSq = kTranslucentNearRadius * kTranslucentNearRadius;

    ChunkRenderer::ChunkRenderer() {
        SetupRenderConfigs();
        for (auto& slot : m_reachableSlots) slot.sections.reserve(2048);
        m_visibleSections.reserve(2048);
        m_perSlabCounts.reserve(16);
        m_perSlabOffsets.reserve(16);
        m_perSlabBaseVertices.reserve(16);
        m_distantCutoutCounts.reserve(1024);
        m_distantCutoutOffsets.reserve(1024);
        m_distantCutoutBaseVertices.reserve(1024);
    }

    ChunkRenderer::~ChunkRenderer() {
        Shutdown();
    }

    void ChunkRenderer::SetupRenderConfigs() {
        // Opaque pass configuration
        m_opaqueConfig.enableDepthWrite = true;
        m_opaqueConfig.enableDepthTest = true;
        m_opaqueConfig.enableBlending = false;
        m_opaqueConfig.enableAlphaTest = false;
        m_opaqueConfig.enableBackFaceCulling = true;
        m_opaqueConfig.frontToBack = true;

        // Cutout pass configuration
        m_cutoutConfig.enableDepthWrite = true;
        m_cutoutConfig.enableDepthTest = true;
        m_cutoutConfig.enableBlending = false;
        m_cutoutConfig.enableAlphaTest = true;
        m_cutoutConfig.enableBackFaceCulling = true;
        m_cutoutConfig.alphaThreshold = 0.5f;
        m_cutoutConfig.frontToBack = true;

        // Translucent pass configuration (Minecraft-style)
        // MC writes depth here too: TRANSLUCENT_TERRAIN overrides neither
        // writeDepth nor cull, and RenderPipeline.Builder defaults both to true
        // (RenderPipeline.java:434 — writeDepth.orElse(true)).
        //
        // It matters because glass.png's alpha is BINARY, 0 or 255 — the frame
        // and streaks are fully opaque and the interior is discarded outright
        // by the 0.01 cutout. With depth writes off those opaque texels
        // occluded nothing, so a glass block further away could be rasterised
        // afterwards and paint its streaks straight over the frame of the block
        // in front. Writing depth makes the frame occlude properly, while the
        // discarded interior still writes nothing and stays see-through.
        m_translucentConfig.enableDepthWrite = true;
        m_translucentConfig.enableDepthTest = true;
        m_translucentConfig.enableBlending = true;
        m_translucentConfig.enableAlphaTest = false;
        // MC's TRANSLUCENT_TERRAIN pipeline (RenderPipelines.java:170) builds
        // on TERRAIN_SNIPPET and overrides neither cull nor depth-write, and
        // RenderPipeline.Builder defaults BOTH to true — so vanilla back-face
        // culls translucent terrain just like every other pass.
        //
        // With culling off, a glass or ice block drew its FAR faces as well as
        // its near ones, so every block blended twice and the whole surface
        // came out roughly twice as tinted and visibly layered.
        //
        // Water keeps working because FluidMeshBuilder now emits MC's
        // backward-up face, the reverse-wound copy of the surface quad that
        // vanilla adds so the underside stays visible from in the water.
        m_translucentConfig.enableBackFaceCulling = true;
        m_translucentConfig.blendSrc = BlendFactor::SrcAlpha;
        m_translucentConfig.blendDst = BlendFactor::OneMinusSrcAlpha;
        m_translucentConfig.frontToBack = false;  // Back-to-front for proper blending
    }

    bool ChunkRenderer::Initialize() {
        Log::Info("Initializing ChunkRenderer...");

        if (!g_renderBackend) {
            Log::Error("Cannot initialize ChunkRenderer: no render backend");
            return false;
        }

        // Create separate shader programs for opaque (no discard → early-z enabled)
        // and cutout/translucent (with discard for alpha testing).
        // Matches Minecraft's SOLID_TERRAIN vs CUTOUT_TERRAIN pipeline split.
        // On Vulkan the block fragment shaders read the environment/fog fields
        // from the Common UBO, which needs the UBO-aware (portal) pipeline
        // layout — same backend-cast pattern as PortalRenderer/SkyRenderer.
        auto createBlockShader = [](const char* vertPath, const char* fragPath) {
            if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
                auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
                return vk->CreateShaderFromFilesPortal(vertPath, fragPath);
#else
                return INVALID_SHADER;
#endif
            }
            return g_renderBackend->CreateShaderFromFiles(vertPath, fragPath);
        };
        m_opaqueShader = createBlockShader("shaders/block.vert", "shaders/block_opaque.frag");
        if (m_opaqueShader == INVALID_SHADER) {
            Log::Error("Failed to create opaque block shader");
            return false;
        }
        m_cutoutShader = createBlockShader("shaders/block.vert", "shaders/block.frag");
        if (m_cutoutShader == INVALID_SHADER) {
            Log::Error("Failed to create cutout block shader");
            return false;
        }
        m_solidShader = createBlockShader("shaders/block.vert", "shaders/block_solid.frag");
        if (m_solidShader == INVALID_SHADER) {
            Log::Error("Failed to create solid block shader");
            return false;
        }
        m_backendShader = m_opaqueShader;
        m_shadersLoaded = true;
        Log::Info("Block shaders created (opaque + cutout + solid)");

        // Grab backend atlas texture handle (created by AtlasBuilder)
        if (g_atlasBuilder) {
            m_backendAtlasTexture = g_atlasBuilder->GetBackendTextureHandle();
        }

        // Reset statistics
        m_stats.Reset();

        Log::Info("ChunkRenderer initialized (render distance: %d chunks, backend: %s)",
                  Platform::g_gameSettings.GetRenderDistance(),
                  g_renderBackend->GetName());
        return true;
    }

    void ChunkRenderer::Shutdown() {
        if (g_renderBackend) {
            if (m_opaqueShader != INVALID_SHADER) {
                g_renderBackend->DestroyShader(m_opaqueShader);
                m_opaqueShader = INVALID_SHADER;
            }
            if (m_cutoutShader != INVALID_SHADER) {
                g_renderBackend->DestroyShader(m_cutoutShader);
                m_cutoutShader = INVALID_SHADER;
            }
            if (m_solidShader != INVALID_SHADER) {
                g_renderBackend->DestroyShader(m_solidShader);
                m_solidShader = INVALID_SHADER;
            }
        }
        m_backendShader = INVALID_SHADER;
        m_activeShader = INVALID_SHADER;
        m_blockShader.reset();
        m_shadersLoaded = false;
        Log::Info("ChunkRenderer shutdown complete");
    }

    void ChunkRenderer::RefreshSettings() {
        Log::Info("ChunkRenderer settings refreshed (render distance: %d chunks)", Platform::g_gameSettings.GetRenderDistance());
    }


    void ChunkRenderer::RenderOpaque(const Camera& camera, const Frustum& frustum) {
        PROFILE_ZONE;
        if (!m_shadersLoaded || !g_clientMeshManager || (m_debugLayer >= 0 && m_debugLayer != 0)) {
            m_stats.opaquePassTimeMs = 0.0f;
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Setup render state for opaque pass
        SetupRenderPass(m_opaqueConfig);

        // Render directly from m_visibleSections, filtering by layerMask (front-to-back)
        RenderLayerPass(RenderLayer::Opaque, LayerOpaque);

        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.opaquePassTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    void ChunkRenderer::RenderCutout(const Camera& camera, const Frustum& frustum) {
        PROFILE_ZONE;
        if (!m_shadersLoaded || !g_clientMeshManager || (m_debugLayer >= 0 && m_debugLayer != 1)) {
            m_stats.cutoutPassTimeMs = 0.0f;
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Setup render state for cutout pass
        SetupRenderPass(m_cutoutConfig);

        // Render directly from m_visibleSections, filtering by layerMask (front-to-back)
        RenderLayerPass(RenderLayer::Cutout, LayerCutout);

        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.cutoutPassTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    void ChunkRenderer::RenderTranslucent(const Camera& camera, const Frustum& frustum) {
        PROFILE_ZONE;
        if (!m_shadersLoaded || !g_clientMeshManager || (m_debugLayer >= 0 && m_debugLayer != 2)) {
            m_stats.translucentPassTimeMs = 0.0f;
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Order this frame's translucent quads back-to-front before drawing
        // them. Sorting SECTIONS (below) is not enough on its own — quads
        // WITHIN a section also have to be ordered, or a nearer surface can
        // write depth first and cut out everything behind it. See
        // mesh/TranslucentSort.hpp.
        ScheduleTranslucentSectionResort(camera.position);

        // Setup render state for translucent pass
        SetupRenderPass(m_translucentConfig);

        // Render directly from m_visibleSections in reverse (back-to-front for blending)
        RenderLayerPass(RenderLayer::Translucent, LayerTranslucent, true);

        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.translucentPassTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    void ChunkRenderer::RenderAll(const Camera& camera, const Frustum& frustum,
                                  const glm::mat4& projectionOverride) {
        m_useProjectionOverride = true;
        m_projectionOverride    = projectionOverride;
        RenderAll(camera, frustum);
        m_useProjectionOverride = false;
    }

    void ChunkRenderer::RenderAll(const Camera& camera, const Frustum& frustum) {
        PROFILE_ZONE;
        m_stats.Reset();

        // --- GPU pass timing (main scene only; portal re-entries skipped) ---
        // Poll last frame's queries first (non-blocking: -1 means still in
        // flight, keep waiting). A pass only starts a new query when its
        // previous one has been collected, so there is never more than one
        // in-flight query per pass and GL_TIME_ELAPSED brackets never nest.
        const bool mainScene = !m_useProjectionOverride && g_renderBackend != nullptr;
        if (mainScene) {
            // Always drain pending queries (even when timers are toggled off,
            // so outstanding query objects get collected and freed).
            PROFILE_ZONE_N("GpuTimerPoll");
            for (int i = 0; i < kGpuPassCount; ++i) {
                if (m_gpuTimerPending[i] == INVALID_GPU_TIMER) continue;
                float r = g_renderBackend->GetGPUTimerResultMs(m_gpuTimerPending[i]);
                if (r >= 0.0f) {
                    m_gpuPassResultMs[i] = r;
                    m_gpuTimerPending[i] = INVALID_GPU_TIMER;
                }
            }
        }
        const bool gpuTiming = mainScene && g_enableGpuPassTimers;
        auto beginPassTimer = [&](int pass, const char* name) -> GPUTimerHandle {
            if (!gpuTiming || m_gpuTimerPending[pass] != INVALID_GPU_TIMER)
                return INVALID_GPU_TIMER;
            PROFILE_ZONE_N("GpuTimerBegin");
            return g_renderBackend->BeginGPUTimer(name);
        };
        auto endPassTimer = [&](int pass, GPUTimerHandle t) {
            if (t == INVALID_GPU_TIMER) return;
            PROFILE_ZONE_N("GpuTimerEnd");
            g_renderBackend->EndGPUTimer(t);
            m_gpuTimerPending[pass] = t;
        };

        // Prepare visible sections ONCE at the beginning of the frame.
        // This includes chunk iteration, GPU data loading, and frustum culling.
        // Note: PrepareVisibleSections manages its own clearing — the visible section
        // cache may reuse last frame's list if the camera hasn't moved significantly.
        PrepareVisibleSections(camera, frustum);

        // Bind opaque shader, compute MVP, and bind atlas texture
        BindSharedRenderState(camera);

        // Bind shared block VAO once per frame — all mega-buffers share this
        // VAO's vertex format.  Switching between mega-buffers only calls
        // BindBuffers() (glBindVertexBuffer + IBO rebind), avoiding the GPU
        // pipeline flush that glBindVertexArray causes on macOS. Zoned because
        // that same flush behavior makes THIS bind a candidate collection
        // point for deferred driver work (e.g. this frame's buffer uploads).
        if (g_clientMeshManager) {
            PROFILE_ZONE_N("BindBlockVAO");
            g_clientMeshManager->BindSharedBlockVAO();
        }

        // Opaque pass: uses opaque shader (no discard → early-z enabled)
        {
            GPUTimerHandle t = beginPassTimer(0, "opaque");
            RenderOpaque(camera, frustum);
            endPassTimer(0, t);
        }

        // Switch to cutout shader for cutout + translucent passes (has discard)
        if (m_cutoutShader != INVALID_SHADER && g_renderBackend) {
            PROFILE_ZONE_N("PassShaderSwitch");
            m_activeShader = m_cutoutShader;
            g_renderBackend->BindShader(m_cutoutShader);
            g_renderBackend->SetUniformMat4(m_cutoutShader, "uMVP", m_cachedMVP);
            g_renderBackend->SetUniformVec4(m_cutoutShader, "uPortalClipPlane", s_portalClipPlane);
            SetEnvironmentUniforms(m_cutoutShader, camera);
            g_renderBackend->BindTexture(m_backendAtlasTexture, 0);
        }

        {
            GPUTimerHandle t = beginPassTimer(1, "cutout");
            RenderCutout(camera, frustum);
            endPassTimer(1, t);
        }

        // Switch to solid shader for translucent pass (no discard → early-z enabled,
        // blending handles transparency). This avoids the discard penalty entirely.
        if (m_solidShader != INVALID_SHADER && g_renderBackend) {
            PROFILE_ZONE_N("PassShaderSwitch");
            m_activeShader = m_solidShader;
            g_renderBackend->BindShader(m_solidShader);
            g_renderBackend->SetUniformMat4(m_solidShader, "uMVP", m_cachedMVP);
            g_renderBackend->SetUniformVec4(m_solidShader, "uPortalClipPlane", s_portalClipPlane);
            SetEnvironmentUniforms(m_solidShader, camera);
            g_renderBackend->BindTexture(m_backendAtlasTexture, 0);
        }

        {
            GPUTimerHandle t = beginPassTimer(2, "translucent");
            RenderTranslucent(camera, frustum);
            endPassTimer(2, t);
        }

        // Publish the most recent completed GPU readings (1-2 frame latency)
        m_stats.gpuOpaqueTimeMs = m_gpuPassResultMs[0];
        m_stats.gpuCutoutTimeMs = m_gpuPassResultMs[1];
        m_stats.gpuTranslucentTimeMs = m_gpuPassResultMs[2];
        m_stats.gpuTotalTimeMs = m_gpuPassResultMs[0] + m_gpuPassResultMs[1] + m_gpuPassResultMs[2];

        // Into the trace as well as F3. Vk.FenceWait + Vk.QueueSubmit tell us
        // how long the CPU waited on the GPU but not what the GPU was doing;
        // these three are what splits that wait into fill-bound vs vertex-bound.
        // Microseconds, because Tracy plots integers.
        PROFILE_PLOT("Gpu/OpaqueUs",      static_cast<int64_t>(m_gpuPassResultMs[0] * 1000.0f));
        PROFILE_PLOT("Gpu/CutoutUs",      static_cast<int64_t>(m_gpuPassResultMs[1] * 1000.0f));
        PROFILE_PLOT("Gpu/TranslucentUs", static_cast<int64_t>(m_gpuPassResultMs[2] * 1000.0f));

        // Calculate total render time as sum of all components
        m_stats.renderTimeMs = m_stats.buildDrawListsTimeMs + 
                               m_stats.opaquePassTimeMs + 
                               m_stats.cutoutPassTimeMs + 
                               m_stats.translucentPassTimeMs;

        // Render section bounds if enabled
        if (m_showSectionBounds) {
            RenderSectionBounds(camera, m_visibleSections);
        }

        RestoreRenderState();
    }

    void ChunkRenderer::SetWireframeMode(bool enable) {
        m_wireframeMode = enable;
        // Wireframe state is applied in SetupRenderPass via PipelineState
    }

    void ChunkRenderer::PrepareVisibleSections(const Camera& camera, const Frustum& frustum) {
        PROFILE_ZONE;
        auto overallStartTime = std::chrono::high_resolution_clock::now();

        // --- Reachable-section caching (async BFS occlusion graph) ---
        // The BFS result depends only on the camera's SECTION and world state,
        // so it is cached across frames in per-camera-section slots. The frustum
        // test is applied fresh every frame below — rotation never goes stale.
        // The BFS itself runs on a dedicated worker: when a slot is stale
        // (world changed) or missing (camera crossed a section), the main
        // thread snapshots the inputs, submits a job, and keeps rendering the
        // best available slot until the result lands 1-2 frames later. The
        // main thread only ever runs the BFS inline on cold start (world
        // entry / post-erase, when no usable slot exists at all).
        int currentChunkX = static_cast<int>(std::floor(camera.position.x / 16.0f));
        int currentChunkZ = static_cast<int>(std::floor(camera.position.z / 16.0f));
        int currentSectionY = static_cast<int>(std::floor((camera.position.y - Config::MinY) / 16.0f));

        m_prepareCounter++;

        // Erase safety: some GPUSectionData objects were destroyed. Every
        // cached list and any in-flight result may hold dangling pointers.
        if (m_sectionDataErased) {
            m_sectionDataErased = false;
            m_eraseToken++;
            for (auto& s : m_reachableSlots) s.valid = false;
            // The live graph's nodes are fine (they hold no pointers), but the
            // sections a partial update would emit come from a slot that no
            // longer exists, and its anchor token is now stale. Drop it.
            m_occlusionGraph.InvalidateGraph();
            m_visibleSectionsDirty = true;
        }

        // Drop slots that haven't been used in a while (~10 s). They no longer
        // pin anything — the lists hold identity only — but a slot this stale,
        // e.g. a portal view no longer rendered, describes a world that has
        // moved on and is cheap to rebuild if it is ever needed again.
        for (auto& s : m_reachableSlots) {
            if (s.valid && m_prepareCounter - s.lastUsed > 600) s.valid = false;
        }

        // Collect a finished async BFS result, if one is waiting.
        if (auto job = m_occlusionGraph.TryCollect()) {
            // Degenerate result: the snapshot never saw the camera's own
            // chunk (player crossed into a chunk the server stream hasn't
            // delivered yet), so the BFS seed couldn't spread and the empty
            // result says nothing about the world. Adopting it as the exact
            // slot would blank the whole frame ("sky flash") — drop it and
            // keep rendering the best stale slot; the refresh kick below
            // retries every frame until the chunk arrives.
            const bool degenerate = job->result.empty() && !job->centerLoaded;
            if (job->eraseToken == m_eraseToken && !degenerate) {
                // Store into the slot matching the job's camera section
                // (or an invalid/LRU slot).
                ReachableCacheSlot* dst = nullptr;
                for (auto& s : m_reachableSlots) {
                    if (s.valid && s.cx == job->keyCx && s.cz == job->keyCz && s.sy == job->keySy) {
                        dst = &s;
                        break;
                    }
                }
                if (!dst) {
                    dst = &m_reachableSlots[0];
                    for (auto& s : m_reachableSlots) {
                        if (!s.valid) { dst = &s; break; }
                        if (s.lastUsed < dst->lastUsed) dst = &s;
                    }
                }
                dst->cx = job->keyCx;
                dst->cz = job->keyCz;
                dst->sy = job->keySy;
                dst->sections.swap(job->result);
                dst->worldVersion = job->worldVersion;
                    dst->valid = true;
                dst->lastUsed = m_prepareCounter;
                m_bfsVisitedCount = job->visitedCount;
                m_bfsOccludedCount = job->occludedCount;

                // The finished grid becomes the live graph, so per-frame
                // partial updates can extend it instead of waiting for the
                // next full rebuild — MC currentGraph.set(newState). The slot
                // it applies to is found later by matching the graph's own
                // anchor, so nothing extra needs recording here.
                m_occlusionGraph.AdoptGraph(*job);
            }
            // Stale erase token: result holds dangling pointers — drop it.
            m_occlusionGraph.RecycleJob(std::move(job));
        }

        // World changed (mesh upload/unload, smart-cull toggle): existing
        // results are stale (but pointer-safe) — they trigger a refresh below.
        if (m_visibleSectionsDirty) {
            m_visibleSectionsDirty = false;
            m_worldVersion++;
        }

        // Effective render distance (needed by both sync and async paths)
        int renderDistanceChunks = Platform::g_gameSettings.GetRenderDistance();
        if (Client::g_networkClient && Client::g_networkClient->GetServerViewDistance() > 0) {
            renderDistanceChunks = std::min(renderDistanceChunks, Client::g_networkClient->GetServerViewDistance());
        }

        // Pick the render source: exact-key slot if we have one, else the most
        // recently used valid slot (approximately right for a frame or two
        // while the async rebuild for the new section is in flight).
        ReachableCacheSlot* exact = nullptr;
        for (auto& s : m_reachableSlots) {
            if (s.valid && s.cx == currentChunkX && s.cz == currentChunkZ && s.sy == currentSectionY) {
                exact = &s;
                break;
            }
        }
        ReachableCacheSlot* usable = exact;
        if (!usable) {
            for (auto& s : m_reachableSlots) {
                if (s.valid && (!usable || s.lastUsed > usable->lastUsed)) usable = &s;
            }
        }

        if (!usable) {
            // Cold start (world entry / post-erase): synchronous rebuild so
            // this frame renders correct data.
            if (!g_clientMeshManager) {
                m_visibleSections.clear();
                m_stats.buildDrawListsTimeMs = 0.0f;
                return;
            }
            ReachableCacheSlot* dst = &m_reachableSlots[0];
            for (auto& s : m_reachableSlots) {
                if (!s.valid) { dst = &s; break; }
                if (s.lastUsed < dst->lastUsed) dst = &s;
            }

            auto job = m_occlusionGraph.AcquireJob();
            job->keyCx = currentChunkX;
            job->keyCz = currentChunkZ;
            job->keySy = currentSectionY;
            job->worldVersion = m_worldVersion;
            job->eraseToken = m_eraseToken;

            auto iterationStart = std::chrono::high_resolution_clock::now();
            m_occlusionGraph.BuildInput(*job, camera.position, m_enableSmartCull, renderDistanceChunks);
            m_occlusionGraph.RunSync(*job);
            auto iterationEnd = std::chrono::high_resolution_clock::now();
            m_stats.chunkIterationTimeMs = std::chrono::duration<float, std::milli>(iterationEnd - iterationStart).count();
            m_stats.sortingTimeMs = 0.0f;  // Sorted inside the BFS run

            dst->cx = currentChunkX;
            dst->cz = currentChunkZ;
            dst->sy = currentSectionY;
            dst->sections.swap(job->result);
            // A degenerate cold-start result (camera chunk not streamed in
            // yet) renders as empty this frame — that's honest, nothing
            // better exists — but must stay marked stale so the async
            // refresh keeps retrying until real data exists.
            const bool syncDegenerate = dst->sections.empty() && !job->centerLoaded;
            dst->worldVersion = syncDegenerate ? job->worldVersion - 1 : job->worldVersion;
            dst->valid = true;
            m_bfsVisitedCount = job->visitedCount;
            m_bfsOccludedCount = job->occludedCount;
            m_occlusionGraph.AdoptGraph(*job);   // cold-start graph, same as the async path
            m_occlusionGraph.RecycleJob(std::move(job));
            // Counted alongside the async site below — this branch leaves the
            // slot fresh, so the two are mutually exclusive within a frame.
            PROFILE_PLOT("Bfs/FullRebuilds", static_cast<int64_t>(1));

            usable = dst;
            exact = dst;
        } else {
            m_stats.chunkIterationTimeMs = 0.0f;
            m_stats.sortingTimeMs = 0.0f;
        }
        m_stats.gpuDataLoadTimeMs = 0.0f;
        usable->lastUsed = m_prepareCounter;

        // ── Incremental graph update, MC LevelRenderer.cullTerrain -> updateSOG
        // -> runPartialUpdate. Runs EVERY frame on the main thread, unlike the
        // full rebuild, and only extends the slot the live graph is anchored
        // to. Sections that finished meshing since last frame become reachable
        // now instead of after the next full rebuild (which lands ~50-90x/s
        // with 1-2 frames of collect latency on top).
        //
        // It only ever appends, so it cannot empty the list — the sky-flash
        // failure mode stays governed by the centerLoaded/blind guards above.
        if (m_occlusionGraph.HasGraphFor(usable->cx, usable->cz, usable->sy,
                                         renderDistanceChunks, m_eraseToken)) {
            if (m_occlusionGraph.RunPartialUpdate(camera.position, usable->cx, usable->cz,
                                                  usable->sy, renderDistanceChunks,
                                                  m_eraseToken, usable->sections)) {
                // Appended entries land after the front-to-back sorted body.
                // Re-sorting every frame would cost more than the ordering is
                // worth (it is an early-z hint, not correctness), and the next
                // full rebuild restores it — but the translucent pass reads
                // this list in reverse, so leaving it wrong is not free either.
                std::sort(usable->sections.begin(), usable->sections.end(),
                          [](const SectionRenderData& a, const SectionRenderData& b) {
                              return a.distanceToCamera < b.distanceToCamera;
                          });
            }
            // Deliberately NOT touching m_visibleSectionsDirty or the slot's
            // worldVersion. Setting either would bump m_worldVersion and kick a
            // FULL rebuild next frame — the opposite of the point. The frustum
            // filter below re-runs unconditionally every frame, so appended
            // sections are picked up with no flag at all.
            //
            // Leaving the slot marked stale also means the full rebuild still
            // runs on its own schedule and stays authoritative. Partial updates
            // are permissive (see RunPartialUpdate) and never remove anything,
            // so they must not be allowed to suppress the pass that does.
        }

        // Kick an async refresh when the current view's data is missing or
        // stale and the worker is idle (one job in flight at a time — no
        // queue buildup, newest state wins).
        const bool haveExactFresh = exact && exact->worldVersion == m_worldVersion;
        bool startedFullRebuild = false;
        if (!haveExactFresh && g_clientMeshManager && !m_occlusionGraph.Busy()) {
            auto job = m_occlusionGraph.AcquireJob();
            job->keyCx = currentChunkX;
            job->keyCz = currentChunkZ;
            job->keySy = currentSectionY;
            job->worldVersion = m_worldVersion;
            job->eraseToken = m_eraseToken;
            m_occlusionGraph.BuildInput(*job, camera.position, m_enableSmartCull, renderDistanceChunks);
            m_occlusionGraph.SubmitAsync(std::move(job));
            startedFullRebuild = true;
        }
        // Full rebuilds per frame. MC only invalidates on an 8-block camera move
        // or needsUpdate(); anything above ~0 while standing still means
        // something is forcing rebuilds that propagation should be handling.
        PROFILE_PLOT("Bfs/FullRebuilds", static_cast<int64_t>(startedFullRebuild ? 1 : 0));

        ReachableCacheSlot* slot = usable;

        // --- Per-frame frustum filter over the cached reachable set ---
        auto cullStart = std::chrono::high_resolution_clock::now();
        {
            PROFILE_ZONE_N("FrustumFilter");
            m_visibleSections.clear();

            // MC tags `isClose` during the same traversal that builds the
            // visible list (SectionOcclusionGraph:80-89), so the flag costs
            // nothing extra and is always current for THIS camera position.
            auto tagNearby = [&](SectionRenderData& rd, float minX, float minY, float minZ) {
                const float dx = (minX + 8.0f) - camera.position.x;
                const float dy = (minY + 8.0f) - camera.position.y;
                const float dz = (minZ + 8.0f) - camera.position.z;
                rd.nearby = (dx * dx + dy * dy + dz * dz) < kTranslucentNearRadiusSq;
            };

            // LIVE RESOLUTION — MC LevelRenderer.prepareChunkRenders:1003,
            // `SectionMesh sectionMesh = section.getSectionMesh();` fetched
            // fresh for every section every frame.
            //
            // The reachable list carries identity only, so this is the single
            // point where a section's current GPU data is bound, and it is bound
            // for exactly one RenderAll. A section that has been remeshed away,
            // emptied or unloaded resolves to null and contributes nothing —
            // the same silent skip MC gets from getBuffers(layer) == null.
            //
            // Uses the CONST GetSectionInfo deliberately: the non-const overload
            // calls UpdateAccessTime(), a steady_clock::now() plus a store, per
            // section per frame, for a field nothing reads.
            //
            // Do NOT switch this to ClientMeshManager::GetSectionGPUData — that
            // takes shared_lock(m_gpuDataMutex), and UploadMeshResultToGPU holds
            // that mutex uniquely while calling back into this renderer.
            const auto* ccm = Client::g_clientChunkManager.get();
            [[maybe_unused]] int64_t deadEntries = 0;
            auto resolveLive = [&](SectionRenderData& s) {
                const Client::SectionInfo* si =
                    ccm ? ccm->GetSectionInfo(s.chunkPos, s.sectionY) : nullptr;
                s.resolved = si ? si->gpuData.load(std::memory_order_acquire) : nullptr;
                if (!s.resolved) ++deadEntries;
            };

            if (m_enableFrustumCulling) {
                for (const auto& section : slot->sections) {
                    float minX = static_cast<float>(section.chunkPos.x * 16);
                    float minY = static_cast<float>(section.sectionY * 16 + Config::MinY);
                    float minZ = static_cast<float>(section.chunkPos.z * 16);
                    if (frustum.IsBoxVisible(glm::vec3(minX, minY, minZ),
                                             glm::vec3(minX + 16.0f, minY + 16.0f, minZ + 16.0f))) {
                        m_visibleSections.push_back(section);
                        tagNearby(m_visibleSections.back(), minX, minY, minZ);
                        resolveLive(m_visibleSections.back());
                    }
                }
            } else {
                m_visibleSections = slot->sections;
                for (auto& rd : m_visibleSections) {
                    tagNearby(rd,
                              static_cast<float>(rd.chunkPos.x * 16),
                              static_cast<float>(rd.sectionY * 16 + Config::MinY),
                              static_cast<float>(rd.chunkPos.z * 16));
                    resolveLive(rd);
                }
            }
            PROFILE_PLOT("Sections/Dead", deadEntries);

            // MAIN-VIEW SNAPSHOT for the mesh scheduler.
            //
            // m_visibleSections is overwritten by every re-entry into
            // PrepareVisibleSections, and the portal see-through pass re-enters
            // it once per recursion level with a VIRTUAL camera. A scheduler
            // reading m_visibleSections directly would therefore mesh whatever
            // the last portal recursion happened to see rather than what the
            // player is looking at — a correctness bug, not a latency one.
            //
            // m_useProjectionOverride is set only by the portal renderer, so it
            // is the exact discriminator for "this is the real view".
            if (!m_useProjectionOverride) {
                m_mainViewSections = m_visibleSections;
            }
        }
        auto cullEnd = std::chrono::high_resolution_clock::now();
        m_stats.frustumCullingTimeMs = std::chrono::duration<float, std::milli>(cullEnd - cullStart).count();

        // Reachable = survived the occlusion BFS; Visible = also survived the
        // frustum. Visible is the denominator for the translucency re-sort
        // budget, so a spike here shows up in Resort/Considered next frame.
        PROFILE_PLOT("Sections/Reachable", static_cast<int64_t>(slot->sections.size()));
        PROFILE_PLOT("Sections/Visible", static_cast<int64_t>(m_visibleSections.size()));

        m_stats.sectionsRendered = static_cast<int>(m_visibleSections.size());
        m_stats.sectionsSkipped = m_bfsOccludedCount;
        m_stats.sectionsAvailable = m_bfsVisitedCount;

        auto overallEndTime = std::chrono::high_resolution_clock::now();
        m_stats.buildDrawListsTimeMs = std::chrono::duration<float, std::milli>(overallEndTime - overallStartTime).count();
    }

    // Port of LevelRenderer.scheduleTranslucentSectionResort (LevelRenderer.java:953).
    //
    // Two phases, both over the VISIBLE list — never over every loaded section.
    // MC keeps two lists (visibleSections / nearbyVisibleSections) because its
    // octree visitor fills them both in one pass; our frustum filter tags a
    // `nearby` bit on the single list instead, which has the same semantics.
    // MC's phase 2 also walks the full visible list including the nearby ones,
    // so a section can be visited twice in a frame — harmless, because the
    // second visit finds the point of view already up to date and no-ops.
    void ChunkRenderer::ScheduleTranslucentSectionResort(const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("ResortTranslucent");
        if (m_visibleSections.empty() || !g_clientMeshManager) return;

        const glm::ivec3 cameraBlock(static_cast<int>(std::floor(cameraPos.x)),
                                     static_cast<int>(std::floor(cameraPos.y)),
                                     static_cast<int>(std::floor(cameraPos.z)));
        const bool blockPosChanged = cameraBlock != m_lastTranslucentSortBlockPos;
        m_lastTranslucentSortBlockPos = cameraBlock;

        // Profiling counters only — PROFILE_PLOT compiles out without Tracy.
        [[maybe_unused]] int64_t considered = 0, uploaded = 0;
        auto visit = [&](const SectionRenderData& s, bool isNearby) {
            // The visible list now holds every reachable non-air section,
            // meshed or not (MC runUpdates:253). Unmeshed ones have no
            // translucent quads to re-sort, and skipping them BEFORE the
            // counter is what keeps Resort/Considered at ~nearby+15 instead of
            // thousands — each one would otherwise cost an m_gpuData.find.
            if (!s.resolved) return;
            ++considered;
            if (g_clientMeshManager->ResortTranslucentSection(
                    s.chunkPos, s.sectionY, cameraPos, blockPosChanged, isNearby)) {
                ++uploaded;
            }
        };

        // Phase 1 — every nearby visible section, every frame. These are the
        // ones where a single block of camera movement visibly reorders quads.
        for (const auto& section : m_visibleSections) {
            if (section.nearby) visit(section, true);
        }

        // Phase 2 — a rotating slice of the visible list. The budget is
        // max(visible/8, 15) against the VISIBLE count, which is what bounds
        // the per-frame cost; the cursor persists across frames so the whole
        // list is covered over time instead of just its first N entries.
        const size_t visibleCount = m_visibleSections.size();
        m_translucencyResortIndex %= visibleCount;
        int resortsLeft = std::max<int>(static_cast<int>(visibleCount) / 8, 15);
        while (resortsLeft-- > 0) {
            const size_t index = m_translucencyResortIndex++ % visibleCount;
            visit(m_visibleSections[index], false);
        }

        // Considered = sections the policy looked at (should be ~nearby + 15,
        // NOT thousands). Uploaded = of those, how many actually re-sorted and
        // wrote a new index range — that number feeds Upload/Bytes too.
        PROFILE_PLOT("Resort/Considered", considered);
        PROFILE_PLOT("Resort/Uploaded", uploaded);
    }

    void ChunkRenderer::SetEnvironmentUniforms(ShaderHandle shader, const Camera& camera) {
        // Day/night terrain dim + MC-style distance fog, from the per-frame
        // EnvironmentState. uCameraPos must be the CURRENT view's camera —
        // portal views re-enter here with their own virtual camera, so fog
        // stays consistent through portals.
        const EnvironmentFrame& env = EnvironmentState::Get().Frame();
        g_renderBackend->SetUniformVec3(shader, "uCameraPos", camera.position);
        g_renderBackend->SetUniformFloat(shader, "uSkyBrightness", env.skyBrightness);
        g_renderBackend->SetUniformVec4(shader, "uFogColor", glm::vec4(env.fogColor, 1.0f));
        g_renderBackend->SetUniformVec4(shader, "uFogEnv",
            glm::vec4(env.fogEnvStart, env.fogEnvEnd, env.fogRdStart, env.fogRdEnd));
    }

    void ChunkRenderer::BindSharedRenderState(const Camera& camera) {
        PROFILE_ZONE;
        if (m_visibleSections.empty() || !g_renderBackend || m_opaqueShader == INVALID_SHADER) {
            return;
        }

        // Bind opaque shader (no discard → GPU can use early-z)
        m_activeShader = m_opaqueShader;
        g_renderBackend->BindShader(m_opaqueShader);

        // Compute MVP once and cache it (needed when switching to cutout shader)
        int width, height;
        glfwGetFramebufferSize(g_renderBackend->GetWindow(), &width, &height);
        float aspect = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);
        glm::mat4 view = camera.GetViewMatrix();
        int effectiveRenderDist = Platform::g_gameSettings.GetRenderDistance();
        if (Client::g_networkClient && Client::g_networkClient->GetServerViewDistance() > 0) {
            effectiveRenderDist = std::min(effectiveRenderDist, Client::g_networkClient->GetServerViewDistance());
        }
        float farPlane = static_cast<float>(effectiveRenderDist) * 16.0f * 4.0f;
        // Use the caller-supplied projection (oblique-near-plane from the
        // portal renderer) when present; otherwise build the standard one.
        //
        // Vulkan special-case: skip the oblique override. The Vulkan
        // backend's GL→VK depth remap (kVkZCorrect in VKBackend.cpp)
        // interacts with the Lengyel oblique modification such that
        // kept-side geometry past the clip plane's asymptote ends up at
        // z_ndc > 1 in Vulkan NDC, getting clipped by the rasterizer's
        // far plane and producing the "stand 5+ blocks from a portal
        // and the see-through view collapses to sky color" bug. On
        // Vulkan we instead rely on block_vk.vert's gl_ClipDistance[0]
        // write (using the world-space uPortalClipPlane uniform set by
        // PortalRenderer::SetPortalClipPlane) to clip at the dst plane —
        // same end result as the oblique math, no depth compression.
        const bool useOverride = m_useProjectionOverride && g_renderBackend &&
                                 g_renderBackend->GetType() != BackendType::Vulkan;
        const glm::mat4 proj = useOverride
            ? m_projectionOverride
            : glm::perspective(glm::radians(camera.fov), aspect, 0.05f, farPlane);
        m_cachedMVP = proj * view;
        g_renderBackend->SetUniformMat4(m_opaqueShader, "uMVP", m_cachedMVP);
        g_renderBackend->SetUniformVec4(m_opaqueShader, "uPortalClipPlane", s_portalClipPlane);
        SetEnvironmentUniforms(m_opaqueShader, camera);

        // Fetch and bind atlas texture once (fresh handle in case atlas was rebuilt)
        if (g_atlasBuilder) {
            m_backendAtlasTexture = g_atlasBuilder->GetBackendTextureHandle();
        }
        if (m_backendAtlasTexture != INVALID_TEXTURE) {
            g_renderBackend->BindTexture(m_backendAtlasTexture, 0);
        }
    }

    void ChunkRenderer::RenderLayerPass(RenderLayer layer, uint8_t layerBit, bool reverseOrder) {
        if (m_visibleSections.empty() || !g_renderBackend || m_activeShader == INVALID_SHADER) {
            return;
        }

        // Set alpha discard threshold per pass:
        //   Opaque: 0.1 (discard overlay transparency like grass sides)
        //   Cutout: 0.5 (standard alpha test for leaves, flowers)
        //   Translucent: 0.01 (only fully invisible pixels, rest is blended)
        float alphaTest = 0.1f;
        if (layer == RenderLayer::Cutout) alphaTest = 0.5f;
        else if (layer == RenderLayer::Translucent) alphaTest = 0.01f;
        g_renderBackend->SetUniformFloat(m_activeShader, "uAlphaTest", alphaTest);

        // Get the mega-buffer for this layer
        auto* megaBuffer = g_clientMeshManager ? g_clientMeshManager->GetMegaBuffer(layer) : nullptr;
        if (!megaBuffer || !megaBuffer->IsInitialized()) return;

        // Build multi-draw command arrays from visible sections
        uint32_t slabCount = megaBuffer->GetSlabCount();

        // Resize per-slab draw command vectors (reuse allocations across frames)
        m_perSlabCounts.resize(slabCount);
        m_perSlabOffsets.resize(slabCount);
        m_perSlabBaseVertices.resize(slabCount);
        const bool perSectionIbos = megaBuffer->UsesPerSectionIndexBuffers();
        if (perSectionIbos) m_perSlabIbos.resize(slabCount);
        for (uint32_t s = 0; s < slabCount; s++) {
            m_perSlabCounts[s].clear();
            m_perSlabOffsets[s].clear();
            m_perSlabBaseVertices[s].clear();
            if (perSectionIbos) m_perSlabIbos[s].clear();
        }

        int layerCount = 0;
        uint32_t totalVerts = 0, totalIndices = 0;

        // Group visible sections by slab index for per-slab multi-draw.
        // Zone split: "BuildDrawList" is OUR loop over visible sections;
        // "SubmitMultiDraw" is time spent inside GL driver calls. If a pass
        // shows milliseconds, this tells you which side owns them.
        {
            PROFILE_ZONE_N("BuildDrawList");
            auto processSection = [&](const SectionRenderData& section) {
                // `resolved` was bound this frame by the frustum filter. Null
                // means the section has no mesh right now — never meshed, or
                // remeshed to empty, or unloaded — and it silently contributes
                // nothing, exactly as MC's null getBuffers(layer) does.
                if (!section.resolved) return;

                // The layerMask pre-filter is gone: it was derived from vertex
                // counts and duplicated what `valid && indexCount > 0` below
                // already decides, but from a snapshot that could disagree with
                // the draw command it was guarding.
                const auto& cachedCmd = (layer == RenderLayer::Opaque)      ? section.resolved->opaqueDrawCmd :
                                        (layer == RenderLayer::Cutout)       ? section.resolved->cutoutDrawCmd :
                                                                               section.resolved->translucentDrawCmd;
                if (cachedCmd.valid && cachedCmd.indexCount > 0 && cachedCmd.slabIndex < slabCount) {
                    m_perSlabCounts[cachedCmd.slabIndex].push_back(cachedCmd.indexCount);
                    m_perSlabOffsets[cachedCmd.slabIndex].push_back(cachedCmd.indexByteOffset);
                    m_perSlabBaseVertices[cachedCmd.slabIndex].push_back(cachedCmd.baseVertex);
                    if (perSectionIbos) m_perSlabIbos[cachedCmd.slabIndex].push_back(cachedCmd.ibo);
                    layerCount++;

                    totalIndices += cachedCmd.indexCount;
                    switch (layer) {
                        case RenderLayer::Opaque:      totalVerts += section.resolved->opaqueVertexCount; break;
                        case RenderLayer::Cutout:       totalVerts += section.resolved->cutoutVertexCount; break;
                        case RenderLayer::Translucent:  totalVerts += section.resolved->translucentVertexCount; break;
                    }
                }
            };

            if (reverseOrder) {
                for (auto it = m_visibleSections.rbegin(); it != m_visibleSections.rend(); ++it)
                    processSection(*it);
            } else {
                for (const auto& section : m_visibleSections)
                    processSection(section);
            }
        }

        // Issue one multi-draw per slab that has sections
        {
            PROFILE_ZONE_N("SubmitMultiDraw");
            for (uint32_t s = 0; s < slabCount; s++) {
                if (m_perSlabCounts[s].empty()) continue;
                megaBuffer->BindSlab(s);   // binds the slab VBO; IBO only when shared

                if (perSectionIbos) {
                    // MC's layout: each section owns its index buffer, so this
                    // layer cannot be one multi-draw. One bind + draw per section
                    // is the price of never writing into an IBO that has draws in
                    // flight — see ChunkMegaBuffer::Initialize.
                    const size_t n = m_perSlabCounts[s].size();
                    for (size_t i = 0; i < n; ++i) {
                        const BufferHandle ibo = m_perSlabIbos[s][i];
                        if (ibo == INVALID_BUFFER) continue;
                        g_renderBackend->BindIndexBuffer(ibo);
                        g_renderBackend->DrawIndexedBaseVertex(
                            static_cast<uint32_t>(m_perSlabCounts[s][i]),
                            0,                              // own buffer: always at 0
                            m_perSlabBaseVertices[s][i],
                            IndexType::Uint16
                        );
                        m_stats.totalDrawCalls++;
                    }
                } else {
                    g_renderBackend->MultiDrawIndexedBaseVertex(
                        m_perSlabCounts[s].data(),
                        m_perSlabOffsets[s].data(),
                        m_perSlabBaseVertices[s].data(),
                        static_cast<uint32_t>(m_perSlabCounts[s].size()),
                        IndexType::Uint16
                    );
                    m_stats.totalDrawCalls++;
                }
            }
        }

        m_stats.totalVerticesRendered += totalVerts;
        m_stats.totalIndicesRendered += totalIndices;

        // Sub-draws issued for this layer — one per section, since each has its
        // own baseVertex. This is the number that drives Vk.QueueSubmit: MoltenVK
        // defers translation and encodes every recorded vkCmd* into Metal inside
        // vkQueueSubmit, so submit time scales with commands recorded, not with
        // GPU work. Plotted so the two can be correlated directly in Tracy —
        // a flat ratio between them means the per-command translation cost is
        // the ceiling, and the only lever is issuing fewer draws.
        PROFILE_PLOT("Draws/Chunk", static_cast<int64_t>(layerCount));

        switch (layer) {
            case RenderLayer::Opaque:      m_stats.opaqueSections = layerCount; break;
            case RenderLayer::Cutout:       m_stats.cutoutSections = layerCount; break;
            case RenderLayer::Translucent:  m_stats.translucentSections = layerCount; break;
        }
    }

    // RenderSectionLayer is no longer needed (mega-buffer multi-draw replaces it),
    // but kept as a no-op stub since the declaration exists in the header.
    void ChunkRenderer::RenderSectionLayer(const SectionRenderData& section, RenderLayer layer) {
        // No-op: rendering now handled by glMultiDrawElementsBaseVertex in RenderLayerPass
        (void)section;
        (void)layer;
    }

    void ChunkRenderer::SortSections(const Camera& camera, std::vector<SectionRenderData>& sections, bool frontToBack) {
        auto startTime = std::chrono::high_resolution_clock::now();

        if (frontToBack) {
            // Sort front to back (nearest first) for depth buffer optimization
            std::sort(sections.begin(), sections.end(),
                     [](const SectionRenderData& a, const SectionRenderData& b) {
                         return a.distanceToCamera < b.distanceToCamera;
                     });
        } else {
            // Sort back to front (farthest first) for correct alpha blending
            std::sort(sections.begin(), sections.end(),
                     [](const SectionRenderData& a, const SectionRenderData& b) {
                         return a.distanceToCamera > b.distanceToCamera;
                     });
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        float sortTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        m_stats.sortingTimeMs += sortTime;
    }

    // Utility methods still needed by the new optimized rendering system
    void ChunkRenderer::SetupRenderPass(const RenderPassConfig& config) {
        if (!g_renderBackend) return;

        PipelineState state;
        state.depthTestEnabled = config.enableDepthTest;
        state.depthWriteEnabled = config.enableDepthWrite;
        state.depthCompareOp = CompareOp::LessEqual;
        state.blendEnabled = config.enableBlending;
        state.srcBlendFactor = config.blendSrc;
        state.dstBlendFactor = config.blendDst;
        state.cullMode = config.enableBackFaceCulling ? CullMode::Back : CullMode::None;
        state.frontFace = FrontFace::CounterClockwise;
        state.polygonMode = m_wireframeMode ? PolygonMode::Line : PolygonMode::Fill;
        g_renderBackend->SetPipelineState(state);
    }

    void ChunkRenderer::RestoreRenderState() {
        if (!g_renderBackend) return;

        PipelineState defaultState;
        defaultState.depthTestEnabled = true;
        defaultState.depthWriteEnabled = true;
        defaultState.depthCompareOp = CompareOp::LessEqual;
        defaultState.blendEnabled = false;
        defaultState.cullMode = CullMode::Back;
        defaultState.frontFace = FrontFace::CounterClockwise;
        defaultState.polygonMode = PolygonMode::Fill;
        g_renderBackend->SetPipelineState(defaultState);
    }

    float ChunkRenderer::CalculateSectionDistance(const Camera& camera, ::Game::Math::ChunkPos chunkPos, int sectionY) {
        // Calculate squared distance from camera to section center.
        // Squared distance preserves sort order and avoids ~7000 sqrt calls/frame.
        float sectionCenterX = chunkPos.x * ::Game::Math::CHUNK_SIZE_X + ::Game::Math::CHUNK_SIZE_X * 0.5f;
        float sectionCenterY = sectionY * ::Game::Math::SECTION_HEIGHT + ::Game::Math::SECTION_HEIGHT * 0.5f + Config::MinY;
        float sectionCenterZ = chunkPos.z * ::Game::Math::CHUNK_SIZE_Z + ::Game::Math::CHUNK_SIZE_Z * 0.5f;

        float dx = sectionCenterX - camera.position.x;
        float dy = sectionCenterY - camera.position.y;
        float dz = sectionCenterZ - camera.position.z;

        // Squared distance: XZ is primary, Y is de-weighted (same relative
        // importance as the old linear formula, just squared throughout).
        return (dx * dx + dz * dz) + (dy * dy * 0.01f);
    }

    bool ChunkRenderer::IsSectionInFrustum(const Frustum& frustum, ::Game::Math::ChunkPos chunkPos, int sectionY) {
        AABB sectionAABB = GetSectionAABB(chunkPos, sectionY);
        return frustum.IsBoxVisible(sectionAABB);
    }

    AABB ChunkRenderer::GetSectionAABB(::Game::Math::ChunkPos chunkPos, int sectionY) {
        float minX = chunkPos.x * ::Game::Math::CHUNK_SIZE_X;
        float maxX = minX + ::Game::Math::CHUNK_SIZE_X;
        float minY = sectionY * ::Game::Math::SECTION_HEIGHT + Config::MinY;
        float maxY = minY + ::Game::Math::SECTION_HEIGHT;
        float minZ = chunkPos.z * ::Game::Math::CHUNK_SIZE_Z;
        float maxZ = minZ + ::Game::Math::CHUNK_SIZE_Z;

        AABB aabb;
        aabb.min = glm::vec3(minX, minY, minZ);
        aabb.max = glm::vec3(maxX, maxY, maxZ);
        return aabb;
    }

    void ChunkRenderer::RenderSectionBounds(const Camera& camera, const std::vector<SectionRenderData>& sections) {
        // TODO: Implement section bounds rendering for debugging
        // This would draw wireframe boxes around each section
    }

    bool ChunkRenderer::CheckShaderErrors(const std::string& pass) {
        // Error checking is handled internally by each backend
        return true;
    }

    // Global utility functions
    bool InitializeChunkRenderer() {
        if (g_chunkRenderer) {
            Log::Warning("ChunkRenderer already initialized");
            return true;
        }

        g_chunkRenderer = std::make_unique<ChunkRenderer>();
        return g_chunkRenderer->Initialize();
    }

    void ShutdownChunkRenderer() {
        if (g_chunkRenderer) {
            g_chunkRenderer->Shutdown();
            g_chunkRenderer.reset();
        }
    }

    void RenderChunksAll(const Camera& camera, const Frustum& frustum) {
        if (g_chunkRenderer) {
            g_chunkRenderer->RenderAll(camera, frustum);
        }
    }

    void RenderChunksAll(const Camera& camera, const Frustum& frustum,
                         const glm::mat4& projectionOverride) {
        if (g_chunkRenderer) {
            g_chunkRenderer->RenderAll(camera, frustum, projectionOverride);
        }
    }
    
    const RenderStats* GetChunkRendererStats() {
        if (g_chunkRenderer) {
            return &g_chunkRenderer->GetStats();
        }
        return nullptr;
    }

} // namespace Render
