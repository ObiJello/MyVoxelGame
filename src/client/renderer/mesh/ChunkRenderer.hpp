// File: src/client/renderer/mesh/ChunkRenderer.hpp
#pragma once

#include "ClientMeshManager.hpp"
#include "Mesher.hpp"
#include "../core/Camera.hpp"
#include "../core/Frustum.hpp"
#include "../culling/SectionOcclusionGraph.hpp"
#include "../shader/Shader.hpp"
#include "../texture/TextureAnimator.hpp"
#include "../backend/RenderTypes.hpp"
#include <climits>
#include <cstdint>
#include <functional>
#include <vector>
#include <chrono>
#include <array>
#include <memory>
#include <unordered_set>

namespace Render {

    class ChunkMegaBuffer;

    // Render pass configuration
    struct RenderPassConfig {
        bool enableDepthWrite = true;
        bool enableDepthTest = true;
        bool enableBlending = false;
        bool enableAlphaTest = false;
        bool enableBackFaceCulling = true;

        // Blending settings (for translucent pass)
        BlendFactor blendSrc = BlendFactor::SrcAlpha;
        BlendFactor blendDst = BlendFactor::OneMinusSrcAlpha;

        // Alpha test threshold (for cutout pass)
        float alphaThreshold = 0.5f;

        // Render order
        bool frontToBack = true;  // For opaque/cutout (depth buffer optimization)
                                  // Will be set to false for translucent (back to front)
    };

    // Layer presence bitmask flags
    enum LayerFlag : uint8_t {
        LayerOpaque      = 1 << 0,
        LayerCutout      = 1 << 1,
        LayerTranslucent = 1 << 2,
    };

    // A section's IDENTITY in the reachable/visible lists.
    //
    // Deliberately carries no mesh state. MC's visibleSections holds bare
    // RenderSection object references and reads the compiled mesh LIVE at draw
    // time (LevelRenderer.prepareChunkRenders:1003 -> getBuffers(layer):1009);
    // a null result simply contributes nothing. We do the same by resolving
    // through (chunkPos, sectionY) each frame.
    //
    // This used to cache a GPUSectionData* and a layerMask captured when the
    // occlusion graph was built. Those went stale the moment a section was
    // remeshed, emptied or unloaded, which is why a full BFS rebuild had to be
    // forced on every mesh upload just to garbage-collect the list — and why
    // removing that rebuild made Sections/Reachable climb to 5652 and never
    // recover. Identity cannot go stale, so none of that is needed now.
    struct SectionRenderData {
        ::Game::Math::ChunkPos chunkPos;
        int sectionY;
        float distanceToCamera;
        // MC's `isClose` (SectionOcclusionGraph.addSectionsInFrustum:80-89).
        // Set by the PER-FRAME frustum filter, deliberately not by the BFS:
        // distanceToCamera is baked when the reachable list is snapshotted and
        // that list survives across frames, so it goes stale by up to a
        // section of camera movement — which is most of the 32-block radius.
        bool nearby = false;

        // NON-PERSISTENT. Valid only for the duration of the RenderAll that
        // resolved it, and only in m_visibleSections. Never read it from a
        // list that survived a frame boundary; that is the exact bug this
        // redesign removes.
        const GPUSectionData* resolved = nullptr;

        // Resolution cache, on the ReachableCacheSlot entries. Live resolution
        // costs an unordered_map find per visible section per frame; that is
        // thousands of hash lookups for a list whose answer only changes when
        // a mesh is uploaded or destroyed. So the answer is remembered with
        // the ClientMeshManager GPU-data generation it was fetched at: equal
        // stamp, same answer, no lookup. Not a stale-pointer risk — the
        // generation is bumped before any GPUSectionData dies, so a matching
        // stamp is proof the pointer is live. `resolved` is what the frame
        // reads; this is only how it gets filled.
        const GPUSectionData* cachedGpu = nullptr;
        uint64_t cacheGeneration = 0;

        SectionRenderData(::Game::Math::ChunkPos pos, int secY, float dist)
            : chunkPos(pos), sectionY(secY), distanceToCamera(dist) {}
    };

    // Render statistics for debugging
    struct RenderStats {
        // Per-frame counters
        int sectionsRendered = 0;
        int sectionsSkipped = 0;  // Culled by frustum
        int sectionsAvailable = 0;  // Total sections checked before culling
        int totalDrawCalls = 0;

        // Per-layer counters: sections that contributed geometry...
        int opaqueSections = 0;
        int cutoutSections = 0;
        int translucentSections = 0;
        // ...and the sub-draws they were issued as after run merging. The
        // ratio is the merge win; draws is what the driver's per-command
        // cost scales with.
        int opaqueDraws = 0;
        int cutoutDraws = 0;
        int translucentDraws = 0;

        // Geometry statistics
        size_t totalVerticesRendered = 0;
        size_t totalIndicesRendered = 0;

        // NEW: Optimized build phase timings
        float buildDrawListsTimeMs = 0.0f;     // Total time to build draw lists (lock-free!)
        float chunkIterationTimeMs = 0.0f;     // Time iterating chunks
        float gpuDataLoadTimeMs = 0.0f;        // Time loading atomic pointers
        float frustumCullingTimeMs = 0.0f;     // Time for frustum tests
        float sortingTimeMs = 0.0f;            // Time sorting draw items
        
        // Individual render pass timings (ONLY the actual drawing)
        float opaquePassTimeMs = 0.0f;         // Time for opaque drawing only
        float cutoutPassTimeMs = 0.0f;         // Time for cutout drawing only
        float translucentPassTimeMs = 0.0f;    // Time for translucent drawing only
        
        // Total render time
        float renderTimeMs = 0.0f;              // Total render time (build + all passes)

        // GPU-side timing (from GL_TIME_ELAPSED queries, 1-2 frame latency)
        float gpuOpaqueTimeMs = 0.0f;
        float gpuCutoutTimeMs = 0.0f;
        float gpuTranslucentTimeMs = 0.0f;
        float gpuTotalTimeMs = 0.0f;

        void Reset() {
            sectionsRendered = sectionsSkipped = sectionsAvailable = totalDrawCalls = 0;
            opaqueSections = cutoutSections = translucentSections = 0;
            opaqueDraws = cutoutDraws = translucentDraws = 0;
            totalVerticesRendered = totalIndicesRendered = 0;
            buildDrawListsTimeMs = chunkIterationTimeMs = gpuDataLoadTimeMs = 0.0f;
            frustumCullingTimeMs = sortingTimeMs = 0.0f;
            opaquePassTimeMs = cutoutPassTimeMs = translucentPassTimeMs = 0.0f;
            renderTimeMs = 0.0f;
            gpuOpaqueTimeMs = gpuCutoutTimeMs = gpuTranslucentTimeMs = gpuTotalTimeMs = 0.0f;
        }
    };

    // Main chunk renderer - handles three-layer rendering pipeline
    class ChunkRenderer {
    public:
        ChunkRenderer();
        ~ChunkRenderer();

        // Initialize renderer with shaders, bound to the chunk and mesh
        // managers of the SAME level (never the globals — see ClientLevel.hpp).
        bool Initialize(Client::ClientChunkManager* chunks, ClientMeshManager* meshes);
        void Shutdown();

