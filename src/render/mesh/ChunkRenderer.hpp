// File: src/render/mesh/ChunkRenderer.hpp
#pragma once

#include "MeshManager.hpp"
#include "Mesher.hpp"
#include "../gfx/Camera.hpp"
#include "../gfx/Frustum.hpp"
#include "../gfx/Shader.hpp"
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
        GLenum blendSrc = GL_SRC_ALPHA;
        GLenum blendDst = GL_ONE_MINUS_SRC_ALPHA;

        // Alpha test threshold (for cutout pass)
        float alphaThreshold = 0.5f;

        // Render order
        bool frontToBack = true;  // For opaque/cutout (depth buffer optimization)
                                  // Will be set to false for translucent (back to front)
    };

    // Section render data for sorting and culling
    struct SectionRenderData {
        Game::Math::ChunkPos chunkPos;
        int sectionY;
        const GPUSectionData* gpuData;
        float distanceToCamera;
        bool inFrustum;

        SectionRenderData(Game::Math::ChunkPos pos, int secY, const GPUSectionData* gpu, float dist)
            : chunkPos(pos), sectionY(secY), gpuData(gpu), distanceToCamera(dist), inFrustum(true) {}
    };

    // Render statistics for debugging
    struct RenderStats {
        // Per-frame counters
        int sectionsRendered = 0;
        int sectionsSkipped = 0;
        int totalDrawCalls = 0;

        // Per-layer counters
        int opaqueSections = 0;
        int cutoutSections = 0;
        int translucentSections = 0;

        // Geometry statistics
        size_t totalVerticesRendered = 0;
        size_t totalIndicesRendered = 0;

        // Performance timing
        float frustumCullTimeMs = 0.0f;
        float sortTimeMs = 0.0f;
        float renderTimeMs = 0.0f;

        void Reset() {
            sectionsRendered = sectionsSkipped = totalDrawCalls = 0;
            opaqueSections = cutoutSections = translucentSections = 0;
            totalVerticesRendered = totalIndicesRendered = 0;
            frustumCullTimeMs = sortTimeMs = renderTimeMs = 0.0f;
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

        // Three-layer rendering pipeline
        void RenderOpaque(const Camera& camera, const Frustum& frustum);
        void RenderCutout(const Camera& camera, const Frustum& frustum);
        void RenderTranslucent(const Camera& camera, const Frustum& frustum);

        // Convenience method to render all layers
        void RenderAll(const Camera& camera, const Frustum& frustum);

        // Configuration
        void SetRenderDistance(float distance) { m_renderDistance = distance; }
        float GetRenderDistance() const { return m_renderDistance; }

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

        // Render configuration
        float m_renderDistance = 128.0f;
        bool m_enableFrustumCulling = true;
        bool m_wireframeMode = false;
        bool m_showSectionBounds = false;
        int m_debugLayer = -1; // -1 = all layers

        // Render state
        RenderStats m_stats;
        std::vector<SectionRenderData> m_visibleSections;

        // Pass configurations
        RenderPassConfig m_opaqueConfig;
        RenderPassConfig m_cutoutConfig;
        RenderPassConfig m_translucentConfig;

        // Core rendering methods
        void PrepareVisibleSections(const Camera& camera, const Frustum& frustum);
        void RenderLayerPass(RenderLayer layer, const Camera& camera, const std::vector<SectionRenderData>& sections);
        void RenderSectionLayer(const SectionRenderData& section, RenderLayer layer);

        // Culling and sorting
        void PerformFrustumCulling(const Frustum& frustum, std::vector<SectionRenderData>& sections);
        void SortSections(const Camera& camera, std::vector<SectionRenderData>& sections, bool frontToBack);

        // OpenGL state management
        void SetupRenderPass(const RenderPassConfig& config);
        void RestoreRenderState();

        // Utility methods
        float CalculateSectionDistance(const Camera& camera, Game::Math::ChunkPos chunkPos, int sectionY);
        bool IsSectionInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos, int sectionY);
        AABB GetSectionAABB(Game::Math::ChunkPos chunkPos, int sectionY);

        // Shader uniform setup
        void SetupShaderUniforms(const Camera& camera);
        void BindTextureAtlas();

        // Debug rendering
        void RenderSectionBounds(const Camera& camera, const std::vector<SectionRenderData>& sections);
        void RenderDebugInfo(const Camera& camera);

        // Error checking
        bool CheckShaderErrors(const std::string& pass);
        void LogRenderError(const std::string& operation, GLenum error);

        // Initialize render pass configurations
        void SetupRenderConfigs();
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

} // namespace Render