// File: src/client/renderer/mesh/ChunkRenderer.cpp
#include "ChunkRenderer.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "platform/GameDirectory.hpp"
#include "../../world/ClientChunkManager.hpp"
#include <algorithm>
#include <chrono>
#include <glm/gtc/matrix_transform.hpp>

namespace Render {

    // Global chunk renderer instance
    std::unique_ptr<ChunkRenderer> g_chunkRenderer = nullptr;

    ChunkRenderer::ChunkRenderer() {
        SetupRenderConfigs();
        m_visibleSections.reserve(768); // Reserve space for performance
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
        m_translucentConfig.enableDepthWrite = false;  // Read depth but don't write
        m_translucentConfig.enableDepthTest = true;
        m_translucentConfig.enableBlending = true;
        m_translucentConfig.enableAlphaTest = false;
        m_translucentConfig.enableBackFaceCulling = false;  // Disable culling for translucent blocks like water
        m_translucentConfig.blendSrc = GL_SRC_ALPHA;
        m_translucentConfig.blendDst = GL_ONE_MINUS_SRC_ALPHA;
        m_translucentConfig.frontToBack = false;  // Back-to-front for proper blending
    }

    bool ChunkRenderer::Initialize() {
        Log::Info("Initializing ChunkRenderer...");

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

        Log::Info("✓ ChunkRenderer initialized successfully (render distance: %d chunks)", Platform::g_gameSettings.GetRenderDistance());
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
        Log::Info("ChunkRenderer settings refreshed (render distance: %d chunks)", Platform::g_gameSettings.GetRenderDistance());
    }


