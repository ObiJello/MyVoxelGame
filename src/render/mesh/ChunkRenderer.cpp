// File: src/render/mesh/ChunkRenderer.cpp
#include "ChunkRenderer.hpp"

#include "debug/DebugSystem.hpp"
#include "gfx/Camera.hpp"
#include "gfx/Shader.hpp"

namespace Render {
    // Define the global vector
    std::vector<ChunkMesh> g_chunkMeshes;

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

    // Enhanced three-layer rendering system
    LayeredRenderStats RenderLayeredChunks(const Camera& camera, const Shader& blockShader,
                                          const glm::mat4& proj, const glm::mat4& view,
                                          const Frustum& frustum) {
        LayeredRenderStats stats;
        auto renderStart = std::chrono::high_resolution_clock::now();

        // Collect visible chunks and separate translucent chunks for sorting
        std::vector<ChunkMesh*> visibleChunks;
        std::vector<ChunkMesh*> translucentChunks;

        for (auto& chunkMesh : g_chunkMeshes) {
            AABB box = chunkMesh.GetAABB();
            if (frustum.IsBoxVisible(box)) {
                visibleChunks.push_back(&chunkMesh);
                stats.totalVisibleChunks++;

                // Collect chunks with translucent geometry for separate sorting
                if (chunkMesh.translucent.HasGeometry()) {
                    translucentChunks.push_back(&chunkMesh);
                }
            }
        }

        // Use the shader program
        blockShader.Use();

        // ===============================
        // PASS 1: OPAQUE RENDERING
        // ===============================
        glDepthMask(GL_TRUE);        // Write to depth buffer
        glDisable(GL_BLEND);         // No blending
        glDisable(GL_ALPHA_TEST);    // No alpha testing

        for (const auto* chunkMesh : visibleChunks) {
            if (chunkMesh->opaque.HasGeometry()) {
                // Set up model-view-projection matrix
                glm::mat4 model = glm::translate(glm::mat4(1.0f), chunkMesh->worldOffset);
                glm::mat4 mvp = proj * view * model;
                blockShader.SetMat4("uMVP", mvp);

                // Draw opaque geometry
                chunkMesh->DrawLayer(RenderLayer::Opaque);
                stats.opaqueDrawCalls++;
                stats.opaqueVertices += chunkMesh->opaque.indexCount;
            }
        }

        // ===============================
        // PASS 2: CUTOUT RENDERING (Alpha Test)
        // ===============================
        glDepthMask(GL_TRUE);        // Still write to depth buffer
        glDisable(GL_BLEND);         // No blending
        glEnable(GL_ALPHA_TEST);     // Enable alpha testing
        glAlphaFunc(GL_GREATER, 0.5f); // Discard fragments with alpha <= 0.5

        for (const auto* chunkMesh : visibleChunks) {
            if (chunkMesh->cutout.HasGeometry()) {
                // Set up model-view-projection matrix
                glm::mat4 model = glm::translate(glm::mat4(1.0f), chunkMesh->worldOffset);
                glm::mat4 mvp = proj * view * model;
                blockShader.SetMat4("uMVP", mvp);

                // Draw cutout geometry (leaves, etc.)
                chunkMesh->DrawLayer(RenderLayer::Cutout);
                stats.cutoutDrawCalls++;
                stats.cutoutVertices += chunkMesh->cutout.indexCount;
            }
        }

        glDisable(GL_ALPHA_TEST);    // Clean up alpha test state

        // ===============================
        // PASS 3: TRANSLUCENT RENDERING (Blended)
        // ===============================
        glDepthMask(GL_FALSE);       // Don't write to depth buffer (but still test)
        glEnable(GL_BLEND);          // Enable blending
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // Standard alpha blending

        // Sort translucent chunks back-to-front for proper blending
        if (!translucentChunks.empty()) {
            auto sortStart = std::chrono::high_resolution_clock::now();
            SortChunksBackToFront(translucentChunks, camera.position);
            auto sortEnd = std::chrono::high_resolution_clock::now();
            stats.sortTime = std::chrono::duration<float, std::milli>(sortEnd - sortStart).count();

            // Render sorted translucent chunks (fluids, glass, etc.)
            for (const auto* chunkMesh : translucentChunks) {
                // Set up model-view-projection matrix
                glm::mat4 model = glm::translate(glm::mat4(1.0f), chunkMesh->worldOffset);
                glm::mat4 mvp = proj * view * model;
                blockShader.SetMat4("uMVP", mvp);

                // Draw translucent geometry
                chunkMesh->DrawLayer(RenderLayer::Translucent);
                stats.translucentDrawCalls++;
                stats.translucentVertices += chunkMesh->translucent.indexCount;
            }
        }

        // ===============================
        // RESTORE OPENGL STATE
        // ===============================
        glDepthMask(GL_TRUE);        // Restore depth writing
        glDisable(GL_BLEND);         // Disable blending

        auto renderEnd = std::chrono::high_resolution_clock::now();
        float totalRenderTime = std::chrono::duration<float, std::milli>(renderEnd - renderStart).count();

        // Log detailed performance stats occasionally
        static int perfLogCounter = 0;
        if (++perfLogCounter % 300 == 0) { // Every 5 seconds at 60fps
            Log::Info("Layered Rendering Performance:");
            Log::Info("  Total chunks: %d visible, %zu total loaded",
                     stats.totalVisibleChunks, g_chunkMeshes.size());
            Log::Info("  Opaque: %d calls, %zu vertices",
                     stats.opaqueDrawCalls, stats.opaqueVertices);
            Log::Info("  Cutout: %d calls, %zu vertices",
                     stats.cutoutDrawCalls, stats.cutoutVertices);
            Log::Info("  Translucent: %d calls, %zu vertices",
                     stats.translucentDrawCalls, stats.translucentVertices);
            Log::Info("  Sort time: %.2fms, Total render: %.2fms",
                     stats.sortTime, totalRenderTime);
        }

        return stats;
    }

