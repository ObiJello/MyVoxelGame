// File: src/render/mesh/ChunkRenderer.cpp
#include "ChunkRenderer.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include "../../core/Log.hpp"
#include "../../core/Config.hpp"
#include "../../platform/GameDirectory.hpp"  // **NEW**: Include for settings access
#include <algorithm>
#include <chrono>
#include <glm/gtc/matrix_transform.hpp>

namespace Render {

    // Global chunk renderer instance
    std::unique_ptr<ChunkRenderer> g_chunkRenderer = nullptr;

    ChunkRenderer::ChunkRenderer() {
        SetupRenderConfigs();
        LoadRenderSettings();  // **NEW**: Load settings on construction
        m_visibleSections.reserve(768); // Reserve space for performance
    }

    ChunkRenderer::~ChunkRenderer() {
        Shutdown();
    }

    bool ChunkRenderer::Initialize() {
        Log::Info("Initializing ChunkRenderer...");

        // Load render settings from game settings
        LoadRenderSettings();

        // Load shaders
        try {
            m_blockShader = std::make_unique<Shader>("shaders/block.vert", "shaders/block.frag");
            m_shadersLoaded = true;
            Log::Info("✓ Block shaders loaded successfully");
        } catch (const std::exception& e) {
            Log::Error("Failed to load block shaders: %s", e.what());
            return false;
        }

        // Reset statistics
        m_stats.Reset();

        Log::Info("✓ ChunkRenderer initialized successfully (render distance: %.1f blocks)", m_renderDistance);
        return true;
    }

    void ChunkRenderer::Shutdown() {
        if (m_blockShader) {
            m_blockShader.reset();
            m_shadersLoaded = false;
        }
        Log::Info("ChunkRenderer shutdown complete");
    }

    void ChunkRenderer::RefreshSettings() {
        LoadRenderSettings();
        Log::Info("ChunkRenderer settings refreshed (render distance: %.1f blocks)", m_renderDistance);
    }

    void ChunkRenderer::LoadRenderSettings() {
        // **NEW**: Load render distance from game settings
        m_renderDistance = static_cast<float>(Platform::g_gameSettings.GetRenderDistance());

        // Convert from chunks to blocks for internal use
        // Minecraft's render distance is in chunks (16 blocks each)
        m_renderDistance = m_renderDistance * 16.0f;

        // Clamp to reasonable values
        m_renderDistance = std::clamp(m_renderDistance, 32.0f, 1024.0f);

        Log::Debug("Loaded render distance from settings: %.1f blocks (%.1f chunks)",
                  m_renderDistance, m_renderDistance / 16.0f);
    }

