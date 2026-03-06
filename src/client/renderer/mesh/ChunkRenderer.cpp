// File: src/client/renderer/mesh/ChunkRenderer.cpp
#include "ChunkRenderer.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "platform/GameDirectory.hpp"
#include "../../world/ClientChunkManager.hpp"
#include <algorithm>
#include <chrono>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>

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

        // Create backend shader (works for both GL and Vulkan)
        m_backendShader = g_renderBackend->CreateShaderFromFiles("shaders/block.vert", "shaders/block.frag");
        if (m_backendShader == INVALID_SHADER) {
            Log::Error("Failed to create block shader");
            return false;
        }
        m_shadersLoaded = true;
        Log::Info("Block shader created successfully");

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
        if (m_backendShader != INVALID_SHADER && g_renderBackend) {
            g_renderBackend->DestroyShader(m_backendShader);
            m_backendShader = INVALID_SHADER;
        }
        m_blockShader.reset();
        m_shadersLoaded = false;
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

        // Already sorted front-to-back by PrepareVisibleSections - no re-sort needed

        // Setup render state for opaque pass
        SetupRenderPass(m_opaqueConfig);

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

        // Already sorted front-to-back by PrepareVisibleSections - no re-sort needed

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

        // Reverse the front-to-back order from PrepareVisibleSections for back-to-front blending
        std::reverse(translucentSections.begin(), translucentSections.end());

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
        // Wireframe state is applied in SetupRenderPass via PipelineState
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
        if (sections.empty() || !g_renderBackend || m_backendShader == INVALID_SHADER) {
            return;
        }

        // Bind shader and set uniforms through backend (works for both GL and Vulkan)
        g_renderBackend->BindShader(m_backendShader);

        // Compute and set MVP
        int width, height;
        glfwGetFramebufferSize(g_renderBackend->GetWindow(), &width, &height);
        float aspect = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);
        glm::mat4 view = camera.GetViewMatrix();
        glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.01f, 800.0f);
        glm::mat4 mvp = proj * view;
        g_renderBackend->SetUniformMat4(m_backendShader, "uMVP", mvp);

        // Bind texture atlas (fetch fresh handle in case atlas was rebuilt)
        if (g_atlasBuilder) {
            m_backendAtlasTexture = g_atlasBuilder->GetBackendTextureHandle();
        }
        if (m_backendAtlasTexture != INVALID_TEXTURE) {
            g_renderBackend->BindTexture(m_backendAtlasTexture, 0);
        }

        // Render each section
        for (const auto& section : sections) {
            RenderSectionLayer(section, layer);
            m_stats.totalDrawCalls++;
        }
    }

    void ChunkRenderer::RenderSectionLayer(const SectionRenderData& section, RenderLayer layer) {
        if (!section.gpuData || !g_renderBackend) return;

        MeshHandle mesh = INVALID_MESH;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;

        switch (layer) {
            case RenderLayer::Opaque:
                mesh = section.gpuData->opaqueMesh;
                indexCount = section.gpuData->opaqueIndexCount;
                vertexCount = section.gpuData->opaqueVertexCount;
                break;
            case RenderLayer::Cutout:
                mesh = section.gpuData->cutoutMesh;
                indexCount = section.gpuData->cutoutIndexCount;
                vertexCount = section.gpuData->cutoutVertexCount;
                break;
            case RenderLayer::Translucent:
                mesh = section.gpuData->translucentMesh;
                indexCount = section.gpuData->translucentIndexCount;
                vertexCount = section.gpuData->translucentVertexCount;
                break;
        }

        if (indexCount == 0 || mesh == INVALID_MESH) return;

        g_renderBackend->DrawIndexed(mesh, indexCount);

        m_stats.totalVerticesRendered += vertexCount;
        m_stats.totalIndicesRendered += indexCount;
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
    
    const RenderStats* GetChunkRendererStats() {
        if (g_chunkRenderer) {
            return &g_chunkRenderer->GetStats();
        }
        return nullptr;
    }

} // namespace Render
