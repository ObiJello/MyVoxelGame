// File: src/client/renderer/mesh/ChunkRenderer.cpp
#include "ChunkRenderer.hpp"
#include "ChunkMegaBuffer.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "../backend/RenderBackend.hpp"
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

    ChunkRenderer::ChunkRenderer() {
        SetupRenderConfigs();
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

        // Create separate shader programs for opaque (no discard → early-z enabled)
        // and cutout/translucent (with discard for alpha testing).
        // Matches Minecraft's SOLID_TERRAIN vs CUTOUT_TERRAIN pipeline split.
        m_opaqueShader = g_renderBackend->CreateShaderFromFiles("shaders/block.vert", "shaders/block_opaque.frag");
        if (m_opaqueShader == INVALID_SHADER) {
            Log::Error("Failed to create opaque block shader");
            return false;
        }
        m_cutoutShader = g_renderBackend->CreateShaderFromFiles("shaders/block.vert", "shaders/block.frag");
        if (m_cutoutShader == INVALID_SHADER) {
            Log::Error("Failed to create cutout block shader");
            return false;
        }
        m_solidShader = g_renderBackend->CreateShaderFromFiles("shaders/block.vert", "shaders/block_solid.frag");
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

        // Setup render state for translucent pass
        SetupRenderPass(m_translucentConfig);

        // Render directly from m_visibleSections in reverse (back-to-front for blending)
        RenderLayerPass(RenderLayer::Translucent, LayerTranslucent, true);

        auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.translucentPassTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    }

    void ChunkRenderer::RenderAll(const Camera& camera, const Frustum& frustum) {
        PROFILE_ZONE;
        m_stats.Reset();

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
        // pipeline flush that glBindVertexArray causes on macOS.
        if (g_clientMeshManager) {
            g_clientMeshManager->BindSharedBlockVAO();
        }

        // Opaque pass: uses opaque shader (no discard → early-z enabled)
        RenderOpaque(camera, frustum);

        // Switch to cutout shader for cutout + translucent passes (has discard)
        if (m_cutoutShader != INVALID_SHADER && g_renderBackend) {
            m_activeShader = m_cutoutShader;
            g_renderBackend->BindShader(m_cutoutShader);
            g_renderBackend->SetUniformMat4(m_cutoutShader, "uMVP", m_cachedMVP);
            g_renderBackend->BindTexture(m_backendAtlasTexture, 0);
        }

        RenderCutout(camera, frustum);

        // Switch to solid shader for translucent pass (no discard → early-z enabled,
        // blending handles transparency). This avoids the discard penalty entirely.
        if (m_solidShader != INVALID_SHADER && g_renderBackend) {
            m_activeShader = m_solidShader;
            g_renderBackend->BindShader(m_solidShader);
            g_renderBackend->SetUniformMat4(m_solidShader, "uMVP", m_cachedMVP);
            g_renderBackend->BindTexture(m_backendAtlasTexture, 0);
        }

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
        PROFILE_ZONE;
        auto overallStartTime = std::chrono::high_resolution_clock::now();

        // --- Visible section caching ---
        // Skip full rebuild if camera hasn't moved to a new chunk or rotated significantly
        // and no sections have been uploaded/removed since last frame.
        int currentChunkX = static_cast<int>(std::floor(camera.position.x / 16.0f));
        int currentChunkZ = static_cast<int>(std::floor(camera.position.z / 16.0f));

        bool cameraChangedChunk = (currentChunkX != m_lastCameraChunkX ||
                                   currentChunkZ != m_lastCameraChunkZ);
        bool cameraRotated = (std::abs(camera.yaw - m_lastCameraYaw) > 3.0f ||
                              std::abs(camera.pitch - m_lastCameraPitch) > 3.0f);

        if (!m_visibleSectionsDirty && !cameraChangedChunk && !cameraRotated && !m_visibleSections.empty()) {
            // Reuse last frame's visible sections — just update timing stats
            auto overallEndTime = std::chrono::high_resolution_clock::now();
            m_stats.buildDrawListsTimeMs = std::chrono::duration<float, std::milli>(overallEndTime - overallStartTime).count();
            return;
        }

        // Update cache state
        m_lastCameraChunkX = currentChunkX;
        m_lastCameraChunkZ = currentChunkZ;
        m_lastCameraYaw = camera.yaw;
        m_lastCameraPitch = camera.pitch;
        m_visibleSectionsDirty = false;

        // --- Full rebuild (existing code below) ---
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

        // Get effective render distance: min(client setting, server cap)
        int renderDistanceChunks = Platform::g_gameSettings.GetRenderDistance();
        if (Client::g_networkClient && Client::g_networkClient->GetServerViewDistance() > 0) {
            renderDistanceChunks = std::min(renderDistanceChunks, Client::g_networkClient->GetServerViewDistance());
        }
        int renderDistanceSquared = renderDistanceChunks * renderDistanceChunks;

        int totalSectionsChecked = 0;        // Total sections we looked at
        int sectionsWithGeometry = 0;        // Sections that passed frustum AND have geometry
        int sectionsCulled = 0;              // Sections culled by frustum
        int sectionsOutOfRange = 0;         // Sections outside render distance

        // Iterate active sections under shared lock (zero-copy, GPU data passed directly)
        auto iterationStart = std::chrono::high_resolution_clock::now();

        // Cache chunk-level frustum results to avoid redundant per-section tests.
        // Sections in the same chunk share XZ bounds — if the full-height chunk AABB
        // is Outside, all 24 sections can be skipped; if Inside, per-section tests
        // can be skipped entirely.
        std::unordered_map<uint64_t, FrustumResult> chunkFrustumCache;
        int cacheSize = (2 * renderDistanceChunks + 1) * (2 * renderDistanceChunks + 1);
        chunkFrustumCache.reserve(cacheSize);

        g_clientMeshManager->ForEachActiveSection([&](const auto& sectionKey, const GPUSectionData* gpuData) {
            ::Game::Math::ChunkPos chunkPos = sectionKey.chunkPos;
            int sectionY = sectionKey.sectionY;

            totalSectionsChecked++;

            // Check if chunk is within render distance (square pattern)
            int dx = chunkPos.x - playerChunkX;
            int dz = chunkPos.z - playerChunkZ;

            // Use square distance check for square render area
            if (std::abs(dx) > renderDistanceChunks || std::abs(dz) > renderDistanceChunks) {
                sectionsOutOfRange++;
                return;  // Outside render distance
            }

            // Perform frustum culling with chunk-level pre-test
            if (m_enableFrustumCulling) {
                // Compute or retrieve chunk-level frustum result
                uint64_t chunkKey = (static_cast<uint64_t>(static_cast<uint32_t>(chunkPos.x)) << 32)
                                  | static_cast<uint64_t>(static_cast<uint32_t>(chunkPos.z));
                auto cacheIt = chunkFrustumCache.find(chunkKey);
                FrustumResult chunkResult;
                if (cacheIt != chunkFrustumCache.end()) {
                    chunkResult = cacheIt->second;
                } else {
                    // Test full-height chunk AABB (all 24 sections)
                    float minX = static_cast<float>(chunkPos.x * ::Game::Math::CHUNK_SIZE_X);
                    float maxX = minX + ::Game::Math::CHUNK_SIZE_X;
                    float minZ = static_cast<float>(chunkPos.z * ::Game::Math::CHUNK_SIZE_Z);
                    float maxZ = minZ + ::Game::Math::CHUNK_SIZE_Z;
                    chunkResult = frustum.TestAABB(
                        glm::vec3(minX, static_cast<float>(Config::MinY), minZ),
                        glm::vec3(maxX, static_cast<float>(Config::MaxY), maxZ)
                    );
                    chunkFrustumCache[chunkKey] = chunkResult;
                }

                if (chunkResult == FrustumResult::Outside) {
                    sectionsCulled++;
                    return;  // Entire chunk is outside frustum
                }

                if (chunkResult == FrustumResult::Intersect) {
                    // Chunk straddles frustum boundary — do per-section test
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
                        return; // Section outside frustum
                    }
                }
                // If chunkResult == FrustumResult::Inside, skip per-section test
            }

            // GPU data already passed in — no separate hash lookup needed
            if (gpuData && gpuData->HasGeometry()) {
                sectionsWithGeometry++;
                float sectionDistance = CalculateSectionDistance(camera, chunkPos, sectionY);

                SectionRenderData renderData(chunkPos, sectionY, gpuData, sectionDistance);
                renderData.inFrustum = true; // Already passed frustum test

                // Tag with layer bitmask so render passes can filter without copying
                uint8_t mask = 0;
                if (gpuData->opaqueIndexCount > 0)      mask |= LayerOpaque;
                if (gpuData->cutoutIndexCount > 0)       mask |= LayerCutout;
                if (gpuData->translucentIndexCount > 0)  mask |= LayerTranslucent;
                renderData.layerMask = mask;

                m_visibleSections.push_back(renderData);
            }
        });

        auto iterationEnd = std::chrono::high_resolution_clock::now();
        m_stats.chunkIterationTimeMs = std::chrono::duration<float, std::milli>(iterationEnd - iterationStart).count();
        m_stats.frustumCullingTimeMs = m_stats.chunkIterationTimeMs; // Combined in single pass now
        m_stats.gpuDataLoadTimeMs = 0.0f; // No separate GPU data lookup overhead

        // Track simple statistics
        m_stats.sectionsAvailable = totalSectionsChecked;       // Total sections we checked
        m_stats.sectionsSkipped = sectionsCulled;               // Sections culled by frustum
        m_stats.sectionsRendered = sectionsWithGeometry;        // Sections actually being rendered

        // Measure sorting time if we have enough visible sections to warrant sorting
        auto sortingStart = std::chrono::high_resolution_clock::now();
        if (m_visibleSections.size() > 1) {
            // Sort sections by distance for proper rendering order
            // Front-to-back for opaque early-z; translucent pass iterates in reverse
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
        glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.05f, farPlane);
        m_cachedMVP = proj * view;
        g_renderBackend->SetUniformMat4(m_opaqueShader, "uMVP", m_cachedMVP);

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
        bool useVAB = g_clientMeshManager && g_clientMeshManager->HasVertexAttribBinding();

        // Resize per-slab draw command vectors (reuse allocations across frames)
        m_perSlabCounts.resize(slabCount);
        m_perSlabOffsets.resize(slabCount);
        m_perSlabBaseVertices.resize(slabCount);
        for (uint32_t s = 0; s < slabCount; s++) {
            m_perSlabCounts[s].clear();
            m_perSlabOffsets[s].clear();
            m_perSlabBaseVertices[s].clear();
        }

        int layerCount = 0;
        uint32_t totalVerts = 0, totalIndices = 0;

        // Group visible sections by slab index for per-slab multi-draw.
        auto processSection = [&](const SectionRenderData& section) {
            if (!section.gpuData) return;
            if (!(section.layerMask & layerBit)) return;

            const auto& cachedCmd = (layer == RenderLayer::Opaque)      ? section.gpuData->opaqueDrawCmd :
                                    (layer == RenderLayer::Cutout)       ? section.gpuData->cutoutDrawCmd :
                                                                           section.gpuData->translucentDrawCmd;
            if (cachedCmd.valid && cachedCmd.indexCount > 0 && cachedCmd.slabIndex < slabCount) {
                m_perSlabCounts[cachedCmd.slabIndex].push_back(cachedCmd.indexCount);
                m_perSlabOffsets[cachedCmd.slabIndex].push_back(reinterpret_cast<const void*>(cachedCmd.indexByteOffset));
                m_perSlabBaseVertices[cachedCmd.slabIndex].push_back(cachedCmd.baseVertex);
                layerCount++;

                totalIndices += cachedCmd.indexCount;
                switch (layer) {
                    case RenderLayer::Opaque:      totalVerts += section.gpuData->opaqueVertexCount; break;
                    case RenderLayer::Cutout:       totalVerts += section.gpuData->cutoutVertexCount; break;
                    case RenderLayer::Translucent:  totalVerts += section.gpuData->translucentVertexCount; break;
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

        // Issue one multi-draw per slab that has sections
        for (uint32_t s = 0; s < slabCount; s++) {
            if (m_perSlabCounts[s].empty()) continue;
            megaBuffer->BindSlab(s, useVAB);
            glMultiDrawElementsBaseVertex(
                GL_TRIANGLES,
                m_perSlabCounts[s].data(),
                GL_UNSIGNED_INT,
                m_perSlabOffsets[s].data(),
                static_cast<GLsizei>(m_perSlabCounts[s].size()),
                m_perSlabBaseVertices[s].data()
            );
            m_stats.totalDrawCalls++;
        }

        m_stats.totalVerticesRendered += totalVerts;
        m_stats.totalIndicesRendered += totalIndices;

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
    
    const RenderStats* GetChunkRendererStats() {
        if (g_chunkRenderer) {
            return &g_chunkRenderer->GetStats();
        }
        return nullptr;
    }

} // namespace Render