    void ChunkRenderer::RenderOpaque(const Camera& camera, const Frustum& frustum) {
        if (!m_shadersLoaded || !g_clientMeshManager || (m_debugLayer >= 0 && m_debugLayer != 0)) {
            Log::Debug("RenderOpaque early exit: shaders=%d, clientMeshManager=%p, debugLayer=%d",
                      m_shadersLoaded, (void*)g_clientMeshManager.get(), m_debugLayer);
            m_stats.opaquePassTimeMs = 0.0f;
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Don't call PrepareVisibleSections here - it should be called once in RenderAll
        // Just filter the already-prepared sections for opaque geometry
        std::vector<SectionRenderData> opaqueSections;
        for (const auto& section : m_visibleSections) {
            if (section.gpuData && section.gpuData->opaqueIndexCount > 0) {
                opaqueSections.push_back(section);
            }
        }

        if (opaqueSections.empty()) {
            m_stats.opaquePassTimeMs = 0.0f;
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
        m_stats.opaquePassTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    void ChunkRenderer::RenderCutout(const Camera& camera, const Frustum& frustum) {
        if (!m_shadersLoaded || !g_clientMeshManager || (m_debugLayer >= 0 && m_debugLayer != 1)) {
            m_stats.cutoutPassTimeMs = 0.0f;
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Filter for sections with cutout geometry from already-prepared sections
        std::vector<SectionRenderData> cutoutSections;
        for (const auto& section : m_visibleSections) {
            if (section.gpuData && section.gpuData->cutoutIndexCount > 0) {
                cutoutSections.push_back(section);
            }
        }

        if (cutoutSections.empty()) {
            m_stats.cutoutPassTimeMs = 0.0f;
            return;
        }

        // Sort front to back for depth buffer optimization
        SortSections(camera, cutoutSections, true);

        // Setup render state for cutout pass
        SetupRenderPass(m_cutoutConfig);

        // Render the layer
        RenderLayerPass(RenderLayer::Cutout, camera, cutoutSections);

        m_stats.cutoutSections = static_cast<int>(cutoutSections.size());

        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.cutoutPassTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        //Log::Debug("Rendered %zu cutout sections in %.2f ms", cutoutSections.size(), m_stats.cutoutPassTimeMs);
    }

    void ChunkRenderer::RenderTranslucent(const Camera& camera, const Frustum& frustum) {
        if (!m_shadersLoaded || !g_clientMeshManager || (m_debugLayer >= 0 && m_debugLayer != 2)) {
            m_stats.translucentPassTimeMs = 0.0f;
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Filter for sections with translucent geometry from already-prepared sections
        std::vector<SectionRenderData> translucentSections;
        for (const auto& section : m_visibleSections) {
            if (section.gpuData && section.gpuData->translucentIndexCount > 0) {
                translucentSections.push_back(section);
            }
        }

        if (translucentSections.empty()) {
            m_stats.translucentPassTimeMs = 0.0f;
            return;
        }

        // Sort back to front for correct alpha blending
        SortSections(camera, translucentSections, false);

        // Setup render state for translucent pass
        SetupRenderPass(m_translucentConfig);

        // Render the layer
        RenderLayerPass(RenderLayer::Translucent, camera, translucentSections);

        m_stats.translucentSections = static_cast<int>(translucentSections.size());

        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.translucentPassTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        //Log::Debug("Rendered %zu translucent sections in %.2f ms", translucentSections.size(), m_stats.translucentPassTimeMs);
    }

    void ChunkRenderer::RenderAll(const Camera& camera, const Frustum& frustum) {
        m_stats.Reset();

        // Clear visible sections cache for fresh frame
        m_visibleSections.clear();

        // Prepare visible sections ONCE at the beginning of the frame
        // This includes chunk iteration, GPU data loading, and frustum culling
        PrepareVisibleSections(camera, frustum);

        // Render in correct order for proper blending
        RenderOpaque(camera, frustum);
        RenderCutout(camera, frustum);
        RenderTranslucent(camera, frustum);

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
        if (enable) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
    }

    void ChunkRenderer::PrepareVisibleSections(const Camera& camera, const Frustum& frustum) {
        auto overallStartTime = std::chrono::high_resolution_clock::now();

        m_visibleSections.clear();

        if (!g_clientMeshManager) {
            Log::Debug("PrepareVisibleSections: No client mesh manager available");
            m_stats.buildDrawListsTimeMs = 0.0f;
            return;
        }

        // Get chunk manager for chunk loading status checks
        auto* chunkManager = Client::g_clientChunkManager.get();
        if (!chunkManager) {
            Log::Debug("PrepareVisibleSections: No client chunk manager available");
            m_stats.buildDrawListsTimeMs = 0.0f;
            return;
        }

        // Calculate player chunk position for square rendering
        glm::vec3 playerPos = camera.position;
        int playerChunkX = static_cast<int>(std::floor(playerPos.x / ::Game::Math::CHUNK_SIZE_X));
        int playerChunkZ = static_cast<int>(std::floor(playerPos.z / ::Game::Math::CHUNK_SIZE_Z));

        // Get render distance from settings (already in chunks)
        int renderDistanceChunks = Platform::g_gameSettings.GetRenderDistance();
        int renderDistanceSquared = renderDistanceChunks * renderDistanceChunks;

        int totalSectionsChecked = 0;        // Total sections we looked at
        int sectionsWithGeometry = 0;        // Sections that passed frustum AND have geometry
        int sectionsCulled = 0;              // Sections culled by frustum
        int sectionsOutOfRange = 0;         // Sections outside render distance

        // Measure chunk iteration time
        auto iterationStart = std::chrono::high_resolution_clock::now();
        const auto& activeSections = g_clientMeshManager->GetActiveSections();
        auto iterationEnd = std::chrono::high_resolution_clock::now();
        m_stats.chunkIterationTimeMs = std::chrono::duration<float, std::milli>(iterationEnd - iterationStart).count();
        
        // Measure frustum culling and GPU data access time
        auto cullingStart = std::chrono::high_resolution_clock::now();
        float gpuDataAccessTime = 0.0f;
        
        for (const auto& sectionKey : activeSections) {
            ::Game::Math::ChunkPos chunkPos = sectionKey.chunkPos;
            int sectionY = sectionKey.sectionY;
            
            totalSectionsChecked++;
            
            // Check if chunk is within render distance (square pattern)
            int dx = chunkPos.x - playerChunkX;
            int dz = chunkPos.z - playerChunkZ;
            
            // Use square distance check for square render area
            if (std::abs(dx) > renderDistanceChunks || std::abs(dz) > renderDistanceChunks) {
                sectionsOutOfRange++;
                continue;  // Outside render distance
            }
            
            // Perform frustum culling
            if (m_enableFrustumCulling) {
                // Calculate section bounds directly without creating AABB object
                float sectionMinX = static_cast<float>(chunkPos.x * ::Game::Math::CHUNK_SIZE_X);
                float sectionMaxX = sectionMinX + ::Game::Math::CHUNK_SIZE_X;
                float sectionMinY = static_cast<float>(sectionY * ::Game::Math::SECTION_HEIGHT + Config::MinY);
                float sectionMaxY = sectionMinY + ::Game::Math::SECTION_HEIGHT;
                float sectionMinZ = static_cast<float>(chunkPos.z * ::Game::Math::CHUNK_SIZE_Z);
                float sectionMaxZ = sectionMinZ + ::Game::Math::CHUNK_SIZE_Z;
                
                glm::vec3 sectionMin(sectionMinX, sectionMinY, sectionMinZ);
                glm::vec3 sectionMax(sectionMaxX, sectionMaxY, sectionMaxZ);
                
                if (!frustum.IsBoxVisible(sectionMin, sectionMax)) {
                    sectionsCulled++;
                    continue; // Outside frustum
                }
            }
            
            // Get GPU data - measure this access time
            auto gpuAccessStart = std::chrono::high_resolution_clock::now();
            const auto* gpuData = g_clientMeshManager->GetSectionGPUData(chunkPos, sectionY);
            auto gpuAccessEnd = std::chrono::high_resolution_clock::now();
            gpuDataAccessTime += std::chrono::duration<float, std::milli>(gpuAccessEnd - gpuAccessStart).count();
            
            if (gpuData && gpuData->HasGeometry()) {
                sectionsWithGeometry++;
                float sectionDistance = CalculateSectionDistance(camera, chunkPos, sectionY);
                
                SectionRenderData renderData(chunkPos, sectionY, gpuData, sectionDistance);
                renderData.inFrustum = true; // Already passed frustum test
                
                m_visibleSections.push_back(renderData);
            }
        }
        
        auto cullingEnd = std::chrono::high_resolution_clock::now();
        float totalCullingTime = std::chrono::duration<float, std::milli>(cullingEnd - cullingStart).count();
        
        // Frustum culling time is total loop time minus GPU data access time
        m_stats.frustumCullingTimeMs = totalCullingTime - gpuDataAccessTime;
        m_stats.gpuDataLoadTimeMs = gpuDataAccessTime;

        // Track simple statistics
        m_stats.sectionsAvailable = totalSectionsChecked;       // Total sections we checked
        m_stats.sectionsSkipped = sectionsCulled;               // Sections culled by frustum
        m_stats.sectionsRendered = sectionsWithGeometry;        // Sections actually being rendered

        // Measure sorting time if we have visible sections
        auto sortingStart = std::chrono::high_resolution_clock::now();
        if (!m_visibleSections.empty()) {
            // Sort sections by distance for proper rendering order (if needed)
            std::sort(m_visibleSections.begin(), m_visibleSections.end(),
                     [](const SectionRenderData& a, const SectionRenderData& b) {
                         return a.distanceToCamera < b.distanceToCamera;
                     });
        }
        auto sortingEnd = std::chrono::high_resolution_clock::now();
        m_stats.sortingTimeMs = std::chrono::duration<float, std::milli>(sortingEnd - sortingStart).count();

        // Total build time for draw lists (entire PrepareVisibleSections time)
        auto overallEndTime = std::chrono::high_resolution_clock::now();
        m_stats.buildDrawListsTimeMs = std::chrono::duration<float, std::milli>(overallEndTime - overallStartTime).count();
        
        // Note: The individual phase timings (chunkIterationTimeMs, gpuDataLoadTimeMs, frustumCullingTimeMs, sortingTimeMs)
        // are now measured accurately and should sum to approximately buildDrawListsTimeMs
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
        // This function is now deprecated - frustum culling is performed earlier in PrepareVisibleSections
        // Kept for compatibility but does nothing as sections are already culled
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
        // Depth testing - Minecraft uses GL_LEQUAL for all passes
        if (config.enableDepthTest) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);  // Minecraft standard
        } else {
            glDisable(GL_DEPTH_TEST);
        }

        // Depth writing
        glDepthMask(config.enableDepthWrite ? GL_TRUE : GL_FALSE);

        // Blending - use config values for blend function
        if (config.enableBlending) {
            glEnable(GL_BLEND);
            glBlendFunc(config.blendSrc, config.blendDst);
        } else {
            glDisable(GL_BLEND);
        }

        // Alpha testing (handled in shader)
        // Set alpha threshold uniform if needed

        // Face culling with proper winding order
        if (config.enableBackFaceCulling) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);  // Counter-clockwise front faces
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

    float ChunkRenderer::CalculateSectionDistance(const Camera& camera, ::Game::Math::ChunkPos chunkPos, int sectionY) {
        // Calculate distance from camera to section center
        // Use XZ distance primarily for chunk-based decisions, with Y as secondary factor for sorting
        float sectionCenterX = chunkPos.x * ::Game::Math::CHUNK_SIZE_X + ::Game::Math::CHUNK_SIZE_X * 0.5f;
        float sectionCenterY = sectionY * ::Game::Math::SECTION_HEIGHT + ::Game::Math::SECTION_HEIGHT * 0.5f + Config::MinY;
        float sectionCenterZ = chunkPos.z * ::Game::Math::CHUNK_SIZE_Z + ::Game::Math::CHUNK_SIZE_Z * 0.5f;

        // Calculate XZ distance (horizontal distance for chunk-based decisions)
        float dx = sectionCenterX - camera.position.x;
        float dz = sectionCenterZ - camera.position.z;
        float xzDistance = std::sqrt(dx * dx + dz * dz);
        
        // Add Y distance as secondary factor (less weight for sorting within chunk)
        float dy = sectionCenterY - camera.position.y;
        float yDistance = std::abs(dy);
        
        // Combined distance: XZ is primary, Y is secondary with less weight
        // This ensures proper square chunk pattern while still prioritizing closer sections
        return xzDistance + yDistance * 0.1f;
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

    // Essential utility methods still needed by the optimized rendering system
    void ChunkRenderer::SetupShaderUniforms(const Camera& camera) {
        if (!m_blockShader) return;

        // Get current viewport dimensions to calculate correct aspect ratio
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        int width = viewport[2];
        int height = viewport[3];
        
        // Calculate aspect ratio, with fallback to prevent division by zero
        float aspectRatio = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);

        // Calculate matrices
        glm::mat4 view = camera.GetViewMatrix();
        glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspectRatio, 0.01f, 800.0f);
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
            case GL_INVALID_FRAMEBUFFER_OPERATION: errorString = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
            case GL_OUT_OF_MEMORY: errorString = "GL_OUT_OF_MEMORY"; break;
        }
        Log::Error("OpenGL error in %s: %s (0x%x)", operation.c_str(), errorString, error);
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
    
    const RenderStats* GetChunkRendererStats() {
        if (g_chunkRenderer) {
            return &g_chunkRenderer->GetStats();
        }
        return nullptr;
    }

} // namespace Render