        // Convenience method to render all layers. Computes projection
        // internally using the standard glm::perspective(fov, aspect, ...).
        void RenderAll(const Camera& camera, const Frustum& frustum);

        // Variant for callers that need a NON-standard projection — e.g.
        // the portal renderer's see-through pass uses an oblique near-plane
        // projection so the destination wall block doesn't occlude the
        // destination room from the virtual camera. The override is used as
        // the projection matrix for ALL render passes; pass the same matrix
        // you used to derive `frustum`.
        // `exactProjection`: use the matrix on every backend. The default
        // (false) lets Vulkan build its own perspective instead — the
        // portal pass's OBLIQUE matrix compresses depth past the far plane
        // there (see the note at the use), and it clips with
        // gl_ClipDistance instead. A plain perspective with a different
        // aspect or field of view — the panorama capture's square 90° —
        // has no such problem and needs the matrix honoured.
        void RenderAll(const Camera& camera, const Frustum& frustum,
                       const glm::mat4& projectionOverride, bool exactProjection = false);

        // The translucent pass RenderAll held back. RenderAll draws the
        // opaque and cutout passes and parks the translucent one here; the
        // caller draws its entities and block entities, then calls this.
        // That is MC LevelRenderer's order — solid terrain, entities, block
        // entities, translucent terrain — and the reason it matters is that
        // the translucent pass writes depth: drawn first, water and glass
        // hid every mob and player behind them. No-op when nothing is
        // pending (a second call, or a RenderAll with no translucent
        // sections in view). Greedy-debug mode draws inline instead.
        void RenderDeferredTranslucent();

        // Configuration - **UPDATED**: Now reads from game settings
        void RefreshSettings(); // Call when settings change

        void SetEnableFrustumCulling(bool enable) { m_enableFrustumCulling = enable; }
        bool IsEnabledFrustumCulling() const { return m_enableFrustumCulling; }
        void SetEnableSmartCull(bool enable) { m_enableSmartCull = enable; m_visibleSectionsDirty = true; }
        bool IsEnabledSmartCull() const { return m_enableSmartCull; }

        // Statistics
        const RenderStats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats.Reset(); }

        // Mark visible sections as dirty (call when sections are uploaded/removed)
        void MarkVisibleSectionsDirty() { m_visibleSectionsDirty = true; }

        // A section's mesh just reached the GPU — MC's
        // LevelRenderer.addRecentlyCompiledSection. Makes it a propagation
        // source for next frame's incremental occlusion-graph update.
        // MAIN THREAD ONLY (called from the mesh upload drain).
        // `mayRequestRebuild`: see SectionOcclusionGraph::PendingSource —
        // true only for block edits.
        //
        // Every event also advances the propagation epoch: a cached slot the
        // live graph is NOT anchored to never receives these events, so a slot
        // built before the latest one is out of date even when no world-
        // version bump happened (see IsSlotFresh).
        void SchedulePropagationFrom(::Game::Math::ChunkPos chunkPos, int sectionY,
                                     bool mayRequestRebuild = false) {
            m_occlusionGraph.SchedulePropagationFrom(chunkPos, sectionY, mayRequestRebuild);
            ++m_propagationEpoch;
        }

        // Post-BFS, post-frustum list for the MAIN camera — MC's
        // LevelRenderer.visibleSections (getVisibleSections():1356). Holds every
        // reachable non-air section, meshed or not, exactly as MC's octree walk
        // does; `resolved` is null for the ones without a mesh yet.
        //
        // This is the scheduler's candidate source (MC compileSections:1136).
        // It is a SNAPSHOT, not m_visibleSections, because the portal pass
        // overwrites the latter with a virtual camera's view — see the frustum
        // filter in PrepareVisibleSections.
        //
        // One frame stale: PrepareVisibleSections runs in the Render phase,
        // after MeshSchedule. MC culls and compiles back-to-back in renderLevel.
        const std::vector<SectionRenderData>& GetMainViewSections() const { return m_mainViewSections; }
        // Sections drawn by views through portals INTO THIS SAME LEVEL (a
        // wrap border shows the level to itself) since the last main pass —
        // every portal recursion's list, appended. The scheduler walks these
        // after the main view's: without it the far side of a wrap border
        // was never compiled (it lies a world's width from the player, so
        // no main view ever reaches it) and showed as fog until crossed.
        // Cleared by the main pass; one frame stale like the main list.
        const std::vector<SectionRenderData>& GetPortalViewSections() const { return m_portalViewSections; }
        // A level seen only through portals has no main pass to clear its
        // list: whoever schedules its meshes (once per frame, after every
        // view of it has been drawn) clears it.
        void ClearPortalViewSections() {
            m_portalViewSections.clear(); m_portalViewGrid.Clear();
        }
        // Cull one view and hand its sections to the mesh scheduler as a
        // portal view, drawing nothing. The last-world panorama's prewarm:
        // one of its six faces a frame while the pause menu is up, so every
        // direction is meshed before "Save and Quit". Overwrites the
        // visible-section list, so it belongs after the frame's last draw
        // that reads it.
        void RecordViewForScheduler(const Camera& camera, const Frustum& frustum);
        // Of the main view's visible sections this frame, how many still
        // have a mesh outstanding (dirty, or a build in flight). Zero for a
        // few frames running means the view on screen is complete — what
        // the join transition waits for before it hands the panorama over.
        int MainViewSectionsPending() const { return m_mainViewPending; }
        // The chunk fade-in (SectionFade.hpp) off for the draws that follow:
        // the leave capture's panorama faces are built from sections its
        // warm-up has only just meshed, and a picture of the world must
        // not carry them half-faded. The world's own view keeps fading.
        void SetSectionFadeSuppressed(bool suppressed) { m_fadeSuppressed = suppressed; }
        // Membership in the main view's list / the same-level portal views'
        // lists (one lookup each). The mesh scheduler asks these for every
        // DIRTY section instead of walking the ~4,000-section visible list
        // asking each "are you dirty?" — proportional to what changed, not
        // to what is on screen.
        bool IsMainViewSection(::Game::Math::ChunkPos chunkPos, int sectionY) const {
            return m_mainViewGrid.HasSection(chunkPos, sectionY);
        }
        bool IsPortalViewSection(::Game::Math::ChunkPos chunkPos, int sectionY) const {
            return m_portalViewGrid.HasSection(chunkPos, sectionY) || m_shadowViewGrid.HasSection(chunkPos, sectionY);
        }
        // Column-level pre-test for the same question: a streamed-in chunk
        // holds 24 dirty sections, and while flying most of them are behind
        // the player, where the scheduler asked both section questions for
        // every one of them every frame (36% of its time, Instruments
        // 2026-09-04). One lookup per column answers "no" for all 24.
        bool IsMainViewColumn(::Game::Math::ChunkPos chunkPos) const {
            return m_mainViewGrid.HasColumn(chunkPos);
        }
        bool IsPortalViewColumn(::Game::Math::ChunkPos chunkPos) const {
            return m_portalViewGrid.HasColumn(chunkPos) || m_shadowViewGrid.HasColumn(chunkPos);
        }

