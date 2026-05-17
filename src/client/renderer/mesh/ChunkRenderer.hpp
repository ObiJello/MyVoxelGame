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
#include <vector>
#include <memory>

namespace Render {

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

    // Section render data for sorting and culling
    struct SectionRenderData {
        ::Game::Math::ChunkPos chunkPos;
        int sectionY;
        const GPUSectionData* gpuData;
        float distanceToCamera;
        bool inFrustum;
        uint8_t layerMask;  // Bitmask of LayerFlag values

        SectionRenderData(::Game::Math::ChunkPos pos, int secY, const GPUSectionData* gpu, float dist)
            : chunkPos(pos), sectionY(secY), gpuData(gpu), distanceToCamera(dist), inFrustum(true), layerMask(0) {}
    };

    // Render statistics for debugging
    struct RenderStats {
        // Per-frame counters
        int sectionsRendered = 0;
        int sectionsSkipped = 0;  // Culled by frustum
        int sectionsAvailable = 0;  // Total sections checked before culling
        int totalDrawCalls = 0;

        // Per-layer counters
        int opaqueSections = 0;
        int cutoutSections = 0;
        int translucentSections = 0;

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

        // Initialize renderer with shaders
        bool Initialize();
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


        // Debug rendering options
        void SetWireframeMode(bool enable);
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
    private:
        static glm::vec4 s_portalClipPlane;

        // Render configuration
        bool m_enableFrustumCulling = true;
        bool m_enableSmartCull = true;  // Occlusion culling via VisibilitySet BFS
        bool m_wireframeMode = false;
        bool m_showSectionBounds = false;
        int m_debugLayer = -1; // -1 = all layers

        // Render state
        RenderStats m_stats;

        // Occlusion culling graph (BFS from player, skips sections behind solid terrain)
        SectionOcclusionGraph m_occlusionGraph;

        // Visible sections cache
        std::vector<SectionRenderData> m_visibleSections;

        // Visible section cache — avoids rebuilding the visible list every frame
        // when the camera hasn't moved significantly. Matches Minecraft's approach
        // of only recalculating visibility on chunk transitions and rotation changes.
        bool m_visibleSectionsDirty = true;
        int m_lastCameraChunkX = INT_MAX;
        int m_lastCameraChunkZ = INT_MAX;
        int m_lastCameraSectionY = INT_MAX;
        float m_lastCameraYaw = 0.0f;
        float m_lastCameraPitch = 0.0f;

        // Per-slab multi-draw command arrays (reused each frame to avoid allocation)
        std::vector<std::vector<int32_t>> m_perSlabCounts;
        std::vector<std::vector<size_t>> m_perSlabOffsets;
        std::vector<std::vector<int32_t>> m_perSlabBaseVertices;

        // Distant cutout multi-draw arrays (rendered as solid in opaque pass)
        std::vector<int32_t> m_distantCutoutCounts;
        std::vector<size_t> m_distantCutoutOffsets;
        std::vector<int32_t> m_distantCutoutBaseVertices;

        // Pass configurations
        RenderPassConfig m_opaqueConfig;
        RenderPassConfig m_cutoutConfig;
        RenderPassConfig m_translucentConfig;

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
        void SortSections(const Camera& camera, std::vector<SectionRenderData>& sections, bool frontToBack);
        
        // Bind shader, MVP, and atlas texture once per frame (shared across all 3 passes)
        void BindSharedRenderState(const Camera& camera);

        // Render helpers
        void RenderLayerPass(RenderLayer layer, uint8_t layerBit, bool reverseOrder = false);
        void RenderSectionLayer(const SectionRenderData& section, RenderLayer layer);
        void RenderSectionBounds(const Camera& camera, const std::vector<SectionRenderData>& sections);
        float CalculateSectionDistance(const Camera& camera, ::Game::Math::ChunkPos chunkPos, int sectionY);
        bool IsSectionInFrustum(const Frustum& frustum, ::Game::Math::ChunkPos chunkPos, int sectionY);
        AABB GetSectionAABB(::Game::Math::ChunkPos chunkPos, int sectionY);
    };

    // Global chunk renderer instance
    extern std::unique_ptr<ChunkRenderer> g_chunkRenderer;

    // Utility functions for integration
    bool InitializeChunkRenderer();
    void ShutdownChunkRenderer();

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