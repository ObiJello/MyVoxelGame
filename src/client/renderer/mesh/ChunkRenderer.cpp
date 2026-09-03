// File: src/client/renderer/mesh/ChunkRenderer.cpp
#include "ChunkRenderer.hpp"
#include "../core/DevRenderSkip.hpp"
#include <cstdlib>
#include "ChunkMegaBuffer.hpp"
#include "Mesher.hpp"
#include "ClientMeshManager.hpp"
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
#include "client/input/Input.hpp"
#include <set>
#include <tuple>
#include "../../world/ClientChunkManager.hpp"
#include "common/world/math/ChunkViewDistance.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>

namespace Render {

    // Bound-level pointer, owned by ClientLevel (see ClientLevel.hpp).
    ChunkRenderer* g_chunkRenderer = nullptr;

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
    float     ChunkRenderer::s_portalEntityClipMargin = 0.0f;
    float     ChunkRenderer::s_nearPlane = 0.05f;

    // MC's "close" radius for translucency re-sorting — the 32 handed to
    // SectionTree.visitNodes in SectionOcclusionGraph.addSectionsInFrustum:89.
    // Sections inside it are re-checked every frame; the rest ride the sweep.
    static constexpr float kTranslucentNearRadius   = 32.0f;
    static constexpr float kTranslucentNearRadiusSq = kTranslucentNearRadius * kTranslucentNearRadius;

    // Dev A/B switch, same family as OBEY_SKIP (core/DevRenderSkip.hpp):
    //   OBEY_NO_DRAW_MERGE=1 ./MyVoxelGame ...
    // issues one sub-draw per section as before, so the merge's effect on
    // frame time / Vk.QueueSubmit can be measured by subtraction. Read once.
    static bool DrawMergeDisabled() {
        static const bool disabled = [] {
            const char* env = std::getenv("OBEY_NO_DRAW_MERGE");
            const bool on = env && *env && std::strcmp(env, "0") != 0;
            if (on) Log::Info("[DevSkip] OBEY_NO_DRAW_MERGE set: terrain draw-run merging disabled");
            return on;
        }();
        return disabled;
    }