        // The camera jumped — a same-level portal crossing (a wrap border)
        // put it a world's width from where it was. The reachable-set slots
        // are keyed by camera section and the render source falls back to
        // the most recently used slot while the new section's BFS is in
        // flight; after a jump that slot describes the far side of the
        // world, and the frame or two it was drawn from were the flash on
        // every crossing. Drop them all: the next pass rebuilds synchronously
        // (the cold-start path) and draws right.
        void OnCameraTeleport() {
            for (auto& s : m_reachableSlots) s.valid = false;
            m_visibleSectionsDirty = true;
        }
        // The sections of the view being drawn RIGHT NOW — the main view's,
        // or a portal view's while one is rendering. Renderers that gather
        // per pass (block entities) read this, so what a portal shows is
        // gathered from the portal's own view rather than the main camera's.
        const std::vector<SectionRenderData>& GetVisibleSections() const { return m_visibleSections; }
        // For the F3 chunk-culling renderers (section paths / visibility).
        const SectionOcclusionGraph& OcclusionGraph() const { return m_occlusionGraph; }

        // Is (chunkPos, sectionY) in the draw list of the MOST RECENT
        // PrepareVisibleSections — post-BFS, post-frustum, for whichever
        // camera that pass ran with (the main view, or a portal recursion's
        // virtual camera)? The entity and block-entity passes ask this right
        // after the chunk pass of the same view, which is exactly the list
        // they want; it is MC's isSectionCompiledAndVisible gate on
        // extractVisibleEntities, tightened by the occlusion BFS.
        //
        // Answers "in the list" only. A section that is not in the list may be
        // culled OR simply absent (all-air sections are never listed, unloaded
        // chunks have no sections) — callers that need to tell those apart
        // consult ClientChunkManager (see Render::EntityCulling).
        // MAIN THREAD ONLY, like the list it mirrors.
        bool IsSectionVisible(::Game::Math::ChunkPos chunkPos, int sectionY) const {
            return m_visibleGrid.HasSection(chunkPos, sectionY);
        }

        // ── Debug cull override (detached free camera, F+C) ─────────────
        // While set, PrepareVisibleSections runs ENTIRELY from the given
        // camera + frustum: the occlusion-BFS origin/cache key, the per-frame
        // frustum filter, the front-to-back/translucent sort origin, and the
        // IsSectionVisible set the entity passes consult all come from the
        // override view. BindSharedRenderState/RenderAll still build the MVP
        // from the camera passed to RenderAll, so the scene is DRAWN from one
        // viewpoint while being CULLED from another — which is the whole
        // point: fly outside the frozen player's frustum and watch what the
        // culler actually kept.
        //
        // The mesh scheduler snapshot (GetMainViewSections) is taken from the
        // same pass, so with the override anchored at the player, detaching
        // the render camera cannot perturb mesh scheduling either.
        //
        // Set immediately before the MAIN RenderChunksAll and cleared right
        // after it — portal see-through re-entries must keep culling from
        // their own virtual cameras.
        void SetCullOverride(const Camera& camera, const Frustum& frustum) {
            m_cullCamera         = camera;
            m_cullFrustum        = frustum;
            m_cullOverrideActive = true;
        }
        void ClearCullOverride() { m_cullOverrideActive = false; }

        // While set, PrepareVisibleSections records its visible list as this
        // renderer's MAIN view even under a projection override. For a level
        // rendered only through a portal, the portal view IS its main view,
        // and the mesh scheduler needs that list to compile from.
        void SetRecordMainView(bool on) { m_recordMainView = on; }

        // ── Portal-view occlusion seed ─────────────────────────────────
        // A view through a portal has its camera wherever the portal's
        // transform put it — usually inside rock behind the far surface —
        // so an occlusion BFS from the CAMERA sees nothing. The Immersive
        // Portals mod seeds its chunk culling at the portal's destination
        // instead; this is that seed: a point just in front of the far
        // surface, in air. While set (around a projection-override render),
        // PrepareVisibleSections runs the ordinary cached, asynchronous BFS
        // from it, keyed by the seed's section, and the frustum-only sweep
        // is only the fallback for the frame or two before the first result
        // lands. Without it a portal view drew EVERY section in its frustum,
        // which in the Nether — solid terrain in every direction — was
        // thousands of sections per view, and a chain of nested views ran
        // the frame into seconds.
        void SetPortalViewSeed(const glm::vec3& seed) { m_portalSeed = seed; m_portalSeedActive = true; }
        void ClearPortalViewSeed() { m_portalSeedActive = false; }

        // ── Directional (shadow-map) view ───────────────────────────────
        // A shader pack's shadow pass draws the terrain from the sun with an
        // orthographic projection. While set, a projection-override RenderAll
        // is that view: discovery is frustum-only (the light's ortho box)
        // over the render-distance disc around `centre` (the player, not the
        // light's virtual eye, which sits 100 blocks away along the light),
        // the face-direction groups are chosen by `toLight` (a face is a
        // shadow caster iff its normal faces the light — a point test from
        // the eye is wrong for a parallel light), and the sections found are
        // remembered for the mesh scheduler (IsPortalViewSection) so that
        // casters behind the player get meshed too — MC compiles every
        // chunk in range whether or not the player looks at it, and a
        // shadow map is only as complete as the meshes it can draw.
        void SetDirectionalView(const glm::vec3& toLight, const glm::dvec3& centre) {
            m_directionalActive = true;
            m_directionalToLight = toLight;
            m_directionalCentre  = centre;
        }
        void ClearDirectionalView() {
            m_directionalActive = false;
            m_shadowViewGrid.Clear();
        }
        // After the shadow pass: the view is over, its sections stay for the
        // scheduler until the next pass (or ClearDirectionalView).
        void ClearDirectionalViewKeepSections() { m_directionalActive = false; }

        // Call when a GPUSectionData object is ERASED (chunk unload, section
        // remeshed to empty, mesh-manager shutdown) — cached reachable lists
        // and any in-flight async BFS result hold raw pointers into those
        // objects, so they must all be discarded, not just refreshed. The
        // next PrepareVisibleSections invalidates every slot, bumps the erase
        // token (dropping stale async results on arrival), and rebuilds
        // synchronously. Dirty-only events (uploads) keep the async path.
        void MarkSectionDataErased() { m_sectionDataErased = true; }


        // Debug rendering options
        void SetWireframeMode(bool enable);
        // Greedy-mesh debug view: terrain draws as untextured lines (1x1 white
        // texture + PolygonMode::Line), so merged quads read as large
        // triangles and the grouping — and its reaction to block edits — is
        // visible directly. Toggled from the Render Controls panel; the
        // OBEY_GREEDY_DEBUG env var forces it on from launch for harness use.
        // Also flips the mesher's debug coloring and triggers a full remesh,
        // so the view applies immediately and updates on block edits.
        void SetGreedyMeshDebug(bool enable);
        void GetGreedyTotals(uint64_t& eligibleIn, uint64_t& rectsOut) const;

        // Live culprit-isolation toggles (Render Controls): flip while staring
        // at a rendering artifact to convict or clear a subsystem in place.
        void SetDrawMergeEnabled(bool enable) { m_drawMergeEnabled = enable; }
        void SetGapBridgingEnabled(bool enable) { m_gapBridgingEnabled = enable; }
        bool IsGapBridgingEnabled() const { return m_gapBridgingEnabled; }
        bool IsDrawMergeEnabled() const { return m_drawMergeEnabled; }
        void SetGreedyMeshingEnabled(bool enable);   // remeshes the world
        bool IsGreedyMeshingEnabled() const;