    void ChunkRenderer::RenderOpaque(const Camera& camera, const Frustum& frustum) {
        if (!m_shadersLoaded || !g_meshManager || (m_debugLayer >= 0 && m_debugLayer != 0)) {
            Log::Debug("RenderOpaque early exit: shaders=%d, meshManager=%p, debugLayer=%d",
                      m_shadersLoaded, (void*)g_meshManager.get(), m_debugLayer);
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Prepare visible sections
        PrepareVisibleSections(camera, frustum);


        // Filter for sections with opaque geometry
        std::vector<SectionRenderData> opaqueSections;
        for (const auto& section : m_visibleSections) {
            if (section.gpuData && section.gpuData->opaqueIndexCount > 0) {
                opaqueSections.push_back(section);
            }
        }

        if (opaqueSections.empty()) {
            return;
        }

        // Sort front to back for depth buffer optimization
        SortSections(camera, opaqueSections, true);

        // Setup render state for opaque pass
        SetupRenderPass(m_opaqueConfig);

        // **DEBUG**: Log render state
        GLboolean depthTest, depthMask, blendEnabled;
        glGetBooleanv(GL_DEPTH_TEST, &depthTest);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        blendEnabled = glIsEnabled(GL_BLEND);
        //Log::Debug("Render state: depth_test=%d, depth_mask=%d, blend=%d", depthTest, depthMask, blendEnabled);

        // Render the layer
        RenderLayerPass(RenderLayer::Opaque, camera, opaqueSections);

        m_stats.opaqueSections = static_cast<int>(opaqueSections.size());

        auto endTime = std::chrono::high_resolution_clock::now();
        float renderTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        m_stats.renderTimeMs += renderTime;
    }

    void ChunkRenderer::RenderCutout(const Camera& camera, const Frustum& frustum) {
        if (!m_shadersLoaded || !g_meshManager || (m_debugLayer >= 0 && m_debugLayer != 1)) {
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Use cached visible sections from opaque pass if available, otherwise prepare
        if (m_visibleSections.empty()) {
            PrepareVisibleSections(camera, frustum);
        }

        // Filter for sections with cutout geometry
        std::vector<SectionRenderData> cutoutSections;
        for (const auto& section : m_visibleSections) {
            if (section.gpuData && section.gpuData->cutoutIndexCount > 0) {
                cutoutSections.push_back(section);
            }
        }

        // Sort front to back for depth buffer optimization
        SortSections(camera, cutoutSections, true);

        // Setup render state for cutout pass
        SetupRenderPass(m_cutoutConfig);

        // Render the layer
        RenderLayerPass(RenderLayer::Cutout, camera, cutoutSections);

        m_stats.cutoutSections = static_cast<int>(cutoutSections.size());

        auto endTime = std::chrono::high_resolution_clock::now();
        float renderTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        m_stats.renderTimeMs += renderTime;

        //Log::Debug("Rendered %zu cutout sections in %.2f ms", cutoutSections.size(), renderTime);
    }

    void ChunkRenderer::RenderTranslucent(const Camera& camera, const Frustum& frustum) {
        if (!m_shadersLoaded || !g_meshManager || (m_debugLayer >= 0 && m_debugLayer != 2)) {
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Use cached visible sections from previous passes if available
        if (m_visibleSections.empty()) {
            PrepareVisibleSections(camera, frustum);
        }

        // Filter for sections with translucent geometry
        std::vector<SectionRenderData> translucentSections;
        for (const auto& section : m_visibleSections) {
            if (section.gpuData && section.gpuData->translucentIndexCount > 0) {
                translucentSections.push_back(section);
            }
        }

        // Sort back to front for correct alpha blending
        SortSections(camera, translucentSections, false);

        // Setup render state for translucent pass
        SetupRenderPass(m_translucentConfig);

        // Render the layer
        RenderLayerPass(RenderLayer::Translucent, camera, translucentSections);

        m_stats.translucentSections = static_cast<int>(translucentSections.size());

        auto endTime = std::chrono::high_resolution_clock::now();
        float renderTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        m_stats.renderTimeMs += renderTime;

        //Log::Debug("Rendered %zu translucent sections in %.2f ms", translucentSections.size(), renderTime);
    }

    void ChunkRenderer::RenderAll(const Camera& camera, const Frustum& frustum) {
        m_stats.Reset();

        // Clear visible sections cache for fresh frame
        m_visibleSections.clear();

        // Render in correct order for proper blending
        RenderOpaque(camera, frustum);
        RenderCutout(camera, frustum);
        RenderTranslucent(camera, frustum);

        // Render debug info if enabled
        if (m_showSectionBounds) {
            RenderSectionBounds(camera, m_visibleSections);
        }

        RestoreRenderState();
    }

    void ChunkRenderer::SetWireframeMode(bool enable) {
        m_wireframeMode = enable;
        if (enable) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
    }

    void ChunkRenderer::PrepareVisibleSections(const Camera& camera, const Frustum& frustum) {
        auto startTime = std::chrono::high_resolution_clock::now();

        m_visibleSections.clear();

        if (!g_meshManager) {
            Log::Debug("PrepareVisibleSections: No mesh manager available");
            return;
        }

        const auto& gpuManager = g_meshManager->GetGPUDataManager();

        // Calculate player chunk position for square rendering
        glm::vec3 playerPos = camera.position;
        int playerChunkX = static_cast<int>(std::floor(playerPos.x / Game::Math::CHUNK_SIZE_X));
        int playerChunkZ = static_cast<int>(std::floor(playerPos.z / Game::Math::CHUNK_SIZE_Z));

        // **UPDATED**: Calculate square size based on render distance
        // Convert render distance from blocks to chunks and use it as the square half-size
        int renderDistanceChunks = static_cast<int>(std::ceil(m_renderDistance / Game::Math::CHUNK_SIZE_X));

        // Create a square of size (renderDistanceChunks * 2 + 1) x (renderDistanceChunks * 2 + 1)
        int halfSize = renderDistanceChunks;  // This creates the desired square size

        int sectionsFound = 0;
        int sectionsWithGeometry = 0;
        int sectionsInRange = 0;
        int chunksChecked = 0;

        // **UPDATED**: Iterate in a square pattern instead of radius check
        for (int dz = -halfSize; dz <= halfSize; ++dz) {
            for (int dx = -halfSize; dx <= halfSize; ++dx) {
                Game::Math::ChunkPos chunkPos{playerChunkX + dx, playerChunkZ + dz};
                chunksChecked++;
                sectionsInRange++;  // All chunks in the square are "in range"

                // Check all sections in this chunk
                for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                    const GPUSectionData* gpuData = gpuManager.GetSectionData(chunkPos, sectionY);

                    if (gpuData) {
                        sectionsFound++;
                        if (gpuData->HasGeometry()) {
                            sectionsWithGeometry++;
                            float sectionDistance = CalculateSectionDistance(camera, chunkPos, sectionY);

                            SectionRenderData renderData(chunkPos, sectionY, gpuData, sectionDistance);

                            m_visibleSections.push_back(renderData);
                        }
                    }
                }
            }
        }

        // **ALWAYS DISABLE FRUSTUM CULLING FOR NOW** to debug the rendering issue
        if (m_enableFrustumCulling) {
            PerformFrustumCulling(frustum, m_visibleSections);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.frustumCullTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        m_stats.sectionsRendered = static_cast<int>(m_visibleSections.size());
    }

    void ChunkRenderer::RenderLayerPass(RenderLayer layer, const Camera& camera, const std::vector<SectionRenderData>& sections) {
        if (sections.empty() || !m_blockShader) {
            return;
        }

        // Use shader and setup uniforms
        m_blockShader->Use();
        SetupShaderUniforms(camera);
        BindTextureAtlas();

        // Render each section
        for (const auto& section : sections) {
            RenderSectionLayer(section, layer);
            m_stats.totalDrawCalls++;
        }

        CheckShaderErrors("RenderLayer");
    }

    void ChunkRenderer::RenderSectionLayer(const SectionRenderData& section, RenderLayer layer) {
        if (!section.gpuData) {
            return;
        }

        GLuint vao = 0;
        uint32_t indexCount = 0;

        // Select appropriate VAO and index count based on layer
        switch (layer) {
            case RenderLayer::Opaque:
                vao = section.gpuData->opaqueVAO;
                indexCount = section.gpuData->opaqueIndexCount;
                break;
            case RenderLayer::Cutout:
                vao = section.gpuData->cutoutVAO;
                indexCount = section.gpuData->cutoutIndexCount;
                break;
            case RenderLayer::Translucent:
                vao = section.gpuData->translucentVAO;
                indexCount = section.gpuData->translucentIndexCount;
                break;
        }

        if (vao == 0 || indexCount == 0) {
            return;
        }

        // Bind and draw
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);

        // Update statistics
        m_stats.totalVerticesRendered += indexCount / 6 * 4; // Approximate vertices from indices
        m_stats.totalIndicesRendered += indexCount;
    }

    void ChunkRenderer::PerformFrustumCulling(const Frustum& frustum, std::vector<SectionRenderData>& sections) {
        int culled = 0;

        for (auto& section : sections) {
            section.inFrustum = IsSectionInFrustum(frustum, section.chunkPos, section.sectionY);
            if (!section.inFrustum) {
                culled++;
            }
        }

        // Remove culled sections
        sections.erase(
            std::remove_if(sections.begin(), sections.end(),
                          [](const SectionRenderData& section) { return !section.inFrustum; }),
            sections.end()
        );

        m_stats.sectionsSkipped = culled;
        //Log::Debug("Frustum culling: %d sections culled, %zu remaining", culled, sections.size());
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
        m_stats.sortTimeMs += sortTime;
    }

    void ChunkRenderer::SetupRenderPass(const RenderPassConfig& config) {
        // Depth testing
        if (config.enableDepthTest) {
            glEnable(GL_DEPTH_TEST);
        } else {
            glDisable(GL_DEPTH_TEST);
        }

        // Depth writing
        glDepthMask(config.enableDepthWrite ? GL_TRUE : GL_FALSE);

        // Blending
        if (config.enableBlending) {
            glEnable(GL_BLEND);
            glBlendFunc(config.blendSrc, config.blendDst);
        } else {
            glDisable(GL_BLEND);
        }

        // Alpha testing (handled in shader)
        // Set alpha threshold uniform if needed

        // Face culling
        if (config.enableBackFaceCulling) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        } else {
            glDisable(GL_CULL_FACE);
        }
    }

    void ChunkRenderer::RestoreRenderState() {
        // Restore default OpenGL state
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);

        if (m_wireframeMode) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
    }

