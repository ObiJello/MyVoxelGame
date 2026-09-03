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
        void RenderAll(const Camera& camera, const Frustum& frustum,
                       const glm::mat4& projectionOverride);

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
        void SchedulePropagationFrom(::Game::Math::ChunkPos chunkPos, int sectionY) {
            m_occlusionGraph.SchedulePropagationFrom(chunkPos, sectionY);
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
        // The sections of the view being drawn RIGHT NOW — the main view's,
        // or a portal view's while one is rendering. Renderers that gather
        // per pass (block entities) read this, so what a portal shows is
        // gathered from the portal's own view rather than the main camera's.
        const std::vector<SectionRenderData>& GetVisibleSections() const { return m_visibleSections; }

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
            return m_visibleSectionKeys.find(VisibleSectionKey(chunkPos, sectionY))
                != m_visibleSectionKeys.end();
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

        // Occlusion/frustum readout for the debug panel: sections the BFS
        // reached from the (cull) camera, and how many survived the frustum.
        uint32_t GetLastReachableCount() const { return m_lastReachableCount; }
        uint32_t GetLastVisibleCount() const { return m_lastVisibleCount; }
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
        TextureHandle m_backendAtlasTexture = INVALID_TEXTURE;
        glm::mat4 m_cachedMVP{1.0f};                         // Cached for shader switches

        // Projection override — used by the portal renderer's see-through
        // pass to inject an oblique near-plane projection. When
        // m_useProjectionOverride is true, RenderAll uses m_projectionOverride
        // instead of computing its own glm::perspective; reset to false
        // automatically by the (camera, frustum, projection) overload after
        // the call returns.
        bool      m_useProjectionOverride = false;
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
        Frustum m_cullFrustum{};

        // World-space portal clip plane — set by PortalRenderer before the
        // see-through scene render. Mirrors Portal's PushCustomClipPlane
        // (portalrenderable_flatbasic.cpp:454). xyz = plane normal,
        // w = -dot(normal, point on plane). Fragments with positive
        // distance are kept; negative are clipped via gl_ClipDistance[0].
        // vec4(0) = no clipping.
    public:
        static void SetPortalClipPlane(const glm::vec4& plane) {
            s_portalClipPlane = plane;
        }
        static glm::vec4 PortalClipPlane() { return s_portalClipPlane; }
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
        static void  SetNearPlane(float nearPlane) { s_nearPlane = nearPlane; }
        static float NearPlane() { return s_nearPlane; }
        static glm::vec4 PortalEntityClipPlane() {
            glm::vec4 plane = s_portalClipPlane;
            const float len = glm::length(glm::vec3(plane));
            if (len > 0.0f) plane.w += s_portalEntityClipMargin * len;
            return plane;
        }
    private:
        static glm::vec4 s_portalClipPlane;
        static float     s_portalEntityClipMargin;
        static float     s_nearPlane;

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
        void ApplyDebugOverlayUniform(ShaderHandle shader);
        TextureHandle m_whiteDebugTexture = INVALID_TEXTURE;  // lazy, greedy debug view
        // Atlas normally; the lazy 1x1 white texture in greedy-debug view.
        TextureHandle ActiveTerrainTexture();
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
        };
        // Main camera + portal views: every distinct far surface in view
        // keys its own slot (a chain of views through one portal shares one).
        static constexpr int kReachableSlots = 8;
        std::array<ReachableCacheSlot, kReachableSlots> m_reachableSlots;
        uint32_t m_prepareCounter = 0;  // Monotonic, for LRU slot eviction

        // Async BFS bookkeeping. m_worldVersion advances on every dirty event
        // (mesh upload/unload) — slots built against an older version are
        // usable but trigger an async refresh. m_eraseToken advances only on
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

        bool m_visibleSectionsDirty = true;

        // Hash-set mirror of m_visibleSections' identities, rebuilt alongside
        // it in the per-frame frustum filter, so IsSectionVisible is one
        // lookup instead of a scan of a few thousand entries per entity.
        // Key packs chunk x/z (27 bits each — ±67M chunks, far past any world
        // border) and the section index; a chunk beyond that range can only
        // alias onto a false "visible", never a false cull.
        std::unordered_set<uint64_t> m_visibleSectionKeys;
        static uint64_t VisibleSectionKey(::Game::Math::ChunkPos pos, int sectionY) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(pos.x) & 0x7FFFFFFu) << 37)
                 | (static_cast<uint64_t>(static_cast<uint32_t>(pos.z) & 0x7FFFFFFu) << 10)
                 | (static_cast<uint64_t>(static_cast<uint32_t>(sectionY) & 0x3FFu));
        }

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
        // The runs (merged sub-draws) for the slab currently being flushed —
        // exactly the arrays MultiDrawIndexedBaseVertex takes.
        std::vector<int32_t> m_runCounts;
        std::vector<size_t>  m_runByteOffsets;
        // Translucent per-slab run buckets (SubmitOrderedRuns) — reused
        // allocations, one bucket per slab.
        std::vector<std::vector<int32_t>> m_slabRunCounts;
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
        static constexpr uint32_t kDrawMergeGapIndices = 8192;

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
    void RenderChunksAll(const Camera& camera, const Frustum& frustum,
                         const glm::mat4& projectionOverride);
    
    // Get current frame's rendering statistics
    const RenderStats* GetChunkRendererStats();

} // namespace Render