        // Shader-pack terrain passes (Render::ShaderPipeline): a pack program
        // drawn in place of the engine's for a pass, and the render target
        // bound before it (the pack's gbuffer set). The same uniforms are
        // set on the pack program — its translation declares them. A hook
        // runs before the translucent pass (the pack's depthtex1 copy).
        // Everything here is inert while no pack is loaded.
        enum TerrainPass { kPassOpaque = 0, kPassCutout = 1, kPassTranslucent = 2 };
        struct PassOverride {
            ShaderHandle       shader = INVALID_SHADER;
            RenderTargetHandle target = INVALID_RENDER_TARGET;
        };
        void SetPassOverride(TerrainPass pass, PassOverride o) { m_passOverride[pass] = o; }
        void ClearPassOverrides();
        void SetBeforeTranslucentHook(std::function<void()> fn) { m_beforeTranslucent = std::move(fn); }
        // Bound again once the three passes are done, so what the frame
        // draws next (entities, clouds) lands in the pack's colour + depth
        // rather than the last pass's gbuffer set.
        void SetAfterPassesTarget(RenderTargetHandle rt) { m_afterPassesTarget = rt; }

        // Occlusion/frustum readout for the debug panel: sections the BFS
        // reached from the (cull) camera, and how many survived the frustum.
        uint32_t GetLastReachableCount() const { return m_lastReachableCount; }
        uint32_t GetLastVisibleCount() const { return m_lastVisibleCount; }
        // Where the most recent PrepareVisibleSections got its reachable
        // set: the slot for the camera's own section, another (nearby)
        // slot while that one's BFS is in flight, a synchronous cold-start
        // rebuild, or no BFS at all (a portal view by frustum). For the
        // flicker diagnostics: a view drawn from a fallback slot for a
        // frame looks different from the frames around it.
        enum class PrepareSource : uint8_t { Exact = 0, Fallback = 1, ColdSync = 2, FrustumOnly = 3 };
        PrepareSource LastPrepareSource() const { return m_lastPrepareSource; }
        bool IsGreedyMeshDebug() const { return m_greedyMeshDebug; }
        void SetShowSectionBounds(bool enable) { m_showSectionBounds = enable; }
        void SetDebugLayer(int layer) { m_debugLayer = layer; } // -1 = all, 0 = opaque, 1 = cutout, 2 = translucent

    private:
        // Shader management
        std::unique_ptr<Shader> m_blockShader;
        bool m_shadersLoaded = false;

        // Backend shader handles — separate programs for opaque (no discard, enables
        // early-z) and cutout/translucent (with discard for alpha testing).
        // Matches Minecraft's SOLID_TERRAIN vs CUTOUT_TERRAIN pipeline split.
        ShaderHandle m_backendShader = INVALID_SHADER;       // Legacy (unused, kept for compat)
        ShaderHandle m_opaqueShader = INVALID_SHADER;        // block.vert + block_opaque.frag (minimal discard)
        ShaderHandle m_cutoutShader = INVALID_SHADER;        // block.vert + block.frag (alpha discard)
        ShaderHandle m_solidShader = INVALID_SHADER;         // block.vert + block_solid.frag (zero discard)
        ShaderHandle m_activeShader = INVALID_SHADER;        // Currently bound shader
        PassOverride m_passOverride[3];
        std::function<void()> m_beforeTranslucent;
        RenderTargetHandle m_afterPassesTarget = INVALID_RENDER_TARGET;

        // The translucent pass parked by the last RenderAll (see
        // RenderDeferredTranslucent): the cameras it was prepared with and
        // the projection override that was in force.
        struct DeferredTranslucent {
            bool      pending = false;
            bool      gpuTiming = false;
            Camera    camera;
            Camera    cullCamera;
            Frustum   cullFrustum{};
            bool      useProjectionOverride = false;
            bool      projectionOverrideExact = false;
            glm::mat4 projectionOverride{1.0f};
        };
        DeferredTranslucent m_deferredTranslucent;
        // The translucent pass proper — shader bind, uniforms, the pack's
        // before-translucent hook, the sorted draw. Shared by the inline
        // (debug) and deferred paths.
        void DrawTranslucentPass(const Camera& camera, const Camera& cullCamera,
                                 const Frustum& cullFrustum, bool gpuTiming);
        ShaderHandle PassShader(TerrainPass pass, ShaderHandle engine) const {
            return m_passOverride[pass].shader != INVALID_SHADER ? m_passOverride[pass].shader : engine;
        }
        void BindPassTarget(TerrainPass pass);
        TextureHandle m_backendAtlasTexture = INVALID_TEXTURE;
        glm::mat4 m_cachedMVP{1.0f};                         // Cached for shader switches

        // Projection override — used by the portal renderer's see-through
        // pass to inject an oblique near-plane projection. When
        // m_useProjectionOverride is true, RenderAll uses m_projectionOverride
        // instead of computing its own glm::perspective; reset to false
        // automatically by the (camera, frustum, projection) overload after
        // the call returns.
        bool      m_useProjectionOverride = false;
        bool      m_projectionOverrideExact = false;   // honour it on Vulkan too
        bool      m_fadeSuppressed = false;            // see SetSectionFadeSuppressed
        int       m_mainViewPending = 0;               // see MainViewSectionsPending
        glm::mat4 m_projectionOverride{1.0f};
        bool      m_recordMainView = false;
        bool      m_portalSeedActive = false;
        glm::vec3 m_portalSeed{0.0f};

        // Debug cull override state — see SetCullOverride above. Copies, not
        // pointers: the caller's camera is a per-frame temporary and the
        // override outlives the call that set it.
        // Same-level peers, set by Initialize.
        Client::ClientChunkManager* m_chunks = nullptr;
        ClientMeshManager*          m_meshes = nullptr;

        bool    m_cullOverrideActive = false;
        Camera  m_cullCamera;
        // Eye position the face-direction groups are tested against
        // (SectionMesh.hpp): the view PrepareVisibleSections last ran for —
        // the cull camera, so the F+C detached view shows the skipped faces
        // like every other culling decision.
        glm::vec3 m_facingCameraPos{0.0f};
        Frustum m_cullFrustum{};