    float ChunkRenderer::CalculateSectionDistance(const Camera& camera, Game::Math::ChunkPos chunkPos, int sectionY) {
        // Calculate distance from camera to section center
        float sectionCenterX = chunkPos.x * Game::Math::CHUNK_SIZE_X + Game::Math::CHUNK_SIZE_X * 0.5f;
        float sectionCenterY = sectionY * Game::Math::SECTION_HEIGHT + Game::Math::SECTION_HEIGHT * 0.5f + Config::MinY;
        float sectionCenterZ = chunkPos.z * Game::Math::CHUNK_SIZE_Z + Game::Math::CHUNK_SIZE_Z * 0.5f;

        glm::vec3 sectionCenter(sectionCenterX, sectionCenterY, sectionCenterZ);
        return glm::length(camera.position - sectionCenter);
    }

    bool ChunkRenderer::IsSectionInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos, int sectionY) {
        AABB sectionAABB = GetSectionAABB(chunkPos, sectionY);
        return frustum.IsBoxVisible(sectionAABB);
    }

    AABB ChunkRenderer::GetSectionAABB(Game::Math::ChunkPos chunkPos, int sectionY) {
        float minX = chunkPos.x * Game::Math::CHUNK_SIZE_X;
        float maxX = minX + Game::Math::CHUNK_SIZE_X;
        float minY = sectionY * Game::Math::SECTION_HEIGHT + Config::MinY;
        float maxY = minY + Game::Math::SECTION_HEIGHT;
        float minZ = chunkPos.z * Game::Math::CHUNK_SIZE_Z;
        float maxZ = minZ + Game::Math::CHUNK_SIZE_Z;

        AABB aabb;
        aabb.min = glm::vec3(minX, minY, minZ);
        aabb.max = glm::vec3(maxX, maxY, maxZ);
        return aabb;
    }

    void ChunkRenderer::SetupShaderUniforms(const Camera& camera) {
        if (!m_blockShader) return;

        // Calculate matrices
        glm::mat4 view = camera.GetViewMatrix();
        glm::mat4 proj = glm::perspective(glm::radians(camera.fov), 16.0f/9.0f, 0.01f, 500.0f);
        glm::mat4 mvp = proj * view;

        // Set MVP matrix
        m_blockShader->SetMat4("uMVP", mvp);

        // Set other uniforms as needed
        // BiomeTint, time for animations, etc.
    }

    void ChunkRenderer::BindTextureAtlas() {
        if (g_atlasBuilder && g_atlasBuilder->GetAtlasTextureID() != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_atlasBuilder->GetAtlasTextureID());

            // **DEBUG**: Verify texture binding
            GLint boundTexture;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);
            static int bindLogCount = 0;
            if (bindLogCount < 3) {
                bindLogCount++;
                Log::Debug("BindTextureAtlas %d: Atlas ID=%u, bound texture=%d",
                          bindLogCount, g_atlasBuilder->GetAtlasTextureID(), boundTexture);
            }
        } else {
            static int noTextureLogCount = 0;
            if (noTextureLogCount < 3) {
                noTextureLogCount++;
                Log::Error("BindTextureAtlas %d: No atlas available! AtlasBuilder=%p",
                          noTextureLogCount, g_atlasBuilder.get());
            }
        }
    }

    void ChunkRenderer::RenderSectionBounds(const Camera& camera, const std::vector<SectionRenderData>& sections) {
        // Debug rendering - would implement wireframe section bounds here
        // This is a placeholder for debug visualization
    }

    void ChunkRenderer::RenderDebugInfo(const Camera& camera) {
        // Debug UI rendering - placeholder
    }

    bool ChunkRenderer::CheckShaderErrors(const std::string& pass) {
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            LogRenderError(pass, error);
            return false;
        }
        return true;
    }

    void ChunkRenderer::LogRenderError(const std::string& operation, GLenum error) {
        const char* errorString = "Unknown";
        switch (error) {
            case GL_INVALID_ENUM: errorString = "GL_INVALID_ENUM"; break;
            case GL_INVALID_VALUE: errorString = "GL_INVALID_VALUE"; break;
            case GL_INVALID_OPERATION: errorString = "GL_INVALID_OPERATION"; break;
            case GL_OUT_OF_MEMORY: errorString = "GL_OUT_OF_MEMORY"; break;
        }
        Log::Error("OpenGL error in %s: %s (0x%x)", operation.c_str(), errorString, error);
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

        // Translucent pass configuration
        m_translucentConfig.enableDepthWrite = false; // No depth write for translucent
        m_translucentConfig.enableDepthTest = true;
        m_translucentConfig.enableBlending = true;
        m_translucentConfig.enableAlphaTest = false;
        m_translucentConfig.enableBackFaceCulling = false; // Disable for correct blending
        m_translucentConfig.blendSrc = GL_SRC_ALPHA;
        m_translucentConfig.blendDst = GL_ONE_MINUS_SRC_ALPHA;
        m_translucentConfig.frontToBack = false; // Back to front sorting
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

    void RenderChunksOpaque(const Camera& camera, const Frustum& frustum) {
        if (g_chunkRenderer) {
            g_chunkRenderer->RenderOpaque(camera, frustum);
        }
    }

    void RenderChunksCutout(const Camera& camera, const Frustum& frustum) {
        if (g_chunkRenderer) {
            g_chunkRenderer->RenderCutout(camera, frustum);
        }
    }

    void RenderChunksTranslucent(const Camera& camera, const Frustum& frustum) {
        if (g_chunkRenderer) {
            g_chunkRenderer->RenderTranslucent(camera, frustum);
        }
    }

    void RenderChunksAll(const Camera& camera, const Frustum& frustum) {
        if (g_chunkRenderer) {
            g_chunkRenderer->RenderAll(camera, frustum);
        }
    }

} // namespace Render