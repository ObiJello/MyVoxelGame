// File: src/client/renderer/mesh/ChunkRenderer.hpp
#pragma once

#include "ClientMeshManager.hpp"
#include "Mesher.hpp"
#include "../core/Camera.hpp"
#include "../core/Frustum.hpp"
#include "../shader/Shader.hpp"
#include "../texture/TextureAnimator.hpp"
#include "../backend/RenderTypes.hpp"
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

    // Section render data for sorting and culling
    struct SectionRenderData {
        ::Game::Math::ChunkPos chunkPos;
        int sectionY;
        const GPUSectionData* gpuData;
        float distanceToCamera;
        bool inFrustum;

        SectionRenderData(::Game::Math::ChunkPos pos, int secY, const GPUSectionData* gpu, float dist)
            : chunkPos(pos), sectionY(secY), gpuData(gpu), distanceToCamera(dist), inFrustum(true) {}
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

        void Reset() {
            sectionsRendered = sectionsSkipped = sectionsAvailable = totalDrawCalls = 0;
            opaqueSections = cutoutSections = translucentSections = 0;
            totalVerticesRendered = totalIndicesRendered = 0;
            buildDrawListsTimeMs = chunkIterationTimeMs = gpuDataLoadTimeMs = 0.0f;
            frustumCullingTimeMs = sortingTimeMs = 0.0f;
            opaquePassTimeMs = cutoutPassTimeMs = translucentPassTimeMs = 0.0f;
            renderTimeMs = 0.0f;
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

        // Convenience method to render all layers
        void RenderAll(const Camera& camera, const Frustum& frustum);

        // Configuration - **UPDATED**: Now reads from game settings
        void RefreshSettings(); // Call when settings change

        void SetEnableFrustumCulling(bool enable) { m_enableFrustumCulling = enable; }
        bool IsEnabledFrustumCulling() const { return m_enableFrustumCulling; }

        // Statistics
        const RenderStats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats.Reset(); }
        

        // Debug rendering options
        void SetWireframeMode(bool enable);
        void SetShowSectionBounds(bool enable) { m_showSectionBounds = enable; }
        void SetDebugLayer(int layer) { m_debugLayer = layer; } // -1 = all, 0 = opaque, 1 = cutout, 2 = translucent

    private:
        // Shader management
        std::unique_ptr<Shader> m_blockShader;
        bool m_shadersLoaded = false;

        // Backend handles for Vulkan rendering
        ShaderHandle m_backendShader = INVALID_SHADER;
        TextureHandle m_backendAtlasTexture = INVALID_TEXTURE;

        // Render configuration
        bool m_enableFrustumCulling = true;
        bool m_wireframeMode = false;
        bool m_showSectionBounds = false;
        int m_debugLayer = -1; // -1 = all layers

        // Render state
        RenderStats m_stats;
        
        // Visible sections cache
        std::vector<SectionRenderData> m_visibleSections;

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
        
        // Render helpers
        void RenderLayerPass(RenderLayer layer, const Camera& camera, const std::vector<SectionRenderData>& sections);
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
    
    // Get current frame's rendering statistics
    const RenderStats* GetChunkRendererStats();

} // namespace Render