        // World-space portal clip plane — set by PortalRenderer before the
        // see-through scene render. Mirrors Portal's PushCustomClipPlane
        // (portalrenderable_flatbasic.cpp:454). xyz = plane normal,
        // w = -dot(normal, point on plane). Fragments with positive
        // distance are kept; negative are clipped via gl_ClipDistance[0].
        // vec4(0) = no clipping.
    public:
        // DOUBLE: w = −n·point is world-sized; rounded to float at
        // x = 300,000 the plane sits 3 cm off its surface.
        static void SetPortalClipPlane(const glm::dvec4& plane) {
            s_portalClipPlane = plane;
        }
        // The plane as a shader wants it: in the CURRENT view's render space
        // (RenderOrigin.hpp), so every uniform site hands this straight to
        // the backend. Composing with a portal transform or saving it to
        // restore later takes the world-space one below.
        static glm::vec4  PortalClipPlane() { return Render::PlaneToRender(s_portalClipPlane); }
        static glm::dvec4 PortalClipPlaneWorld() { return s_portalClipPlane; }
        // Entities clip against the same plane with its kept side widened
        // by this margin (world units). The immersive portal renderer's
        // mark pass sits its stencil a few centimetres in front of the
        // surface, which wipes anything of the near world in that band;
        // the far view's entity passes draw the band back, and this is
        // what lets them. Terrain keeps the exact plane — a wall flush
        // with a gun portal's surface must stay out of the view.
        static void  SetPortalEntityClipMargin(float margin) { s_portalEntityClipMargin = margin; }
        static float PortalEntityClipMargin() { return s_portalEntityClipMargin; }
        // The camera's near plane, ONE number for the whole frame. The chunk
        // pass builds its own projection (on Vulkan always — the override
        // is a GL-only oblique trick), and every other pass must build the
        // same one or depth stops agreeing between terrain and everything
        // else: mobs drawn through cave walls, the portal mark failing its
        // depth test, the player's own body sinking into the ground. The
        // number scales with the body (see PlayerPhysics::scale).
        // The render distance (chunks) the NEXT chunk pass discovers and
        // draws with, in place of the effective setting; 0 = none. Set by
        // the immersive portal renderer around a view through a portal —
        // the mod draws a far side with its own per-portal distance
        // (PortalRenderer.getPortalRenderDistance, scaled by the client's
        // performance level). Portal-view results are cached per distance,
        // so alternating distances between views do not thrash.
        static void  SetRenderDistanceOverride(int chunks) { s_renderDistanceOverride = chunks; }
        static int   RenderDistanceOverride() { return s_renderDistanceOverride; }
        static void  SetNearPlane(float nearPlane) { s_nearPlane = nearPlane; }
        // How far, in blocks along the view ray, a surface that lies ON a
        // block face (a portal's oval, its rim) must be pushed toward the
        // eye to beat that face's depth reliably at distance `dist`. Two
        // terms: the depth buffer's own resolution there (a float depth's
        // 2^-24 ulp, taken with eight times the headroom), and — the one
        // that actually bites — the float32 error of the vertex transform.
        // Rendering is camera-relative (RenderOrigin.hpp), so the
        // coordinates the GPU multiplies are the distance from the eye, not
        // the world position: the MVP products for a vertex are that size
        // and round to about dist·2^-24 in clip z, and each surface's plane
        // lands with its own rounding, so two coplanar surfaces sit
        // dist·2^-24·dist/near apart in world terms before either is
        // pushed. (Before the render origin existed the world coordinate
        // stood in for `dist` here, ~0.7 mm per block of distance at
        // coordinates of 300 — why a portal's view fought its wall from
        // ten blocks out, worse the farther away.) Four times that error
        // for headroom, a 256-block floor for the matrix entries' own
        // rounding, capped at a quarter block (nothing stands that close
        // to a distant portal). `eye` is the world eye, kept for the
        // callers' sake; only its distance matters now.
        static float SurfaceDepthMargin(float dist, const glm::vec3& eye) {
            (void)eye;
            const float nearPlane = s_nearPlane;
            const float coordMag  = std::max(256.0f, dist);
            const float buffer    = dist * dist / (nearPlane * 2097152.0f);
            const float transform = coordMag * 5.96e-8f * dist / nearPlane * 4.0f;
            return std::clamp(std::max(buffer, transform), 0.005f, 0.25f);
        }
        static float NearPlane() { return s_nearPlane; }
        // Render space, like PortalClipPlane().
        static glm::vec4 PortalEntityClipPlane() {
            glm::dvec4 plane = s_portalClipPlane;
            const double len = glm::length(glm::dvec3(plane));
            if (len > 0.0) plane.w += s_portalEntityClipMargin * len;
            return Render::PlaneToRender(plane);
        }
    private:
        static glm::dvec4 s_portalClipPlane;
        static float     s_portalEntityClipMargin;
        static float     s_nearPlane;
        static int       s_renderDistanceOverride;

        // Render configuration
        bool m_enableFrustumCulling = true;
        bool m_greedyMeshDebug = false;
        // Debug rendering phase: greedy-debug mode draws each pass twice —
        // a depth-biased FILL phase in flat dark grey, then the colored LINE
        // phase on top — so the world reads solid instead of see-through.
        bool m_debugFillPhase = false;
        bool m_drawMergeEnabled = true;   // seeded from OBEY_NO_DRAW_MERGE at init
        // OFF by default (2026-08-31): bridged gaps draw CULLED sections, and
        // the bridged set changes with the camera angle as run layout shifts —
        // at silhouettes and the RD edge that is phantom terrain flickering in
        // and out per angle, which players read as "random holes". Confirmed
        // by pixel-diffing identical camera paths: merge-on frames contained
        // EXTRA unstable geometry, never missing geometry. Exact-adjacent
        // fusion (gap 0) is pixel-identical to unmerged and keeps most of the
        // draw-count win; the checkbox re-enables bridging for benchmarks.
        bool m_gapBridgingEnabled = false;
        // Last frame's OPAQUE submitted runs, for the F8 dump's coverage check:
        // (slab, beginIndex, endIndex) of every run actually handed to the
        // backend. Proves whether an aimed section's range left the CPU.
        std::vector<std::array<size_t, 3>> m_lastOpaqueRuns;
        std::vector<std::array<size_t, 3>> m_callRuns;
        uint32_t m_lastReachableCount = 0;
        uint32_t m_lastVisibleCount = 0;
        PrepareSource m_lastPrepareSource = PrepareSource::Exact;
        void ApplyDebugOverlayUniform(ShaderHandle shader);
        TextureHandle m_whiteDebugTexture = INVALID_TEXTURE;  // lazy, greedy debug view
        // Atlas normally; the lazy 1x1 white texture in greedy-debug view.
        TextureHandle ActiveTerrainTexture();
        void BindSpriteTable(ShaderHandle shader);
        // OBEY_DUMP_VISIBLE=1 diagnostics: see PrepareVisibleSections.
        void DumpVisibleSections(const Camera& camera);
        bool m_enableSmartCull = true;  // Occlusion culling via VisibilitySet BFS
        bool m_wireframeMode = false;
        bool m_showSectionBounds = false;
        int m_debugLayer = -1; // -1 = all layers

        // Render state
        RenderStats m_stats;

        // Occlusion culling graph (BFS from player, skips sections behind solid terrain)
        SectionOcclusionGraph m_occlusionGraph;