    // Enhanced scene rendering function that replaces the old RenderScene
    void RenderLayeredScene(const Camera& camera, const Shader& blockShader,
                           const glm::mat4& proj, const glm::mat4& view, const Frustum& frustum,
                           Debug::PerformanceMetrics& metrics) {

        auto renderStart = std::chrono::high_resolution_clock::now();

        // Bind texture atlas and colormaps
        if (g_atlasBuilder && g_atlasBuilder->GetAtlasTextureID() != 0) {
            // Main texture atlas
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_atlasBuilder->GetAtlasTextureID());
            glUniform1i(blockShader.GetUniformLocation("uTextureAtlas"), 0);

            // Biome colormaps
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

            // Set biome parameters
            glUniform1f(blockShader.GetUniformLocation("uBiomeTemperature"), 0.7f); // Temperate
            glUniform1f(blockShader.GetUniformLocation("uBiomeHumidity"), 0.6f);    // Moderate humidity
            glUniform1i(blockShader.GetUniformLocation("uEnableBiomeTinting"), 1);  // Enable tinting
        }

        // Perform three-layer rendering
        LayeredRenderStats layeredStats = RenderLayeredChunks(camera, blockShader, proj, view, frustum);

        // Update performance metrics for the debug system
        metrics.meshesRenderedThisFrame = layeredStats.opaqueDrawCalls +
                                        layeredStats.cutoutDrawCalls +
                                        layeredStats.translucentDrawCalls;
        metrics.totalVerticesRendered = layeredStats.opaqueVertices +
                                      layeredStats.cutoutVertices +
                                      layeredStats.translucentVertices;
        metrics.totalIndicesRendered = metrics.totalVerticesRendered; // Approximate

        auto renderEnd = std::chrono::high_resolution_clock::now();
        metrics.renderTime = std::chrono::duration<float, std::milli>(renderEnd - renderStart).count();
    }

    // Utility function to get rendering statistics
    LayeredRenderStats GetLastRenderStats() {
        static LayeredRenderStats lastStats;
        return lastStats;
    }

    // Utility function to check if layered rendering is working properly
    bool ValidateLayeredRendering() {
        int totalChunks = static_cast<int>(g_chunkMeshes.size());

        if (totalChunks == 0) {
            return true; // No chunks to validate
        }

        int chunksWithOpaque = 0;
        int chunksWithCutout = 0;
        int chunksWithTranslucent = 0;
        int emptyChunks = 0;

        for (const auto& chunk : g_chunkMeshes) {
            bool hasGeometry = false;

            if (chunk.opaque.HasGeometry()) {
                chunksWithOpaque++;
                hasGeometry = true;
            }
            if (chunk.cutout.HasGeometry()) {
                chunksWithCutout++;
                hasGeometry = true;
            }
            if (chunk.translucent.HasGeometry()) {
                chunksWithTranslucent++;
                hasGeometry = true;
            }

            if (!hasGeometry) {
                emptyChunks++;
            }
        }

        // Log validation results
        Log::Info("Layered Rendering Validation:");
        Log::Info("  Total chunks: %d", totalChunks);
        Log::Info("  Chunks with opaque geometry: %d", chunksWithOpaque);
        Log::Info("  Chunks with cutout geometry: %d", chunksWithCutout);
        Log::Info("  Chunks with translucent geometry: %d", chunksWithTranslucent);
        Log::Info("  Empty chunks: %d", emptyChunks);

        // Basic validation - at least some chunks should have opaque geometry
        return chunksWithOpaque > 0;
    }

    // Debug function to force regeneration of all chunks with layered meshing
    void RegenerateAllChunksLayered() {
        Log::Info("Regenerating all chunks with layered meshing...");

        // Clear existing meshes
        for (auto& chunk : g_chunkMeshes) {
            chunk.Cleanup();
        }
        g_chunkMeshes.clear();

        Log::Info("Cleared %zu existing chunk meshes, world manager will regenerate them", g_chunkMeshes.size());
    }
}