    ChunkRenderer::ChunkRenderer() {
        SetupRenderConfigs();
        for (auto& slot : m_reachableSlots) slot.sections.reserve(2048);
        m_visibleSections.reserve(2048);
        m_drawEntries.reserve(4096);
        m_runCounts.reserve(1024);
        m_runByteOffsets.reserve(1024);
        m_zeroBaseVertices.resize(1024, 0);
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

    bool ChunkRenderer::Initialize(Client::ClientChunkManager* chunks, ClientMeshManager* meshes) {
        Log::Info("Initializing ChunkRenderer...");

        if (!g_renderBackend) {
            Log::Error("Cannot initialize ChunkRenderer: no render backend");
            return false;
        }
        if (!chunks || !meshes) {
            Log::Error("Cannot initialize ChunkRenderer: missing chunk or mesh manager");
            return false;
        }
        m_chunks = chunks;
        m_meshes = meshes;
        m_occlusionGraph.SetChunkManager(chunks);

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
        // Terrain uses its OWN shader files (terrain.* — the block.* shaders
        // plus the greedy-meshing tile-rect attribute at location 3). They are
        // deliberately separate from block.vert/block.frag, which several
        // entity renderers share with 24-byte vertex buffers: on Vulkan a
        // shader that consumes attribute 3 is invalid against a pipeline whose
        // vertex input doesn't provide it, so the shared files must not grow
        // the attribute.
        m_opaqueShader = createBlockShader("shaders/terrain.vert", "shaders/terrain_opaque.frag");
        if (m_opaqueShader == INVALID_SHADER) {
            Log::Error("Failed to create opaque block shader");
            return false;
        }
        m_cutoutShader = createBlockShader("shaders/terrain.vert", "shaders/terrain_cutout.frag");
        if (m_cutoutShader == INVALID_SHADER) {
            Log::Error("Failed to create cutout block shader");
            return false;
        }
        m_solidShader = createBlockShader("shaders/terrain.vert", "shaders/terrain_solid.frag");
        if (m_solidShader == INVALID_SHADER) {
            Log::Error("Failed to create solid block shader");
            return false;
        }
#ifdef HAS_VULKAN
        // Vulkan bakes the vertex input into each shader's pipelines; without
        // a registered layout it falls back to the 24-byte block layout and
        // the 32-byte terrain buffers would be read misaligned. Must happen
        // before the first draw (pipelines are created lazily). OpenGL needs
        // nothing here — the shared terrain VAO (SetupBlockVertexFormat)
        // carries the format.
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            vk->RegisterShaderVertexLayout(m_opaqueShader, GetTerrainVertexLayout());
            vk->RegisterShaderVertexLayout(m_cutoutShader, GetTerrainVertexLayout());
            vk->RegisterShaderVertexLayout(m_solidShader,  GetTerrainVertexLayout());
        }
#endif
        m_backendShader = m_opaqueShader;
        m_shadersLoaded = true;
        Log::Info("Block shaders created (opaque + cutout + solid)");
        if (std::getenv("OBEY_GREEDY_DEBUG")) {           // harness/screenshot use
            m_greedyMeshDebug = true;
            Mesher::SetGreedyDebugColors(true);           // before the first mesh builds
        }

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
            if (m_whiteDebugTexture != INVALID_TEXTURE) {
                g_renderBackend->DestroyTexture(m_whiteDebugTexture);
                m_whiteDebugTexture = INVALID_TEXTURE;
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
        if (!m_shadersLoaded || !m_meshes || (m_debugLayer >= 0 && m_debugLayer != 0)) {
            m_stats.opaquePassTimeMs = 0.0f;
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Setup render state for opaque pass
        SetupRenderPass(m_opaqueConfig);

        // Front-to-back list order is deliberately NOT preserved: runs are
        // merged by slab offset instead. On a tile-based deferred GPU (every
        // Mac this ships on) hidden-surface removal makes opaque submission
        // order irrelevant to overdraw, so the early-z hint was worth nothing
        // there and the draw count is worth a lot.
        RenderLayerPass(RenderLayer::Opaque);

        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.opaquePassTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    void ChunkRenderer::RenderCutout(const Camera& camera, const Frustum& frustum) {
        PROFILE_ZONE;
        if (!m_shadersLoaded || !m_meshes || (m_debugLayer >= 0 && m_debugLayer != 1)) {
            m_stats.cutoutPassTimeMs = 0.0f;
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Setup render state for cutout pass
        SetupRenderPass(m_cutoutConfig);

        // Merged like opaque. Cutout fragments still early-depth-test against
        // the opaque pass that already wrote depth, which is where the win was.
        RenderLayerPass(RenderLayer::Cutout);

        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.cutoutPassTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    void ChunkRenderer::RenderTranslucent(const Camera& camera, const Frustum& frustum) {
        PROFILE_ZONE;
        if (!m_shadersLoaded || !m_meshes || (m_debugLayer >= 0 && m_debugLayer != 2)) {
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

        // Back-to-front, and that order is kept on the GPU — see SubmitOrderedRuns.
        RenderLayerPass(RenderLayer::Translucent, /*backToFront=*/true);

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
        //
        // Debug cull override (free camera): every CULL decision — BFS
        // origin/cache key, frustum filter, sort origins, the translucent
        // resort below — reads cullCamera/cullFrustum, while the MVP and the
        // environment uniforms keep coming from `camera` (the view actually
        // being drawn). With no override the two are the same object.
        const Camera&  cullCamera  = m_cullOverrideActive ? m_cullCamera  : camera;
        const Frustum& cullFrustum = m_cullOverrideActive ? m_cullFrustum : frustum;
        PrepareVisibleSections(cullCamera, cullFrustum);

        // Bind opaque shader, compute MVP, and bind atlas texture
        BindSharedRenderState(camera);

        // Bind shared block VAO once per frame — all mega-buffers share this
        // VAO's vertex format.  Switching between mega-buffers only calls
        // BindBuffers() (glBindVertexBuffer + IBO rebind), avoiding the GPU
        // pipeline flush that glBindVertexArray causes on macOS. Zoned because
        // that same flush behavior makes THIS bind a candidate collection
        // point for deferred driver work (e.g. this frame's buffer uploads).
        if (m_meshes) {
            PROFILE_ZONE_N("BindBlockVAO");
            m_meshes->BindSharedBlockVAO();
        }

        // The three passes, wrapped so greedy-debug mode can run them twice:
        // FILL phase (solid dark grey, depth-biased away) then LINE phase
        // (heat colors, unbiased — wins the depth test over the fill). Stats
        // double-count in that mode; it is a debug view.
        auto renderPasses = [&]() {
        // Opaque pass: uses opaque shader (no discard → early-z enabled)
        {
            GPUTimerHandle t = beginPassTimer(0, "opaque");
            if (!DevSkip("opaque")) RenderOpaque(camera, frustum);
            endPassTimer(0, t);
        }

        // Switch to cutout shader for cutout + translucent passes (has discard)
        const ShaderHandle cutoutPassShader = m_cutoutShader;
        if (cutoutPassShader != INVALID_SHADER && g_renderBackend) {
            PROFILE_ZONE_N("PassShaderSwitch");
            m_activeShader = cutoutPassShader;
            g_renderBackend->BindShader(cutoutPassShader);
            g_renderBackend->SetUniformMat4(cutoutPassShader, "uMVP", m_cachedMVP);
            g_renderBackend->SetUniformVec4(cutoutPassShader, "uPortalClipPlane", s_portalClipPlane);
            SetEnvironmentUniforms(cutoutPassShader, camera);
            ApplyDebugOverlayUniform(cutoutPassShader);
            g_renderBackend->BindTexture(ActiveTerrainTexture(), 0);
        }

        {
            GPUTimerHandle t = beginPassTimer(1, "cutout");
            if (!DevSkip("cutout")) RenderCutout(camera, frustum);
            endPassTimer(1, t);
        }

        // Translucent pass — skipped wholesale when no visible section has any
        // translucent geometry (counted by PrepareVisibleSections this frame,
        // so a water section entering the view is never a frame late). The
        // pass itself would draw nothing, but the shader bind and uniform
        // uploads in front of it are not free on either backend.
        if (m_visibleTranslucentSections > 0) {
            // Switch to solid shader for translucent pass (no discard → early-z enabled,
            // blending handles transparency). This avoids the discard penalty entirely.
            const ShaderHandle solidPassShader = m_solidShader;
            if (solidPassShader != INVALID_SHADER && g_renderBackend) {
                PROFILE_ZONE_N("PassShaderSwitch");
                m_activeShader = solidPassShader;
                g_renderBackend->BindShader(solidPassShader);
                g_renderBackend->SetUniformMat4(solidPassShader, "uMVP", m_cachedMVP);
                g_renderBackend->SetUniformVec4(solidPassShader, "uPortalClipPlane", s_portalClipPlane);
                SetEnvironmentUniforms(solidPassShader, camera);
                ApplyDebugOverlayUniform(solidPassShader);
                g_renderBackend->BindTexture(ActiveTerrainTexture(), 0);
            }

            GPUTimerHandle t = beginPassTimer(2, "translucent");
            // The cull view, deliberately: RenderTranslucent's only use of its
            // camera is the resort origin, and back-to-front order must match
            // the origin the sections were sorted for — the override camera.
            if (!DevSkip("translucent")) RenderTranslucent(cullCamera, cullFrustum);
            endPassTimer(2, t);
        } else {
            m_stats.translucentPassTimeMs = 0.0f;
        }
        };  // renderPasses

        if (!m_greedyMeshDebug) {
            renderPasses();
        } else {
            m_debugFillPhase = true;
            ApplyDebugOverlayUniform(m_activeShader);
            renderPasses();
            m_debugFillPhase = false;
            // Re-enter the pass chain from the opaque shader: the first phase
            // left the translucent pass's solid shader bound.
            BindSharedRenderState(camera);
            renderPasses();
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


    // Greedy-debug substitute for the atlas: with a 1x1 white texture every
    // texel is opaque white, so the fragment shaders pass their alpha tests
    // and output pure vertex color — combined with line polygon mode the
    // terrain reads as its raw (merged) triangle structure.
    // uOverlayColor drives the greedy-debug FILL phase: solid dark grey over
    // every face, so the line phase draws on an opaque world instead of a
    // see-through one. Always set (cleared when the mode is off) — GL uniforms
    // persist per program, so a stale grey would tint normal rendering after
    // the toggle flips off.
    void ChunkRenderer::ApplyDebugOverlayUniform(ShaderHandle shader) {
        if (!g_renderBackend || shader == INVALID_SHADER) return;
        const glm::vec4 overlay = (m_greedyMeshDebug && m_debugFillPhase)
            ? glm::vec4(0.16f, 0.16f, 0.18f, 1.0f)   // the fill grey
            : glm::vec4(0.0f);
        g_renderBackend->SetUniformVec4(shader, "uOverlayColor", overlay);
    }

    TextureHandle ChunkRenderer::ActiveTerrainTexture() {
        if (!m_greedyMeshDebug) return m_backendAtlasTexture;
        if (m_whiteDebugTexture == INVALID_TEXTURE && g_renderBackend) {
            const unsigned char white[] = {255, 255, 255, 255};
            m_whiteDebugTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);
        }
        return m_whiteDebugTexture != INVALID_TEXTURE ? m_whiteDebugTexture : m_backendAtlasTexture;
    }

    void ChunkRenderer::SetGreedyMeshingEnabled(bool enable) {
        if (Mesher::GreedyEnabled() == enable) return;
        Mesher::SetGreedyEnabled(enable);
        if (m_meshes) m_meshes->RemeshAll();
    }
    bool ChunkRenderer::IsGreedyMeshingEnabled() const { return Mesher::GreedyEnabled(); }

    void ChunkRenderer::SetGreedyMeshDebug(bool enable) {
        if (m_greedyMeshDebug == enable) return;
        m_greedyMeshDebug = enable;
        // Colors are baked at MESH time, so the whole world remeshes with the
        // new palette. Main thread only (the debug panel runs there).
        Mesher::SetGreedyDebugColors(enable);
        if (m_meshes) m_meshes->RemeshAll();
    }

    void ChunkRenderer::GetGreedyTotals(uint64_t& eligibleIn, uint64_t& rectsOut) const {
        Mesher::GetGreedyTotals(eligibleIn, rectsOut);
    }

    void ChunkRenderer::SetWireframeMode(bool enable) {
        m_wireframeMode = enable;
        // Wireframe state is applied in SetupRenderPass via PipelineState
    }

    void ChunkRenderer::PrepareVisibleSectionsThroughPortal(const Camera& camera, const Frustum& frustum,
                                                            int renderDistanceChunks) {
        PROFILE_ZONE_N("PortalViewSections");
        m_visibleSections.clear();
        m_visibleSectionKeys.clear();
        m_visibleTranslucentSections = 0;
        const auto* ccm = m_chunks;
        if (!ccm) {
            if (m_recordMainView) m_mainViewSections.clear();
            return;
        }

        const int camChunkX = static_cast<int>(std::floor(camera.position.x / 16.0f));
        const int camChunkZ = static_cast<int>(std::floor(camera.position.z / 16.0f));
        const int sectionsY = Game::Math::SECTIONS_PER_CHUNK;
        // OBEY_PORTAL_DIAG=1: one line per second per portal view.
        static const bool kDiag = std::getenv("OBEY_PORTAL_DIAG") != nullptr;
        static const bool kNoCull = std::getenv("OBEY_PORTAL_NOCULL") != nullptr;
        int diagColumnsLoaded = 0, diagColumnsInFrustum = 0, diagWithGpu = 0;
        const float worldMinY = static_cast<float>(Config::MinY);
        const float worldMaxY = worldMinY + 16.0f * sectionsY;

        for (int cz = camChunkZ - renderDistanceChunks; cz <= camChunkZ + renderDistanceChunks; ++cz) {
            for (int cx = camChunkX - renderDistanceChunks; cx <= camChunkX + renderDistanceChunks; ++cx) {
                // Same buffer-1 rule as the BFS: the outer ring of loaded
                // chunks is a halo, never a draw candidate.
                if (!Game::Math::IsWithinChunkViewDistance(camChunkX, camChunkZ, renderDistanceChunks,
                                                           cx, cz, /*includeNeighbors=*/false)) {
                    continue;
                }
                const float minX = static_cast<float>(cx * 16);
                const float minZ = static_cast<float>(cz * 16);
                // Whole column first: most columns miss the narrow frustum.
                if (!kNoCull && !frustum.IsBoxVisible(glm::vec3(minX, worldMinY, minZ),
                                                      glm::vec3(minX + 16.0f, worldMaxY, minZ + 16.0f))) {
                    continue;
                }
                ++diagColumnsInFrustum;
                const Client::ClientChunk* chunk = ccm->GetChunk({cx, cz});
                if (!chunk || chunk->state != Client::ChunkState::LOADED) continue;
                ++diagColumnsLoaded;

                for (int sy = 0; sy < sectionsY; ++sy) {
                    const auto& si = chunk->sectionInfos[sy];
                    // MC's emptySections test: all-air is never a draw
                    // candidate (and needs no mesh).
                    if (si.isAllAir) continue;
                    const float minY = worldMinY + static_cast<float>(sy * 16);
                    if (!kNoCull && !frustum.IsBoxVisible(glm::vec3(minX, minY, minZ),
                                                          glm::vec3(minX + 16.0f, minY + 16.0f, minZ + 16.0f))) {
                        continue;
                    }
                    const float dx = (minX + 8.0f) - camera.position.x;
                    const float dy = (minY + 8.0f) - camera.position.y;
                    const float dz = (minZ + 8.0f) - camera.position.z;
                    SectionRenderData rd(Game::Math::ChunkPos{cx, cz}, sy, dx * dx + dy * dy + dz * dz);
                    rd.nearby   = rd.distanceToCamera < kTranslucentNearRadiusSq;
                    // Resolved live — this list never survives the frame.
                    const GPUSectionData* gpu = si.gpuData.load(std::memory_order_acquire);
                    rd.resolved = gpu;
                    if (gpu && gpu->translucentIndexCount > 0) ++m_visibleTranslucentSections;
                    if (gpu) ++diagWithGpu;
                    m_visibleSections.push_back(rd);
                }
            }
        }

        // Front to back, as the BFS output is: the opaque pass wants it and
        // the translucent pass reads the list in reverse.
        std::sort(m_visibleSections.begin(), m_visibleSections.end(),
                  [](const SectionRenderData& a, const SectionRenderData& b) {
                      return a.distanceToCamera < b.distanceToCamera;
                  });

        for (const auto& rd : m_visibleSections) {
            m_visibleSectionKeys.insert(VisibleSectionKey(rd.chunkPos, rd.sectionY));
        }
        // A far level's main view (its mesh scheduler reads this) — see
        // SetRecordMainView.
        if (m_recordMainView) m_mainViewSections = m_visibleSections;

        if (kDiag) {
            static auto lastLog = std::chrono::steady_clock::now() - std::chrono::seconds(2);
            const auto now = std::chrono::steady_clock::now();
            if (now - lastLog >= std::chrono::seconds(1)) {
                lastLog = now;
                Log::Info("[PortalDiag] cam=(%.1f,%.1f,%.1f) yaw=%.1f pitch=%.1f rd=%d columns inFrustum=%d loaded=%d sections=%zu withGpu=%d planes: "
                          "(%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f)",
                          camera.position.x, camera.position.y, camera.position.z, camera.yaw, camera.pitch,
                          renderDistanceChunks, diagColumnsInFrustum, diagColumnsLoaded, m_visibleSections.size(), diagWithGpu,
                          frustum.planes[0].x, frustum.planes[0].y, frustum.planes[0].z,
                          frustum.planes[1].x, frustum.planes[1].y, frustum.planes[1].z,
                          frustum.planes[2].x, frustum.planes[2].y, frustum.planes[2].z,
                          frustum.planes[3].x, frustum.planes[3].y, frustum.planes[3].z);
            }
        }

        m_lastReachableCount = static_cast<uint32_t>(m_visibleSections.size());
        m_lastVisibleCount   = static_cast<uint32_t>(m_visibleSections.size());
        m_stats.sectionsRendered = static_cast<int>(m_visibleSections.size());
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
        // A portal view seeds its BFS at the far surface, not at its camera
        // (see SetPortalViewSeed); its slot is keyed by the seed's section.
        const bool portalView = m_useProjectionOverride && m_portalSeedActive;
        const glm::vec3 bfsOrigin = portalView ? m_portalSeed : camera.position;
        int currentChunkX = static_cast<int>(std::floor(bfsOrigin.x / 16.0f));
        int currentChunkZ = static_cast<int>(std::floor(bfsOrigin.z / 16.0f));
        int currentSectionY = static_cast<int>(std::floor((bfsOrigin.y - Config::MinY) / 16.0f));

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
                // anchor, so nothing extra needs recording here. A portal
                // view's grid is not adopted: the live graph is the main
                // view's (see BfsJob::portalView).
                if (!job->portalView) m_occlusionGraph.AdoptGraph(*job);
            }
            // Stale erase token: result holds dangling pointers — drop it.
            m_occlusionGraph.RecycleJob(std::move(job));
        }

        // World changed (mesh upload/unload, smart-cull toggle): existing
        // results are stale (but pointer-safe) — they trigger a refresh below.
        if (m_occlusionGraph.ConsumeFullRebuildRequest()) m_visibleSectionsDirty = true;
        if (m_visibleSectionsDirty) {
            m_visibleSectionsDirty = false;
            m_worldVersion++;
        }

        // Effective render distance (needed by both sync and async paths)
        int renderDistanceChunks = Platform::g_gameSettings.GetRenderDistance();
        if (Client::g_networkClient && Client::g_networkClient->GetServerViewDistance() > 0) {
            renderDistanceChunks = std::min(renderDistanceChunks, Client::g_networkClient->GetServerViewDistance());
        }

        // RENDER-DISTANCE CHANGE invalidation. The reachable-slot cache is
        // keyed by camera section + worldVersion but NOT by render distance,
        // so without this a slot built at the old radius keeps serving its
        // truncated list until some unrelated invalidation happens to land —
        // seen as freshly streamed chunks staying culled from SOME camera
        // sections after raising the setting (slots refreshed since the
        // change are fine, older ones are not, so coverage looks arbitrary).
        // Bumping worldVersion marks every slot stale at once; the async
        // rebuild refreshes them at the new radius while the old lists keep
        // rendering in the meantime. The live graph needs no explicit drop:
        // HasGraphFor already compares renderDistance, so partial updates
        // pause on their own until a rebuild at the new radius is adopted.
        if (renderDistanceChunks != m_lastRenderDistanceChunks) {
            m_lastRenderDistanceChunks = renderDistanceChunks;
            m_worldVersion++;
        }

        // PORTAL VIEWS. A view through a portal has a camera that sits
        // wherever the portal's transform put it — often inside rock or an
        // unstreamed chunk, where a BFS sees nothing — so its BFS is seeded
        // at the far surface instead (bfsOrigin, from SetPortalViewSeed) and
        // cached in a slot keyed by THAT section, never confused with the
        // main camera's. A portal view without a seed (the legacy gun pass)
        // discovers sections by frustum alone, as the mod does.
        if (m_useProjectionOverride && !portalView) {
            PrepareVisibleSectionsThroughPortal(camera, frustum, renderDistanceChunks);
            auto overallEnd = std::chrono::high_resolution_clock::now();
            m_stats.chunkIterationTimeMs = std::chrono::duration<float, std::milli>(overallEnd - overallStartTime).count();
            return;
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

        // A portal view with no reachable set of its own yet must not borrow
        // another view's (that was terrain vanishing through portals as the
        // view turned): it asks the worker for one and draws by frustum
        // alone until it lands, a frame or two later.
        if (portalView && !exact && usable) {
            if (m_meshes && !m_occlusionGraph.Busy()) {
                m_lastRebuildSubmit = std::chrono::steady_clock::now();
                auto job = m_occlusionGraph.AcquireJob();
                job->keyCx = currentChunkX;
                job->keyCz = currentChunkZ;
                job->keySy = currentSectionY;
                job->worldVersion = m_worldVersion;
                job->eraseToken = m_eraseToken;
                job->portalView = true;
                m_occlusionGraph.BuildInput(*job, bfsOrigin, m_enableSmartCull, renderDistanceChunks);
                m_occlusionGraph.SubmitAsync(std::move(job));
            }
            PrepareVisibleSectionsThroughPortal(camera, frustum, renderDistanceChunks);
            auto overallEnd = std::chrono::high_resolution_clock::now();
            m_stats.chunkIterationTimeMs = std::chrono::duration<float, std::milli>(overallEnd - overallStartTime).count();
            return;
        }

        if (!usable) {
            // Cold start (world entry / post-erase): synchronous rebuild so
            // this frame renders correct data.
            if (!m_meshes) {
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
            job->portalView = portalView;

            auto iterationStart = std::chrono::high_resolution_clock::now();
            m_occlusionGraph.BuildInput(*job, bfsOrigin, m_enableSmartCull, renderDistanceChunks);
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
            if (!portalView) m_occlusionGraph.AdoptGraph(*job);   // cold-start graph, same as the async path
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
        const size_t diagPrePartial = usable->sections.size();
        if (m_occlusionGraph.HasGraphFor(usable->cx, usable->cz, usable->sy,
                                         renderDistanceChunks, m_eraseToken)) {
            if (m_occlusionGraph.RunPartialUpdate(camera.position, usable->cx, usable->cz,
                                                  usable->sy, renderDistanceChunks,
                                                  m_eraseToken, usable->sections)) {
                // Appended entries land after the front-to-back sorted body,
                // and the translucent pass reads this list in reverse, so the
                // order has to be restored. The body is already sorted (BFS
                // output, kept sorted by this very step), so sort only the
                // appended tail and merge — O(k log k + n) rather than the
                // O(n log n) re-sort of the whole reachable list this used to
                // do on every frame that appended anything.
                auto& list = usable->sections;
                const auto byDistance = [](const SectionRenderData& a, const SectionRenderData& b) {
                    return a.distanceToCamera < b.distanceToCamera;
                };
                const auto tail = list.begin() + static_cast<std::ptrdiff_t>(diagPrePartial);
                std::sort(tail, list.end(), byDistance);
                std::inplace_merge(list.begin(), tail, list.end(), byDistance);
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
        if (!m_useProjectionOverride) {
            m_diagPartialAdds += static_cast<int>(usable->sections.size() - diagPrePartial);
        }

        // Kick an async refresh when the current view's data is missing or
        // stale and the worker is idle (one job in flight at a time — no
        // queue buildup, newest state wins).
        const bool haveExactFresh = exact && exact->worldVersion == m_worldVersion;
        bool startedFullRebuild = false;
        // REBUILD RATE LIMIT. During a post-blast remesh flood the world
        // version bumps every frame (each section that remeshes to empty kicks
        // MarkVisibleSectionsDirty), so this used to submit a rebuild every
        // single frame — measured at 150-380/s — and each result REPLACED the
        // visible list mid-churn, with consecutive snapshots disagreeing by 4x
        // (733 vs 3425 visible in one second, stationary camera). That IS the
        // flashing. MC only invalidates on an 8-block move or needsUpdate();
        // we keep our richer triggers but floor the steady-camera cadence at
        // 4/s. A camera-section change (no exact slot) still submits
        // immediately — movement latency is untouched — and the per-frame
        // partial updates keep ADDING new sections between rebuilds, so only
        // removals wait, and a late removal is invisible over-draw.
        const bool urgentRebuild = exact == nullptr;
        const auto rebuildNow = std::chrono::steady_clock::now();
        if (!haveExactFresh && m_meshes && !m_occlusionGraph.Busy() &&
            (urgentRebuild ||
             rebuildNow - m_lastRebuildSubmit >= std::chrono::milliseconds(250))) {
            m_lastRebuildSubmit = rebuildNow;
            auto job = m_occlusionGraph.AcquireJob();
            job->keyCx = currentChunkX;
            job->keyCz = currentChunkZ;
            job->keySy = currentSectionY;
            job->worldVersion = m_worldVersion;
            job->eraseToken = m_eraseToken;
            job->portalView = portalView;
            m_occlusionGraph.BuildInput(*job, bfsOrigin, m_enableSmartCull, renderDistanceChunks);
            m_occlusionGraph.SubmitAsync(std::move(job));
            startedFullRebuild = true;
        }
        // Full rebuilds per frame. MC only invalidates on an 8-block camera move
        // or needsUpdate(); anything above ~0 while standing still means
        // something is forcing rebuilds that propagation should be handling.
        PROFILE_PLOT("Bfs/FullRebuilds", static_cast<int64_t>(startedFullRebuild ? 1 : 0));
        if (startedFullRebuild && !m_useProjectionOverride) m_diagRebuilds++;

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
            // Uses the CONST GetSectionInfo deliberately. It used to matter
            // more: the non-const overload called UpdateAccessTime(), a
            // steady_clock::now() per section per frame for a field nothing
            // read. That field is gone now — a `sample` profile put it at 14%
            // of the client main thread once a hundred thousand primed TNT were
            // querying blocks through it — so this is now just the right
            // overload for a read.
            //
            // Do NOT switch this to ClientMeshManager::GetSectionGPUData — that
            // takes shared_lock(m_gpuDataMutex), and UploadMeshResultToGPU holds
            // that mutex uniquely while calling back into this renderer.
            //
            // ...except that "fresh every frame" is a hash lookup per section,
            // and the answer only changes when the mesh manager publishes or
            // destroys GPU data. So the SLOT entry remembers its answer tagged
            // with the GPU-data generation (SectionRenderData::cachedGpu); the
            // lookup runs only when the stamp is stale. Semantics are
            // identical to a live lookup — the generation bumps before any
            // pointer becomes invalid — the steady-state frame just does none.
            const auto* ccm = m_chunks;
            const uint64_t gpuGeneration =
                m_meshes ? m_meshes->GetGpuDataGeneration() : 0;
            [[maybe_unused]] int64_t deadEntries = 0;
            [[maybe_unused]] int64_t lookups = 0;
            int translucentVisible = 0;
            auto resolveLive = [&](SectionRenderData& slotEntry) -> const GPUSectionData* {
                if (slotEntry.cacheGeneration != gpuGeneration) {
                    const Client::SectionInfo* si =
                        ccm ? ccm->GetSectionInfo(slotEntry.chunkPos, slotEntry.sectionY) : nullptr;
                    slotEntry.cachedGpu = si ? si->gpuData.load(std::memory_order_acquire) : nullptr;
                    slotEntry.cacheGeneration = gpuGeneration;
                    ++lookups;
                }
                const GPUSectionData* gpu = slotEntry.cachedGpu;
                if (!gpu) ++deadEntries;
                else if (gpu->translucentIndexCount > 0) ++translucentVisible;
                return gpu;
            };

            if (m_enableFrustumCulling) {
                for (auto& section : slot->sections) {
                    float minX = static_cast<float>(section.chunkPos.x * 16);
                    float minY = static_cast<float>(section.sectionY * 16 + Config::MinY);
                    float minZ = static_cast<float>(section.chunkPos.z * 16);
                    if (frustum.IsBoxVisible(glm::vec3(minX, minY, minZ),
                                             glm::vec3(minX + 16.0f, minY + 16.0f, minZ + 16.0f))) {
                        const GPUSectionData* gpu = resolveLive(section);
                        m_visibleSections.push_back(section);
                        SectionRenderData& vis = m_visibleSections.back();
                        vis.resolved = gpu;
                        tagNearby(vis, minX, minY, minZ);
                    }
                }
            } else {
                for (auto& section : slot->sections) {
                    const GPUSectionData* gpu = resolveLive(section);
                    m_visibleSections.push_back(section);
                    SectionRenderData& vis = m_visibleSections.back();
                    vis.resolved = gpu;
                    tagNearby(vis,
                              static_cast<float>(vis.chunkPos.x * 16),
                              static_cast<float>(vis.sectionY * 16 + Config::MinY),
                              static_cast<float>(vis.chunkPos.z * 16));
                }
            }
            m_visibleTranslucentSections = translucentVisible;
            PROFILE_PLOT("Sections/Dead", deadEntries);
            // Hash lookups actually performed. ~0 while nothing uploads; a
            // full count on the frame after any upload/unload, which is the
            // price of the generation scheme and expected during streaming.
            PROFILE_PLOT("Sections/Lookups", lookups);

            // Identity mirror for IsSectionVisible — see the header. Rebuilt
            // on EVERY pass, portal recursions included, so the entity passes
            // that follow each chunk pass see that pass's view.
            m_visibleSectionKeys.clear();
            for (const auto& rd : m_visibleSections) {
                m_visibleSectionKeys.insert(VisibleSectionKey(rd.chunkPos, rd.sectionY));
            }

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
            if (!m_useProjectionOverride || m_recordMainView) {
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
        // For the Render Controls occlusion readout (main thread only).
        m_lastReachableCount = static_cast<uint32_t>(slot->sections.size());
        m_lastVisibleCount   = static_cast<uint32_t>(m_visibleSections.size());

        m_stats.sectionsRendered = static_cast<int>(m_visibleSections.size());
        m_stats.sectionsSkipped = m_bfsOccludedCount;
        m_stats.sectionsAvailable = m_bfsVisitedCount;

        // ── Culling diagnostics (main view only) ───────────────────────
        if (!m_useProjectionOverride) {
            m_diagVisMin = std::min(m_diagVisMin, m_visibleSections.size());
            m_diagVisMax = std::max(m_diagVisMax, m_visibleSections.size());
            const auto diagNow = std::chrono::steady_clock::now();
            // Off by default: a per-second line in every session's log. Flip
            // for a culling investigation — visMin<<visMax within one second
            // IS the flashing, in numbers (see the F8 DumpViewRay as well).
            constexpr bool kCullDiagLog = false;
            if (kCullDiagLog && diagNow - m_diagLastLog >= std::chrono::seconds(1)) {
                m_diagLastLog = diagNow;
                Log::Info("[CullDiag] cam=(%d,%d,%d) rd=%d slot=(%d,%d,%d) slotVer=%llu/%llu exact=%d "
                          "reach=%zu visMin=%zu visMax=%zu rebuilds=%d partialAdds=%d backlog=%zu graphOk=%d",
                          currentChunkX, currentChunkZ, currentSectionY, renderDistanceChunks,
                          usable->cx, usable->cz, usable->sy,
                          static_cast<unsigned long long>(usable->worldVersion),
                          static_cast<unsigned long long>(m_worldVersion),
                          static_cast<int>(usable == exact),
                          usable->sections.size(),
                          m_diagVisMin == static_cast<size_t>(-1) ? 0 : m_diagVisMin, m_diagVisMax,
                          m_diagRebuilds, m_diagPartialAdds,
                          m_occlusionGraph.PendingSourceCount(),
                          static_cast<int>(m_occlusionGraph.HasGraphFor(
                              usable->cx, usable->cz, usable->sy,
                              renderDistanceChunks, m_eraseToken)));
                m_diagVisMin = static_cast<size_t>(-1);
                m_diagVisMax = 0;
                m_diagRebuilds = 0;
                m_diagPartialAdds = 0;
            }
            if (Input::IsKeyPressed(Input::Key::F8)) {
                DumpViewRay(camera, frustum, *usable, renderDistanceChunks);
            }
        }

        auto overallEndTime = std::chrono::high_resolution_clock::now();
        m_stats.buildDrawListsTimeMs = std::chrono::duration<float, std::milli>(overallEndTime - overallStartTime).count();
    }

    // F8 diagnostic: walk the camera's view ray and log every culling-relevant
    // fact about each section it passes through — is it in the reachable slot,
    // does it pass the frustum, is its mesh present/stale/dirty, what does its
    // visibility mask say. One press while looking at a hole tells us which
    // stage dropped the missing section, without guessing.
    void ChunkRenderer::DumpViewRay(const Camera& camera, const Frustum& frustum,
                                    const ReachableCacheSlot& slot, int renderDistanceChunks) {
        auto* ccm = m_chunks;
        // Heal-probe (2026-08-30 hole hunt): sections whose live block data is
        // solid while the recorded mesh resolved EMPTY, with versions in
        // agreement, are the stale-empty-mesh contradiction behind the
        // reported world holes (all on mass-edited /shape terrain, ver in the
        // thousands). F8 re-dirties them: if the hole fills on the press, the
        // diagnosis is confirmed as a missed dirty/version bump on the edit
        // path. Harmless on legitimately-enclosed solid sections — they just
        // remesh to empty again.
        int healed = 0;
        // Both directions of the stale-mesh contradiction: solid data with an
        // empty mesh (HOLE) and all-air data with leftover geometry (GHOST —
        // "old terrain after blowing things up until I get close").
        auto healIfStaleEmpty = [&](int hx, int hz, int hy,
                                    const Client::SectionInfo* hsi, int nonAirCount) {
            if (!hsi || hsi->dirty) return;
            const GPUSectionData* hgpu = hsi->gpuData.load(std::memory_order_acquire);
            const bool hasMesh = hgpu && hgpu->HasGeometry();
            const char* kind = nullptr;
            if (nonAirCount > 0 && (hsi->meshResolvedEmpty || !hasMesh)) kind = "stale-EMPTY (hole)";
            else if (nonAirCount == 0 && hasMesh)                        kind = "stale-SOLID (ghost)";
            if (!kind) return;
            ccm->MarkSectionDirty(Game::Math::ChunkPos{hx, hz}, hy);
            ++healed;
            Log::Info("[CullDump] SUSPECT %s mesh (%d,%d,%d) nonAir=%d ver=%u seq=%u/%u — re-dirtied",
                      kind, hx, hz, hy, nonAirCount, hsi->version,
                      hsi->uploadedJobSeq, hsi->lastJobSeq);
        };
        Log::Info("[CullDump] ==== pos=(%.1f,%.1f,%.1f) yaw=%.1f pitch=%.1f rd=%d "
                  "slot=(%d,%d,%d) slotN=%zu slotVer=%llu/%llu backlog=%zu",
                  camera.position.x, camera.position.y, camera.position.z,
                  camera.yaw, camera.pitch, renderDistanceChunks,
                  slot.cx, slot.cz, slot.sy, slot.sections.size(),
                  static_cast<unsigned long long>(slot.worldVersion),
                  static_cast<unsigned long long>(m_worldVersion),
                  m_occlusionGraph.PendingSourceCount());
        if (!ccm) return;
        std::set<std::tuple<int, int, int>> inSlot;
        for (const auto& s : slot.sections)
            inSlot.insert({s.chunkPos.x, s.chunkPos.z, s.sectionY});
        const glm::vec3 dir = camera.GetForward();
        std::set<std::tuple<int, int, int>> seen;
        // Column ground probe: a grazing ray samples 3D cells 4 blocks apart
        // and can step OVER the one missing ground section it was aimed at
        // (a 16-block hole 500 blocks out subtends a fraction of a degree).
        // So for every (cx,cz) column the ray crosses, also find the topmost
        // NON-AIR section at-or-below the ray and log it with a GROUND tag —
        // on flat terrain the hole IS a ground section, and a column walk
        // cannot miss it.
        std::set<std::pair<int, int>> groundProbed;
        int logged = 0;
        const float maxDist = static_cast<float>(renderDistanceChunks) * 16.0f;
        for (float d = 0.0f; d <= maxDist && logged < 96; d += 4.0f) {
            const glm::vec3 p = glm::vec3(camera.position) + dir * d;
            const int cx = static_cast<int>(std::floor(p.x / 16.0f));
            const int cz = static_cast<int>(std::floor(p.z / 16.0f));
            const int sy = static_cast<int>(std::floor((p.y - Config::MinY) / 16.0f));
            if (sy < 0 || sy >= 24) continue;
            const bool newCell = seen.insert({cx, cz, sy}).second;
            if (groundProbed.insert({cx, cz}).second && ccm) {
                if (const Client::ClientChunk* gch = ccm->GetChunk({cx, cz});
                    gch && gch->chunkData) {
                    for (int gy = std::min(sy, 23); gy >= 0; --gy) {
                        const auto* gsec = gch->chunkData->GetSection(gy);
                        if (!gsec || gsec->IsAllAir()) continue;
                        const Client::SectionInfo* gsi = ccm->GetSectionInfo({cx, cz}, gy);
                        GPUSectionData* ggpu =
                            gsi ? gsi->gpuData.load(std::memory_order_acquire) : nullptr;
                        const bool gGeom = ggpu && ggpu->HasGeometry();
                        {
                            int gNonAir = 0;
                            for (int by = 0; by < 16 && gNonAir == 0; ++by)
                                for (int bz = 0; bz < 16 && gNonAir == 0; ++bz)
                                    for (int bx = 0; bx < 16; ++bx)
                                        if (gsec->GetBlockID(bx, by, bz) != Game::BlockID::Air) { gNonAir = 1; break; }
                            healIfStaleEmpty(cx, cz, gy, gsi, gNonAir);
                        }
                        Log::Info("[CullDump] GROUND (%d,%d,%d) inSlot=%d dirty=%d built=%d "
                                  "ver=%u up=%u mesh=%u gpu=%d drawn=%d cmds=o%d",
                                  cx, cz, gy,
                                  static_cast<int>(inSlot.count({cx, cz, gy}) > 0),
                                  gsi ? static_cast<int>(gsi->dirty) : -1,
                                  gsi ? static_cast<int>(gsi->builtOnce) : -1,
                                  gsi ? gsi->version : 0, gsi ? gsi->uploadedVersion : 0,
                                  gsi ? gsi->meshingVersion : 0,
                                  static_cast<int>(gGeom),
                                  static_cast<int>(IsSectionVisible(Game::Math::ChunkPos{cx, cz}, gy)),
                                  gGeom && ggpu->opaqueDrawCmd.valid ? ggpu->opaqueDrawCmd.indexCount : 0);
                        break;
                    }
                } else {
                    Log::Info("[CullDump] GROUND (%d,%d) COLUMN-NOT-LOADED", cx, cz);
                }
            }
            if (!newCell) continue;
            const float minX = cx * 16.0f;
            const float minY = sy * 16.0f + static_cast<float>(Config::MinY);
            const float minZ = cz * 16.0f;
            const bool inFrustum = frustum.IsBoxVisible(
                glm::vec3(minX, minY, minZ), glm::vec3(minX + 16.0f, minY + 16.0f, minZ + 16.0f));
            const Client::SectionInfo* si =
                ccm->GetSectionInfo(Game::Math::ChunkPos{cx, cz}, sy);
            if (!si) {
                Log::Info("[CullDump] d=%4.0f (%d,%d,%d) CHUNK-NOT-LOADED frustum=%d",
                          d, cx, cz, sy, static_cast<int>(inFrustum));
                ++logged;
                continue;
            }
            GPUSectionData* gpu = si->gpuData.load(std::memory_order_acquire);
            const bool hasGeom = gpu && gpu->HasGeometry();
            // GROUND TRUTH from the actual block data, not the cached mirror:
            // does the section really hold blocks, and how many? A section
            // that reads air=0/gpu=0 (enclosed-solid claim) while the player
            // flies through it is lying somewhere — this pins which side.
            int actualAir = -1;   // -1 = no CPU data to check
            int nonAir = -1;
            if (const Client::ClientChunk* chunk = ccm->GetChunk({cx, cz});
                chunk && chunk->chunkData) {
                const auto* csec = chunk->chunkData->GetSection(sy);
                actualAir = (csec == nullptr) || csec->IsAllAir() ? 1 : 0;
                if (csec && actualAir == 0) {
                    nonAir = 0;
                    for (int by = 0; by < 16; ++by)
                        for (int bz = 0; bz < 16; ++bz)
                            for (int bx = 0; bx < 16; ++bx)
                                if (csec->GetBlockID(bx, by, bz) != Game::BlockID::Air)
                                    ++nonAir;
                }
            }
            // drawn = in this frame's post-frustum draw list; cmds = per-layer
            // cached draw-command index counts (0 with gpu=1 means the upload
            // produced no draw for that layer). Added for the 2026-08-30 hole
            // hunt: separates "culled" from "meshed but not drawn".
            Log::Info("[CullDump] d=%4.0f (%d,%d,%d) inSlot=%d frustum=%d air=%d ACTUALAIR=%d NONAIR=%d "
                      "dirty=%d built=%d mre=%d ver=%u up=%u mesh=%u gpu=%d vis=%016llx drawn=%d cmds=o%d/c%d/t%d",
                      d, cx, cz, sy,
                      static_cast<int>(inSlot.count({cx, cz, sy}) > 0),
                      static_cast<int>(inFrustum),
                      static_cast<int>(si->isAllAir), actualAir, nonAir,
                      static_cast<int>(si->dirty),
                      static_cast<int>(si->builtOnce),
                      static_cast<int>(si->meshResolvedEmpty),
                      si->version, si->uploadedVersion, si->meshingVersion,
                      static_cast<int>(hasGeom),
                      static_cast<unsigned long long>(hasGeom ? gpu->visibilitySet.raw() : 0ULL),
                      static_cast<int>(IsSectionVisible(Game::Math::ChunkPos{cx, cz}, sy)),
                      hasGeom && gpu->opaqueDrawCmd.valid      ? gpu->opaqueDrawCmd.indexCount      : 0,
                      hasGeom && gpu->cutoutDrawCmd.valid      ? gpu->cutoutDrawCmd.indexCount      : 0,
                      hasGeom && gpu->translucentDrawCmd.valid ? gpu->translucentDrawCmd.indexCount : 0);
            // GPU-side readback (Vulkan: slabs are persistently mapped): the
            // opaque region's bookkeeping, its first indices, and the vertex
            // the first index points at. Distinguishes "command exists but the
            // bytes are dead" (zeros / out-of-region indices / garbage vertex)
            // from "geometry is fine, the problem is later in the pipe". Also
            // whether the frame's draw list actually RESOLVED this section.
            if (hasGeom && m_meshes) {
                bool resolvedNow = false;
                for (const auto& sr : m_mainViewSections) {
                    if (sr.chunkPos.x == cx && sr.chunkPos.z == cz && sr.sectionY == sy) {
                        resolvedNow = (sr.resolved != nullptr);
                        break;
                    }
                }
                auto* mb = m_meshes->GetMegaBuffer(RenderLayer::Opaque);
                uint32_t rSlab = 0; size_t vOff = 0, vCnt = 0, iOff = 0, iCnt = 0;
                if (mb && mb->DebugGetRegionInfo({Game::Math::ChunkPos{cx, cz}, sy},
                                                 rSlab, vOff, vCnt, iOff, iCnt)) {
                    const auto* idxPtr = static_cast<const uint32_t*>(
                        g_renderBackend->DebugGetMappedBufferPtr(mb->DebugGetSlabIbo(rSlab)));
                    const auto* vtxPtr = static_cast<const float*>(
                        g_renderBackend->DebugGetMappedBufferPtr(mb->DebugGetSlabVbo(rSlab)));
                    char ibuf[160]; int off = 0;
                    const size_t nShow = std::min<size_t>(iCnt, 8);
                    if (idxPtr) {
                        for (size_t k = 0; k < nShow; ++k)
                            off += snprintf(ibuf + off, sizeof(ibuf) - off, "%u ",
                                            idxPtr[iOff + k]);
                    } else {
                        off += snprintf(ibuf + off, sizeof(ibuf) - off, "(unmapped)");
                    }
                    float vx = 0, vy = 0, vz = 0; bool haveV = false;
                    if (idxPtr && vtxPtr && iCnt > 0) {
                        const uint32_t v0 = idxPtr[iOff];
                        const size_t floatsPerVert = 32 / sizeof(float);
                        vx = vtxPtr[v0 * floatsPerVert + 0];
                        vy = vtxPtr[v0 * floatsPerVert + 1];
                        vz = vtxPtr[v0 * floatsPerVert + 2];
                        haveV = true;
                    }
                    // Was this section's index range inside any run the last
                    // opaque submit actually issued? submitted=0 with
                    // resolved=1 convicts the run builder / submit path.
                    bool submitted = false;
                    for (const auto& r : m_lastOpaqueRuns) {
                        if (r[0] == rSlab && r[1] <= iOff && iOff + iCnt <= r[2]) { submitted = true; break; }
                    }
                    Log::Info("[CullDump]    gpudata slab=%u v=[%zu+%zu] i=[%zu+%zu] resolved=%d submitted=%d idx: %s%s(%.1f,%.1f,%.1f) inRegion=%d",
                              rSlab, vOff, vCnt, iOff, iCnt,
                              static_cast<int>(resolvedNow), static_cast<int>(submitted), ibuf,
                              haveV ? "v0=" : "", vx, vy, vz,
                              static_cast<int>(idxPtr && iCnt > 0 &&
                                  idxPtr[iOff] >= vOff && idxPtr[iOff] < vOff + vCnt));
                } else {
                    Log::Info("[CullDump]    gpudata NO-OPAQUE-REGION resolved=%d",
                              static_cast<int>(resolvedNow));
                }
            }
            healIfStaleEmpty(cx, cz, sy, si, actualAir == 0 ? std::max(nonAir, 1) : 0);
            ++logged;
        }
        Log::Info("[CullDump] ==== end (%d sections, %d suspect sections re-dirtied)",
                  logged, healed);
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
        if (m_visibleSections.empty() || !m_meshes) return;

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
            if (m_meshes->ResortTranslucentSection(
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

        // Bind the opaque shader. (The old OBEY_SKIP=opaquediscard A/B swap is
        // gone: the opaque shader no longer has a discard to measure against.)
        const ShaderHandle opaquePassShader = m_opaqueShader;
        m_activeShader = opaquePassShader;
        g_renderBackend->BindShader(opaquePassShader);

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
            : glm::perspective(glm::radians(camera.fov), aspect, s_nearPlane, farPlane);
        m_cachedMVP = proj * view;
        g_renderBackend->SetUniformMat4(opaquePassShader, "uMVP", m_cachedMVP);
        g_renderBackend->SetUniformVec4(opaquePassShader, "uPortalClipPlane", s_portalClipPlane);
        SetEnvironmentUniforms(opaquePassShader, camera);
        ApplyDebugOverlayUniform(opaquePassShader);

        // Fetch and bind atlas texture once (fresh handle in case atlas was rebuilt)
        if (g_atlasBuilder) {
            m_backendAtlasTexture = g_atlasBuilder->GetBackendTextureHandle();
        }
        if (m_backendAtlasTexture != INVALID_TEXTURE) {
            g_renderBackend->BindTexture(ActiveTerrainTexture(), 0);
        }
    }

    void ChunkRenderer::RenderLayerPass(RenderLayer layer, bool backToFront) {
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
        auto* megaBuffer = m_meshes ? m_meshes->GetMegaBuffer(layer) : nullptr;
        if (!megaBuffer || !megaBuffer->IsInitialized()) return;
        const uint32_t slabCount = megaBuffer->GetSlabCount();

        int layerCount = 0;
        uint32_t totalVerts = 0, totalIndices = 0;

        // Collect this layer's draw entries from the visible list. Zone split:
        // "BuildDrawList" is OUR loop over visible sections; "SubmitMultiDraw"
        // is time spent inside driver calls. If a pass shows milliseconds,
        // this tells you which side owns them.
        m_drawEntries.clear();
        {
            PROFILE_ZONE_N("BuildDrawList");
            auto processSection = [&](const SectionRenderData& section) {
                // `resolved` was bound this frame by the frustum filter. Null
                // means the section has no mesh right now — never meshed, or
                // remeshed to empty, or unloaded — and it silently contributes
                // nothing, exactly as MC's null getBuffers(layer) does.
                if (!section.resolved) return;

                const auto& cachedCmd = (layer == RenderLayer::Opaque)      ? section.resolved->opaqueDrawCmd :
                                        (layer == RenderLayer::Cutout)       ? section.resolved->cutoutDrawCmd :
                                                                               section.resolved->translucentDrawCmd;
                if (cachedCmd.valid && cachedCmd.indexCount > 0 && cachedCmd.slabIndex < slabCount) {
                    m_drawEntries.push_back({cachedCmd.slabIndex, cachedCmd.indexOffset,
                                             static_cast<uint32_t>(cachedCmd.indexCount), cachedCmd.ibo});
                    layerCount++;

                    totalIndices += static_cast<uint32_t>(cachedCmd.indexCount);
                    switch (layer) {
                        case RenderLayer::Opaque:      totalVerts += section.resolved->opaqueVertexCount; break;
                        case RenderLayer::Cutout:       totalVerts += section.resolved->cutoutVertexCount; break;
                        case RenderLayer::Translucent:  totalVerts += section.resolved->translucentVertexCount; break;
                    }
                }
            };

            if (backToFront) {
                for (auto it = m_visibleSections.rbegin(); it != m_visibleSections.rend(); ++it)
                    processSection(*it);
            } else {
                for (const auto& section : m_visibleSections)
                    processSection(section);
            }
        }

        int subDraws = 0;
        if (!m_drawEntries.empty()) {
            PROFILE_ZONE_N("SubmitMultiDraw");
            if (megaBuffer->UsesPerSectionIndexBuffers()) {
                subDraws = SubmitPerSectionIbos(*megaBuffer);
            } else if (backToFront) {
                subDraws = SubmitOrderedRuns(*megaBuffer);
            } else {
                subDraws = SubmitMergedRuns(*megaBuffer);
            }
        }

        m_stats.totalVerticesRendered += totalVerts;
        m_stats.totalIndicesRendered += totalIndices;

        // Draws/Chunk: sections with geometry in this layer (one sub-draw each
        // before merging). Draws/Merged: sub-draws actually issued. The second
        // is the number that drives driver cost — MoltenVK encodes every
        // recorded vkCmd* into Metal inside vkQueueSubmit, and Apple's GL
        // charges ~0.5us of CPU per sub-draw inside
        // glMultiDrawElementsBaseVertex — so the ratio between the two plots
        // is the merge's payoff, and Draws/Merged flat against QueueSubmit
        // means per-command translation is the ceiling.
        PROFILE_PLOT("Draws/Chunk", static_cast<int64_t>(layerCount));
        PROFILE_PLOT("Draws/Merged", static_cast<int64_t>(subDraws));

        switch (layer) {
            case RenderLayer::Opaque:      m_stats.opaqueSections = layerCount;      m_stats.opaqueDraws = subDraws;      break;
            case RenderLayer::Cutout:       m_stats.cutoutSections = layerCount;      m_stats.cutoutDraws = subDraws;      break;
            case RenderLayer::Translucent:  m_stats.translucentSections = layerCount; m_stats.translucentDraws = subDraws; break;
        }
    }

    // Flush the accumulated runs of one slab as a single multi-draw. Slab
    // indices are absolute, so every run draws with baseVertex 0 and the
    // Uint32 index type — see ChunkMegaBuffer::INDEX_SIZE.
    static inline void FlushSlabRuns(ChunkMegaBuffer& megaBuffer, uint32_t slab,
                                     std::vector<int32_t>& counts, std::vector<size_t>& byteOffsets,
                                     std::vector<int32_t>& zeroBaseVertices,
                                     RenderStats& stats, int& subDraws) {
        if (counts.empty()) return;
        if (zeroBaseVertices.size() < counts.size()) zeroBaseVertices.resize(counts.size(), 0);
        megaBuffer.BindSlab(slab);
        g_renderBackend->MultiDrawIndexedBaseVertex(
            counts.data(), byteOffsets.data(), zeroBaseVertices.data(),
            static_cast<uint32_t>(counts.size()), IndexType::Uint32);
        stats.totalDrawCalls++;
        subDraws += static_cast<int>(counts.size());
        counts.clear();
        byteOffsets.clear();
    }

    // Opaque / cutout: order is free, so sections are sorted by slab and
    // index offset and consecutive ranges fused into one sub-draw whenever
    // the gap between them is small enough AND safe to draw across
    // (ChunkMegaBuffer::IsIndexGapDrawable — never over a just-freed range
    // that still holds its old mesh). Whatever sits in a bridged gap is drawn
    // too: a frustum-culled neighbour (wasted vertex work, invisible) or
    // zeroed free space (degenerate triangles, nothing). One multi-draw per
    // slab, as before; what changed is the number of commands inside it.
    int ChunkRenderer::SubmitMergedRuns(ChunkMegaBuffer& megaBuffer) {
        // Sections of one slab are visited in list (distance) order, so this
        // is a genuine sort each frame — a few thousand 16-byte entries,
        // measured in tens of microseconds, against the driver's per-command
        // cost it removes.
        std::sort(m_drawEntries.begin(), m_drawEntries.end(),
                  [](const DrawEntry& a, const DrawEntry& b) {
                      return a.slab != b.slab ? a.slab < b.slab : a.offset < b.offset;
                  });

        // Gap bridging draws whatever sits between two nearby visible runs —
        // usually a culled section — because one long draw beats two short
        // ones. That is invisible in normal play (the extra sections are
        // outside the frustum) but it is exactly what the F+C free camera
        // exists to inspect, and it reads as "frustum culling is broken":
        // dozens of scattered behind-camera sections. So while the cull
        // override (free cam) or the greedy debug view is active, draw the
        // EXACT visible set; runs still fuse when truly contiguous.
        const bool merge = m_drawMergeEnabled && !DrawMergeDisabled() && !m_cullOverrideActive && !m_greedyMeshDebug;
        int subDraws = 0;
        m_runCounts.clear();
        m_runByteOffsets.clear();
        // Runs built by THIS call — validated below, and copied to
        // m_lastOpaqueRuns for F8. (The first recorder was cross-layer
        // polluted: cutout's runs overwrote opaque's before F8 read them,
        // yielding a false submitted=0 on 2026-08-31.)
        m_callRuns.clear();

        const size_t n = m_drawEntries.size();
        size_t i = 0;
        while (i < n) {
            const uint32_t slab = m_drawEntries[i].slab;
            size_t runBegin = m_drawEntries[i].offset;
            size_t runEnd   = runBegin + m_drawEntries[i].count;
            ++i;
            for (; i < n && m_drawEntries[i].slab == slab; ++i) {
                const DrawEntry& e = m_drawEntries[i];
                const size_t eEnd = static_cast<size_t>(e.offset) + e.count;
                // Live regions are disjoint and sorted, so e.offset >= runEnd
                // and the gap is [runEnd, e.offset). A duplicate entry (offset
                // below runEnd) has an empty gap and simply folds in.
                const size_t gapTol = m_gapBridgingEnabled ? kDrawMergeGapIndices : 0;
                const bool fuse = merge &&
                                  e.offset <= runEnd + gapTol &&
                                  megaBuffer.IsIndexGapDrawable(slab, runEnd, e.offset);
                if (fuse) {
                    runEnd = std::max(runEnd, eEnd);
                } else {
                    m_runCounts.push_back(static_cast<int32_t>(runEnd - runBegin));
                    m_runByteOffsets.push_back(runBegin * ChunkMegaBuffer::INDEX_SIZE);
                    m_callRuns.push_back({static_cast<size_t>(slab), runBegin, runEnd});
                    runBegin = e.offset;
                    runEnd   = eEnd;
                }
            }
            m_runCounts.push_back(static_cast<int32_t>(runEnd - runBegin));
            m_runByteOffsets.push_back(runBegin * ChunkMegaBuffer::INDEX_SIZE);
            m_callRuns.push_back({static_cast<size_t>(slab), runBegin, runEnd});
            FlushSlabRuns(megaBuffer, slab, m_runCounts, m_runByteOffsets, m_zeroBaseVertices, m_stats, subDraws);
        }

        // ── Loss + poison validators (2026-08-31 hole hunt) ──────────────────
        // The merge loop is lossless by construction (brute-verified over 200k
        // adversarial cases), so if holes appear with merging on, either this
        // loss check fires — handing us the exact dropped entry — or the gap
        // CONTENT is poisoned, which the byte sampler below looks for: any
        // bridged-gap index beyond the slab's vertex capacity is garbage that
        // was never zeroed (Vulkan only; slabs are persistently mapped).
        static uint32_t s_validateCounter = 0;
        if (merge && !m_callRuns.empty() && (++s_validateCounter & 127u) == 0) {
            int lost = 0;
            for (const DrawEntry& e : m_drawEntries) {
                bool covered = false;
                for (const auto& r : m_callRuns) {
                    if (r[0] == e.slab && r[1] <= e.offset &&
                        static_cast<size_t>(e.offset) + e.count <= r[2]) { covered = true; break; }
                }
                if (!covered && ++lost <= 3) {
                    Log::Warning("[MergeLoss] entry slab=%u off=%u cnt=%u NOT covered by %zu runs",
                                 e.slab, e.offset, e.count, m_callRuns.size());
                }
            }
            if (lost > 0)
                Log::Warning("[MergeLoss] %d of %zu entries lost this call", lost, m_drawEntries.size());

            // Gap byte sampler: walk consecutive sorted entries; for each
            // bridged gap sample up to 8 indices from the mapped IBO.
            static int s_poisonLogBudget = 20;
            if (s_poisonLogBudget > 0) {
                for (size_t k = 1; k < m_drawEntries.size(); ++k) {
                    const DrawEntry& a2 = m_drawEntries[k - 1];
                    const DrawEntry& b2 = m_drawEntries[k];
                    if (a2.slab != b2.slab) continue;
                    const size_t gapB = static_cast<size_t>(a2.offset) + a2.count;
                    const size_t gapE = b2.offset;
                    if (gapE <= gapB) continue;
                    const auto* idx = static_cast<const uint32_t*>(
                        g_renderBackend->DebugGetMappedBufferPtr(megaBuffer.DebugGetSlabIbo(a2.slab)));
                    if (!idx) break;   // GL: no mapping
                    const size_t cap = megaBuffer.GetTotalVertexCapacity();
                    const size_t nS = std::min<size_t>(gapE - gapB, 8);
                    for (size_t t = 0; t < nS; ++t) {
                        if (idx[gapB + t] > cap) {
                            Log::Warning("[MergePoison] slab=%u gap[%zu,%zu) idx[%zu]=%u > cap %zu",
                                         a2.slab, gapB, gapE, gapB + t, idx[gapB + t], cap);
                            if (--s_poisonLogBudget <= 0) break;
                        }
                    }
                    if (s_poisonLogBudget <= 0) break;
                }
            }
        }
        m_lastOpaqueRuns = m_callRuns;
        return subDraws;
    }

    // Translucent: m_drawEntries is in back-to-front order and the GPU has to
    // see exactly that order — blending is not commutative. Bucketing by slab
    // (what the old path did) silently reordered sections across slabs, so a
    // near section living in slab 0 could be drawn before a far one in slab 1
    // and blend on top of nothing. Now a run of consecutive same-slab entries
    // becomes one multi-draw, in list order (a multi-draw's sub-draws execute
    // in array order on both backends), and the slab is rebound whenever it
    // changes. The only fusing done is the trivially safe kind: the next
    // entry starts exactly where the current run ends, so the fused range
    // draws the same indices in the same order as two separate draws would.
    int ChunkRenderer::SubmitOrderedRuns(ChunkMegaBuffer& megaBuffer) {
        // Translucent submission. Strict global back-to-front emission (flush
        // on every slab change) shattered into thousands of driver calls the
        // moment sections interleaved across slabs: measured 26 ms/frame on
        // Apple's GL at a 19k-section sky view, one glMultiDrawElements call
        // per run. Group runs per slab instead — back-to-front order is exact
        // WITHIN each slab (sub-draws execute in array order on both
        // backends), one multi-draw per slab. Cross-slab order is sacrificed,
        // which is the pre-merge behaviour this pass always had; slabs are
        // submitted in first-seen (nearest-section-last) order to keep the
        // common single-slab case exact.
        int subDraws = 0;
        const uint32_t slabCount = megaBuffer.GetSlabCount();
        m_slabRunCounts.resize(slabCount);
        m_slabRunOffsets.resize(slabCount);
        for (uint32_t s2 = 0; s2 < slabCount; ++s2) {
            m_slabRunCounts[s2].clear();
            m_slabRunOffsets[s2].clear();
        }

        uint32_t slab     = m_drawEntries[0].slab;
        size_t   runBegin = m_drawEntries[0].offset;
        size_t   runEnd   = runBegin + m_drawEntries[0].count;
        auto pushRun = [&]() {
            if (slab < slabCount) {
                m_slabRunCounts[slab].push_back(static_cast<int32_t>(runEnd - runBegin));
                m_slabRunOffsets[slab].push_back(runBegin * ChunkMegaBuffer::INDEX_SIZE);
            }
        };

        const size_t n = m_drawEntries.size();
        for (size_t i = 1; i < n; ++i) {
            const DrawEntry& e = m_drawEntries[i];
            if (e.slab == slab && e.offset == runEnd) {
                runEnd += e.count;          // contiguous and ascending: same bytes, same order
            } else {
                pushRun();
                slab     = e.slab;
                runBegin = e.offset;
                runEnd   = runBegin + e.count;
            }
        }
        pushRun();
        for (uint32_t s2 = 0; s2 < slabCount; ++s2) {
            if (m_slabRunCounts[s2].empty()) continue;
            FlushSlabRuns(megaBuffer, s2, m_slabRunCounts[s2], m_slabRunOffsets[s2],
                          m_zeroBaseVertices, m_stats, subDraws);
        }
        return subDraws;
    }

    // MC's layout: each section owns its index buffer, so the layer cannot be
    // one multi-draw — one bind + draw per section, in list order. Nothing
    // uses this mode today (translucent was switched back to the shared IBO;
    // see ClientMeshManager::Initialize), kept correct so the option stays
    // real. Per-section IBOs hold absolute indices too, hence baseVertex 0.
    int ChunkRenderer::SubmitPerSectionIbos(ChunkMegaBuffer& megaBuffer) {
        int subDraws = 0;
        uint32_t boundSlab = UINT32_MAX;
        for (const DrawEntry& e : m_drawEntries) {
            if (e.ibo == INVALID_BUFFER) continue;
            if (e.slab != boundSlab) {
                megaBuffer.BindSlab(e.slab);   // VBO only in this mode
                boundSlab = e.slab;
            }
            g_renderBackend->BindIndexBuffer(e.ibo);
            g_renderBackend->DrawIndexedBaseVertex(e.count, 0, 0, IndexType::Uint32);
            m_stats.totalDrawCalls++;
            ++subDraws;
        }
        return subDraws;
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
        const bool debugLinePhase = m_greedyMeshDebug && !m_debugFillPhase;
        state.polygonMode = (m_wireframeMode || debugLinePhase) ? PolygonMode::Line : PolygonMode::Fill;
        if (m_greedyMeshDebug && m_debugFillPhase) {
            // Push the grey fill slightly away so the line phase, drawn at
            // true depth, cleanly wins the depth test everywhere.
            state.depthBiasEnabled  = true;
            state.depthBiasConstant = 2.0f;
            state.depthBiasSlope    = 1.0f;
        }
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

    // Global utility functions (act on the bound renderer)
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