        // BFS occlusion-graph result: every reachable section with geometry,
        // sorted front-to-back. Cached across frames — rebuilt only when the
        // camera enters a new section or the world changes (mesh upload/unload).
        // Matches Minecraft: the graph update is throttled, the frustum is not.
        //
        // Multiple slots keyed by camera section: the portal see-through pass
        // renders the scene again from a virtual camera in a DIFFERENT section,
        // which with a single cache would force two full BFS rebuilds per frame
        // (main camera evicts portal camera and vice versa). With slots, each
        // camera keeps its own cached result. LRU eviction by last-used counter.
        struct ReachableCacheSlot {
            bool valid = false;
            int cx = INT_MAX, cz = INT_MAX, sy = INT_MAX;  // Camera section key
            uint32_t lastUsed = 0;
            uint64_t worldVersion = 0;  // World state the BFS saw (staleness check)
            // Prepare-counter at which this slot's list was SNAPSHOTTED
            // (BuildInput time, not adoption time) — drives tombstone
            // reclamation in ClientMeshManager: a GPUSectionData that died
            // at counter D can only be referenced by lists built at <= D.
            std::vector<SectionRenderData> sections;
            // Built for a portal view (seeded at a far surface). The main
            // view never falls back to one of these — drawn from a portal's
            // far-side reachable set for a frame, the world around the
            // player vanished; that was the flicker on every section
            // crossing while flying in a world with many portals — and a
            // portal result never evicts a main-view slot while a portal or
            // free slot exists.
            bool portalView = false;
            // The render distance the list was built at. Part of the key:
            // a portal view drawn at one distance must not serve a view at
            // another (see SetRenderDistanceOverride).
            int renderDistance = 0;
            // m_propagationEpoch when the list's snapshot was taken.
            uint64_t propagationEpoch = 0;
        };
        // A slot describes the world as it is now: no world-version bump
        // since its snapshot, and either the live graph is anchored to it
        // (the per-frame partial updates keep it current) or no propagation
        // event has happened since. MC keeps one graph and invalidates it
        // only on camera movement; a cached slot for another camera section
        // is the thing MC does not have, and it goes out of date by missing
        // exactly those partial updates.
        bool IsSlotFresh(const ReachableCacheSlot& slot, int renderDistanceChunks) const {
            if (!slot.valid || slot.worldVersion != m_worldVersion) return false;
            if (slot.propagationEpoch == m_propagationEpoch) return true;
            return !slot.portalView &&
                   m_occlusionGraph.HasGraphFor(slot.cx, slot.cz, slot.sy, renderDistanceChunks, m_eraseToken);
        }
        uint64_t m_propagationEpoch = 0;
        // Main camera + portal views: every distinct far surface in view
        // keys its own slot (a chain of views through one portal shares one).
        // Enough for the main view and a screenful of portals at once; at 8,
        // seventeen drawn portals cycled every slot every frame.
        static constexpr int kReachableSlots = 32;
        std::array<ReachableCacheSlot, kReachableSlots> m_reachableSlots;
        ReachableCacheSlot* PickEvictionSlot(bool forPortalView);
        // The main view has no slot for its camera section and its BFS has
        // not landed: portal views hold their own submissions until it has
        // (one BFS is in flight at a time, and the player's own world comes
        // first).
        bool m_mainViewAwaitingBfs = false;
        uint32_t m_prepareCounter = 0;  // Monotonic, for LRU slot eviction

        // Async BFS bookkeeping. m_worldVersion advances only on the events
        // MC would invalidate its graph for (render distance, smart-cull
        // toggle, explicit reload, mass block edits) — chunk streaming and
        // mesh uploads are propagation events instead (see IsSlotFresh).
        // Slots built against an older version are usable but trigger an
        // async refresh. m_eraseToken advances only on
        // GPUSectionData ERASURE — results/slots from an older token hold
        // dangling pointers and are discarded outright (see
        // MarkSectionDataErased). BFS stats are copied out of completed jobs.
        bool m_sectionDataErased = false;
        uint64_t m_worldVersion = 1;
        // Last effective render distance PrepareVisibleSections ran with —
        // a change invalidates every reachable-slot (see the worldVersion
        // bump where this is compared).
        int m_lastRenderDistanceChunks = -1;
        uint64_t m_eraseToken = 1;
        int m_bfsVisitedCount = 0;
        int m_bfsOccludedCount = 0;

        // ── Culling diagnostics ────────────────────────────────────────
        // Per-second [CullDiag] log line: min/max visible-section count
        // within the second (a large swing IS the "flashing" symptom, made
        // measurable), full-rebuild and partial-add rates, source backlog.
        // F8 → DumpViewRay: walks the camera ray and logs every section it
        // passes through with its complete culling state, so "look at the
        // hole and press F8" pins which stage dropped it.
        size_t m_diagVisMin = static_cast<size_t>(-1);
        size_t m_diagVisMax = 0;
        int m_diagRebuilds = 0;
        int m_diagPartialAdds = 0;
        std::chrono::steady_clock::time_point m_diagLastLog{};
        // Steady-camera full-rebuild rate limit (see PrepareVisibleSections).
        std::chrono::steady_clock::time_point m_lastRebuildSubmit{};
        // The main view's BFS section last frame. Arriving in a different one
        // whose slot is stale is not a "steady camera": its rebuild skips the
        // rate limit above.
        glm::ivec3 m_lastMainBfsSection{INT_MIN, INT_MIN, INT_MIN};
        bool       m_arrivalRebuildPending = false;
        void DumpViewRay(const Camera& camera, const Frustum& frustum,
                         const ReachableCacheSlot& slot, int renderDistanceChunks);


        // Per-frame draw list: the active slot's sections filtered through the
        // current frustum. Rebuilt every frame (cheap — a few thousand AABB
        // tests), so sections entering the view during rotations appear
        // immediately.
        std::vector<SectionRenderData> m_visibleSections;

        // Copy of m_visibleSections taken only for the MAIN camera, so the mesh
        // scheduler is never handed a portal recursion's view. See the frustum
        // filter in PrepareVisibleSections and GetMainViewSections().
        std::vector<SectionRenderData> m_mainViewSections;
        // See GetPortalViewSections.
        std::vector<SectionRenderData> m_portalViewSections;
        // Membership of a section list, answerable in one array read.
        //
        // A grid of chunk columns around an origin chunk, one 24-bit mask
        // per column (bit = section index); a column is "in" when its mask
        // is non-zero. Anything outside the grid (a portal view into a
        // level a world away) goes to the overflow set, so the answer is
        // exact everywhere and only the common case is fast. This replaced
        // hash sets of packed keys: rebuilding those cost one node
        // allocation per visible section per view (0.12 ms of a 3 ms frame
        // and a quarter of the frustum filter, tour1 2026-09-04), and every
        // scheduler and entity-gating query was a hash probe.
        struct SectionGrid {
            int originX = 0, originZ = 0, radius = -1, width = 0;
            std::vector<uint32_t> masks;
            std::unordered_set<uint64_t> overflow;   // section keys AND column keys

