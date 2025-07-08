// File: src/render/mesh/LayeredRenderer.cpp
#include "ChunkRenderer.hpp"
#include "../gfx/Camera.hpp"
#include "../gfx/Frustum.hpp"
#include "../gfx/Shader.hpp"
#include "../../core/Log.hpp"
#include <algorithm>
#include <vector>

namespace Render {

    // Enhanced rendering statistics
    struct LayeredRenderStats {
        int opaqueDrawCalls = 0;
        int cutoutDrawCalls = 0;
        int translucentDrawCalls = 0;
        size_t opaqueVertices = 0;
        size_t cutoutVertices = 0;
        size_t translucentVertices = 0;
        float sortTime = 0.0f;
    };

    // Sort chunks back-to-front for translucent rendering
    void SortChunksBackToFront(std::vector<ChunkMesh*>& chunks, const glm::vec3& cameraPos) {
        auto start = std::chrono::high_resolution_clock::now();

        std::sort(chunks.begin(), chunks.end(),
            [&cameraPos](const ChunkMesh* a, const ChunkMesh* b) {
                float distA = a->GetDistanceFromCamera(cameraPos);
                float distB = b->GetDistanceFromCamera(cameraPos);
                return distA > distB; // Back to front (greater distance first)
            });

        auto end = std::chrono::high_resolution_clock::now();
        float sortTime = std::chrono::duration<float, std::milli>(end - start).count();

        static int sortCounter = 0;
        if (++sortCounter % 60 == 0) { // Log every second at 60fps
            Log::Debug("Translucent chunk sort: %zu chunks in %.2fms", chunks.size(), sortTime);
        }
    }

    // Render all chunks with three-layer approach
    LayeredRenderStats RenderLayeredChunks(const Camera& camera, const Shader& blockShader,
                                          const glm::mat4& proj, const glm::mat4& view,
                                          const Frustum& frustum) {
        LayeredRenderStats stats;

        // Collect visible chunks
        std::vector<ChunkMesh*> visibleChunks;
        std::vector<ChunkMesh*> translucentChunks;

        for (auto& chunkMesh : g_chunkMeshes) {
            AABB box = chunkMesh.GetAABB();
            if (frustum.IsBoxVisible(box)) {
                visibleChunks.push_back(&chunkMesh);

                // Also collect chunks with translucent geometry for sorting
                if (chunkMesh.translucent.HasGeometry()) {
                    translucentChunks.push_back(&chunkMesh);
                }
            }
        }

        // Use the shader
        blockShader.Use();

        // === OPAQUE PASS ===
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_ALPHA_TEST);

        for (const auto* chunkMesh : visibleChunks) {
            if (chunkMesh->opaque.HasGeometry()) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), chunkMesh->worldOffset);
                glm::mat4 mvp = proj * view * model;
                blockShader.SetMat4("uMVP", mvp);

                chunkMesh->DrawLayer(RenderLayer::Opaque);
                stats.opaqueDrawCalls++;
                stats.opaqueVertices += chunkMesh->opaque.indexCount;
            }
        }

        // === CUTOUT PASS (Alpha Test) ===
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f); // Discard fragments with alpha <= 0.5

        for (const auto* chunkMesh : visibleChunks) {
            if (chunkMesh->cutout.HasGeometry()) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), chunkMesh->worldOffset);
                glm::mat4 mvp = proj * view * model;
                blockShader.SetMat4("uMVP", mvp);

                chunkMesh->DrawLayer(RenderLayer::Cutout);
                stats.cutoutDrawCalls++;
                stats.cutoutVertices += chunkMesh->cutout.indexCount;
            }
        }

        glDisable(GL_ALPHA_TEST);

        // === TRANSLUCENT PASS (Blended) ===
        glDepthMask(GL_FALSE); // Don't write to depth buffer
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Sort translucent chunks back-to-front
        if (!translucentChunks.empty()) {
            auto sortStart = std::chrono::high_resolution_clock::now();
            SortChunksBackToFront(translucentChunks, camera.position);
            auto sortEnd = std::chrono::high_resolution_clock::now();
            stats.sortTime = std::chrono::duration<float, std::milli>(sortEnd - sortStart).count();

            // Render sorted translucent chunks
            for (const auto* chunkMesh : translucentChunks) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), chunkMesh->worldOffset);
                glm::mat4 mvp = proj * view * model;
                blockShader.SetMat4("uMVP", mvp);

                chunkMesh->DrawLayer(RenderLayer::Translucent);
                stats.translucentDrawCalls++;
                stats.translucentVertices += chunkMesh->translucent.indexCount;
            }
        }

        // Restore OpenGL state
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        return stats;
    }

    // Enhanced scene rendering function for PlatformMain.cpp
    void RenderLayeredScene(const Camera& camera, const Shader& blockShader,
                           const glm::mat4& proj, const glm::mat4& view, const Frustum& frustum,
                           Debug::PerformanceMetrics& metrics) {

        auto renderStart = std::chrono::high_resolution_clock::now();

        // Use shader
        blockShader.Use();

        // Bind textures (same as before)
        if (g_atlasBuilder && g_atlasBuilder->GetAtlasTextureID() != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_atlasBuilder->GetAtlasTextureID());
            glUniform1i(blockShader.GetUniformLocation("uTextureAtlas"), 0);

            GLuint grassColormap = g_atlasBuilder->GetGrassColormapID();
            GLuint foliageColormap = g_atlasBuilder->GetFoliageColormapID();

            if (grassColormap != 0) {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, grassColormap);
                glUniform1i(blockShader.GetUniformLocation("uGrassColormap"), 1);
            }

            if (foliageColormap != 0) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, foliageColormap);
                glUniform1i(blockShader.GetUniformLocation("uFoliageColormap"), 2);
            }

            glUniform1f(blockShader.GetUniformLocation("uBiomeTemperature"), 0.7f);
            glUniform1f(blockShader.GetUniformLocation("uBiomeHumidity"), 0.6f);
            glUniform1i(blockShader.GetUniformLocation("uEnableBiomeTinting"), 1);
        }

        // Render with layered approach
        LayeredRenderStats layeredStats = RenderLayeredChunks(camera, blockShader, proj, view, frustum);

        // Update metrics
        metrics.meshesRenderedThisFrame = layeredStats.opaqueDrawCalls +
                                        layeredStats.cutoutDrawCalls +
                                        layeredStats.translucentDrawCalls;
        metrics.totalVerticesRendered = layeredStats.opaqueVertices +
                                      layeredStats.cutoutVertices +
                                      layeredStats.translucentVertices;
        metrics.totalIndicesRendered = metrics.totalVerticesRendered; // Approximate

        auto renderEnd = std::chrono::high_resolution_clock::now();
        metrics.renderTime = std::chrono::duration<float, std::milli>(renderEnd - renderStart).count();

        // Log detailed stats occasionally
        static int logCounter = 0;
        if (++logCounter % 300 == 0) { // Every 5 seconds at 60fps
            Log::Info("Layered Rendering Stats - Opaque: %d calls (%zu verts), Cutout: %d calls (%zu verts), "
                     "Translucent: %d calls (%zu verts), Sort time: %.2fms",
                     layeredStats.opaqueDrawCalls, layeredStats.opaqueVertices,
                     layeredStats.cutoutDrawCalls, layeredStats.cutoutVertices,
                     layeredStats.translucentDrawCalls, layeredStats.translucentVertices,
                     layeredStats.sortTime);
        }
    }

} // namespace Render