            void Reset(int cx, int cz, int r) {
                if (r != radius) {
                    radius = r; width = 2 * r + 1;
                    masks.assign(static_cast<size_t>(width) * width, 0u);
                } else {
                    std::fill(masks.begin(), masks.end(), 0u);
                }
                originX = cx; originZ = cz;
                overflow.clear();
            }
            void Clear() {
                std::fill(masks.begin(), masks.end(), 0u);
                overflow.clear();
            }
            bool Cell(::Game::Math::ChunkPos p, size_t& idx) const {
                const int gx = p.x - originX + radius;
                const int gz = p.z - originZ + radius;
                if (gx < 0 || gx >= width || gz < 0 || gz >= width) return false;
                idx = static_cast<size_t>(gz) * width + gx;
                return true;
            }
            void Insert(::Game::Math::ChunkPos p, int sectionY) {
                size_t idx;
                if (Cell(p, idx)) {
                    masks[idx] |= 1u << (sectionY & 31);
                } else {
                    overflow.insert(VisibleSectionKey(p, sectionY));
                    overflow.insert(ColumnKey(p));
                }
            }
            bool HasSection(::Game::Math::ChunkPos p, int sectionY) const {
                size_t idx;
                if (Cell(p, idx)) return (masks[idx] >> (sectionY & 31)) & 1u;
                return !overflow.empty() && overflow.count(VisibleSectionKey(p, sectionY)) != 0;
            }
            bool HasColumn(::Game::Math::ChunkPos p) const {
                size_t idx;
                if (Cell(p, idx)) return masks[idx] != 0;
                return !overflow.empty() && overflow.count(ColumnKey(p)) != 0;
            }
            // Union with another grid, whatever its origin.
            void MergeFrom(const SectionGrid& o) {
                if (o.radius == radius && o.originX == originX && o.originZ == originZ) {
                    for (size_t i = 0; i < masks.size(); ++i) masks[i] |= o.masks[i];
                } else {
                    for (int gz = 0; gz < o.width; ++gz) {
                        for (int gx = 0; gx < o.width; ++gx) {
                            uint32_t m = o.masks[static_cast<size_t>(gz) * o.width + gx];
                            if (!m) continue;
                            const ::Game::Math::ChunkPos p{gx - o.radius + o.originX, gz - o.radius + o.originZ};
                            for (int y = 0; m; ++y, m >>= 1) if (m & 1u) Insert(p, y);
                        }
                    }
                }
                overflow.insert(o.overflow.begin(), o.overflow.end());
            }
        };
        // The main view's list and the same-level portal views' lists (see
        // IsMainViewSection / IsMainViewColumn).
        SectionGrid m_mainViewGrid;
        SectionGrid m_portalViewGrid;
        // The directional (shadow) view's sections, refreshed by each shadow
        // pass and kept until ClearDirectionalView: the main pass clears the
        // portal-view list at frame start, after the shadow pass has run.
        SectionGrid m_shadowViewGrid;
        bool       m_directionalActive = false;
        glm::vec3  m_directionalToLight{0.0f, 1.0f, 0.0f};
        glm::dvec3 m_directionalCentre{0.0};
        // Per-frame column-visibility memo for the frustum filter: one byte
        // per chunk column of the render-distance grid (0 untested, 1 out,
        // 2 crosses the frustum, 3 fully inside), so a column's 24 sections
        // cost one box test instead of 24.
        std::vector<uint8_t> m_columnCull;
        // For a column the frustum's edge passes through: the rows that
        // pass, computed once per column per pass (Frustum::SectionRowRange).
        // Valid where m_columnCull is 2.
        std::vector<int8_t>  m_columnRowLo;
        std::vector<int8_t>  m_columnRowHi;

        bool m_visibleSectionsDirty = true;

        // Grid mirror of m_visibleSections' identities, rebuilt alongside
        // it in the per-frame frustum filter, so IsSectionVisible is one
        // array read instead of a scan of a few thousand entries per entity.
        SectionGrid m_visibleGrid;
        // Overflow keys pack chunk x/z (27 bits each — ±67M chunks, far past
        // any world border) and the section index; a chunk beyond that range
        // can only alias onto a false "visible", never a false cull.
        static uint64_t VisibleSectionKey(::Game::Math::ChunkPos pos, int sectionY) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(pos.x) & 0x7FFFFFFu) << 37)
                 | (static_cast<uint64_t>(static_cast<uint32_t>(pos.z) & 0x7FFFFFFu) << 10)
                 | (static_cast<uint64_t>(static_cast<uint32_t>(sectionY) & 0x3FFu));
        }
        // sectionY 0x3FF is outside SECTIONS_PER_CHUNK, so a column key never
        // collides with a section key.
        static uint64_t ColumnKey(::Game::Math::ChunkPos pos) { return VisibleSectionKey(pos, 0x3FF); }

        // --- Translucency re-sort scheduling (MC LevelRenderer:165, 953) ---
        // The cursor persists across frames and wraps modulo the visible count,
        // so every visible section eventually takes its turn in the sweep. A
        // budget that restarts from the front each frame would re-sort the same
        // head sections forever and never reach the tail.
        size_t m_translucencyResortIndex = 0;
        glm::ivec3 m_lastTranslucentSortBlockPos{INT32_MIN, INT32_MIN, INT32_MIN};

        // ── Draw-list scratch (members so the per-frame build allocates nothing)
        // One entry per visible section that has geometry in the layer being
        // drawn: where its indices live. Offsets/counts are in INDICES.
        struct DrawEntry {
            uint32_t slab;
            uint32_t offset;
            uint32_t count;
            BufferHandle ibo;   // per-section-IBO layers only; INVALID_BUFFER otherwise
        };
        std::vector<DrawEntry> m_drawEntries;
        // Radix-sort scratch for SubmitMergedRuns (see RadixSortDrawEntries).
        std::vector<DrawEntry> m_sortScratch;
        std::vector<uint32_t>  m_sortKeys;
        std::vector<uint32_t>  m_sortKeysScratch;
        void RadixSortDrawEntries();
#ifdef TRACY_ENABLE
        std::vector<DrawEntry> m_diagFullEntries;   // sub-draw attribution, RenderLayerPass
#endif
        // The runs (merged sub-draws) for the slab currently being flushed —
        // exactly the arrays MultiDrawIndexedBaseVertex takes.
        std::vector<int32_t> m_runCounts;
        std::vector<size_t>  m_runByteOffsets;
        // Translucent per-slab run buckets (SubmitOrderedRuns) — reused
        // allocations, one bucket per slab.
        std::vector<std::vector<int32_t>> m_slabRunCounts;
        std::vector<uint32_t> m_slabOrder;   // SubmitOrderedRuns: slabs in first-seen order
        std::vector<std::vector<size_t>>  m_slabRunOffsets;
        // Slab indices are absolute, so every sub-draw has baseVertex 0; the
        // backend API still wants an array of them. Grown, never shrunk.
        std::vector<int32_t> m_zeroBaseVertices;

        // Opaque/cutout run merging: two sections in the same slab become one
        // sub-draw when the index gap between them is at most this many
        // indices. The gap is drawn too — it is either sections the frustum
        // culled (drawn for nothing, a few thousand extra indices on a tile
        // GPU is nothing next to a driver-side command) or space zeroed on
        // free (degenerate triangles). 8192 indices ≈ 1365 quads, the size of
        // a few small sections. Tune against Draws/Merged in Tracy.
        // Gap tolerance in indices. OBEY_GAP_BRIDGE=<indices> turns bridging
        // on with this tolerance from the command line (0 = off).
        uint32_t m_gapBridgeIndices = 8192;

        // How many of this frame's visible sections carry translucent
        // geometry. Set by PrepareVisibleSections so RenderAll can skip the
        // translucent pass — shader bind, six uniform uploads, the resort
        // sweep — outright when it would draw nothing, which is most frames
        // away from water.
        int m_visibleTranslucentSections = 0;

        // Issue m_drawEntries for one layer. Merged: sorted by (slab, offset)
        // and fused across small gaps — opaque/cutout, order irrelevant.
        // Ordered: list order preserved exactly (back-to-front), fusing only
        // zero-gap ascending neighbours — translucent. Each returns the number
        // of sub-draws issued.
        int SubmitMergedRuns(ChunkMegaBuffer& megaBuffer);

        // ── Stray-section audit (OBEY_STRAY_AUDIT=1) ─────────────────────────
        // Gap bridging draws whatever lives between two visible runs of a
        // slab. Once a second (main view only) every bridged gap is resolved
        // to the sections it holds, and each is classified against the
        // client's chunk state and the render distance; one [StrayAudit] log
        // line reports what bridging drew that the frame did not ask for.
        struct StrayAudit {
            size_t gaps = 0, gapIndices = 0, sections = 0;
            size_t parked = 0, notLoaded = 0, outOfRange = 0, hiddenLoaded = 0, visible = 0;
            struct Example { ::Game::Math::ChunkPos pos; int sectionY; int dist; const char* what; };
            std::vector<Example> examples;
        };
        void AuditBridgedGap(const std::vector<std::vector<ChunkMegaBuffer::DebugRegionRef>>& regions,
                             uint32_t slab, size_t gapBegin, size_t gapEnd);
        void FlushStrayAudit();
        // Fences the loaded chunks outside the rendered view from bridged
        // gaps (ClientMeshManager::SetOutsideViewChunks) whenever the camera
        // chunk, the render distance or the loaded set changed. Main view.
        void UpdateOutsideViewFence(int cameraChunkX, int cameraChunkZ, int renderDistanceChunks);
        int m_fenceCamX = INT_MIN, m_fenceCamZ = INT_MIN, m_fenceRenderDistance = -1;
        uint64_t m_fenceLoadedVersion = ~uint64_t{0};
        StrayAudit m_strayAudit;
        bool m_strayAuditFrame = false;          // this frame's main view is sampled
        int  m_strayAuditRenderDistance = 0;
        std::chrono::steady_clock::time_point m_strayAuditLast{};
        int SubmitOrderedRuns(ChunkMegaBuffer& megaBuffer);
        int SubmitPerSectionIbos(ChunkMegaBuffer& megaBuffer);

        // Pass configurations
        RenderPassConfig m_opaqueConfig;
        RenderPassConfig m_cutoutConfig;
        RenderPassConfig m_translucentConfig;

        // GPU pass timers (GL_TIME_ELAPSED). One in-flight query per pass:
        // results arrive 1-2 frames later and are polled non-blocking at the
        // start of the next RenderAll; while a pass's query is still pending,
        // no new one is started for it. Only the MAIN scene render is timed
        // (portal see-through re-entries are skipped) so the numbers cleanly
        // mean "GPU cost of this pass for the main view". m_gpuPassResultMs
        // holds the last completed reading and feeds m_stats.gpu*TimeMs.
        static constexpr int kGpuPassCount = 3;  // opaque, cutout, translucent
        GPUTimerHandle m_gpuTimerPending[kGpuPassCount] = {
            INVALID_GPU_TIMER, INVALID_GPU_TIMER, INVALID_GPU_TIMER};
        float m_gpuPassResultMs[kGpuPassCount] = {0.0f, 0.0f, 0.0f};

        // OpenGL state management
        void SetupRenderPass(const RenderPassConfig& config);
        void RestoreRenderState();

        // Shader uniform setup
        void SetupShaderUniforms(const Camera& camera);
        void BindTextureAtlas();

        // Error checking (no-op in Vulkan mode, checks glGetError in GL mode via backend)
        bool CheckShaderErrors(const std::string& pass);

        // Initialize render pass configurations
        void SetupRenderConfigs();
        
        // Render methods for each pass
        void RenderOpaque(const Camera& camera, const Frustum& frustum);
        void RenderCutout(const Camera& camera, const Frustum& frustum);
        void RenderTranslucent(const Camera& camera, const Frustum& frustum);
        
        // Section preparation and culling
        void PrepareVisibleSections(const Camera& camera, const Frustum& frustum);
        // The portal-view fallback (m_useProjectionOverride, no seed or no
        // reachable slot yet): no occlusion BFS, no cache — every loaded
        // section within render distance of the far camera that passes the
        // portal-bounded frustum. See SetPortalViewSeed and the call site.
        void PrepareVisibleSectionsThroughPortal(const Camera& camera, const Frustum& frustum,
                                                 int renderDistanceChunks);

        // Port of LevelRenderer.scheduleTranslucentSectionResort (:953). Owns
        // the POLICY — which sections get re-sorted and how often; the per-
        // section work lives in ClientMeshManager::ResortTranslucentSection,
        // mirroring MC's split between LevelRenderer and RenderSection.
        // Runs once a frame, immediately before the translucent pass.
        void ScheduleTranslucentSectionResort(const glm::vec3& cameraPos);
        
        // Bind shader, MVP, and atlas texture once per frame (shared across all 3 passes)
        void BindSharedRenderState(const Camera& camera);
        void SetEnvironmentUniforms(ShaderHandle shader, const Camera& camera);

        // Render helpers. backToFront = walk the visible list in reverse and
        // keep that order on the GPU (translucent); otherwise order is free
        // and runs are merged.
        void RenderLayerPass(RenderLayer layer, bool backToFront = false);
        void RenderSectionBounds(const Camera& camera, const std::vector<SectionRenderData>& sections);
        float CalculateSectionDistance(const Camera& camera, ::Game::Math::ChunkPos chunkPos, int sectionY);
        bool IsSectionInFrustum(const Frustum& frustum, ::Game::Math::ChunkPos chunkPos, int sectionY);
        AABB GetSectionAABB(::Game::Math::ChunkPos chunkPos, int sectionY);
    };

    // The chunk renderer of the level the globals are BOUND to (see
    // ClientLevel.hpp). Owned by ClientLevel; rebound by ClientLevels.
    extern ChunkRenderer* g_chunkRenderer;

    // Runtime toggle for the per-pass GL_TIME_ELAPSED GPU timers (defined in
    // ChunkRenderer.cpp, exposed in the Debug UI's Render Controls panel).
    // Exists to A/B whether query begin/end/poll act as driver flush points
    // on Apple's GL — disable during Tracy captures for clean numbers.
    extern bool g_enableGpuPassTimers;

    // Utility functions for integration (all act on the bound renderer)

    // Main rendering entry points
    void RenderChunksOpaque(const Camera& camera, const Frustum& frustum);
    void RenderChunksCutout(const Camera& camera, const Frustum& frustum);
    void RenderChunksTranslucent(const Camera& camera, const Frustum& frustum);
    void RenderChunksAll(const Camera& camera, const Frustum& frustum);
    // Variant that lets the caller (portal see-through pass) inject a
    // non-standard projection matrix. See ChunkRenderer::RenderAll above.
    // ChunkRenderer::RenderDeferredTranslucent on the global renderer.
    void RenderChunksDeferredTranslucent();
    void RenderChunksAll(const Camera& camera, const Frustum& frustum,
                         const glm::mat4& projectionOverride, bool exactProjection = false);
    
    // Get current frame's rendering statistics
    const RenderStats* GetChunkRendererStats();

} // namespace Render