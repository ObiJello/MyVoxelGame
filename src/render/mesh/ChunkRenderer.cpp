// File: src/render/mesh/ChunkRenderer.cpp
#include "ChunkRenderer.hpp"
#include "../../core/Log.hpp"
#include "../../game/WorldMath.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <algorithm>

namespace Render {

    // Global instance
    std::unique_ptr<ChunkRenderer> g_chunkRenderer = nullptr;

    ChunkRenderer::ChunkRenderer() {
        // Set default configuration
        config = Config{};

        Log::Debug("ChunkRenderer created");
    }

    bool ChunkRenderer::Initialize(Shader& blockShader, AtlasBuilder& atlas) {
        this->blockShader = &blockShader;
        this->atlas = &atlas;

        Log::Info("ChunkRenderer initialized with block shader and atlas");
        return true;
    }

    void ChunkRenderer::Render(const std::vector<ChunkMesh*>& meshes,
                              const std::vector<glm::vec3>& chunkPositions,
                              const Camera& camera) {
        if (!blockShader || !atlas) {
            Log::Warning("ChunkRenderer not properly initialized");
            return;
        }

        if (meshes.size() != chunkPositions.size()) {
            Log::Error("Mesh count (%zu) doesn't match position count (%zu)",
                      meshes.size(), chunkPositions.size());
            return;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        // Reset statistics
        stats.Reset();

        // Filter out empty meshes and those beyond render distance
        std::vector<ChunkMesh*> validMeshes;
        std::vector<glm::vec3> validPositions;

        for (size_t i = 0; i < meshes.size(); ++i) {
            if (meshes[i] && !meshes[i]->IsEmpty()) {
                float distance = glm::length(chunkPositions[i] - camera.position);
                if (distance <= config.maxRenderDistance) {
                    validMeshes.push_back(meshes[i]);
                    validPositions.push_back(chunkPositions[i]);
                }
            }
        }

        if (validMeshes.empty()) {
            return;
        }

        // Save OpenGL state
        GLState savedState;
        SaveGLState(savedState);

        // Set up rendering state
        SetupRenderState();

        // Bind shader and set uniforms
        blockShader->Use();
        SetupShaderUniforms(camera);
        BindTextures();

        // Render opaque pass (front-to-back for early Z rejection)
        RenderOpaquePass(validMeshes);

        // Render cutout pass (alpha testing)
        RenderCutoutPass(validMeshes);

        // Render translucent pass (back-to-front for correct blending)
        if (config.sortTransparentMeshes) {
            std::vector<ChunkRenderData> sortedMeshes = SortMeshesByDistance(
                validMeshes, validPositions, camera.position);
            RenderTranslucentPass(sortedMeshes);
        } else {
            // Render without sorting (faster but less correct)
            std::vector<ChunkRenderData> unsortedMeshes;
            for (size_t i = 0; i < validMeshes.size(); ++i) {
                unsortedMeshes.emplace_back(validMeshes[i], validPositions[i], 0.0f);
            }
            RenderTranslucentPass(unsortedMeshes);
        }

        // Restore OpenGL state
        RestoreGLState(savedState);

        // Calculate timing
        auto endTime = std::chrono::high_resolution_clock::now();
        stats.renderTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        stats.chunksRendered = validMeshes.size();

        Log::Debug("Rendered %d chunks in %.2fms", stats.chunksRendered, stats.renderTimeMs);
    }

    void ChunkRenderer::Render(const std::vector<std::pair<ChunkMesh*, glm::ivec2>>& meshesWithPositions,
                              const Camera& camera) {
        // Convert chunk coordinates to world positions
        std::vector<ChunkMesh*> meshes;
        std::vector<glm::vec3> positions;

        for (const auto& [mesh, chunkCoords] : meshesWithPositions) {
            meshes.push_back(mesh);
            positions.push_back(CalculateChunkCenter(chunkCoords));
        }

        Render(meshes, positions, camera);
    }

    void ChunkRenderer::SetupRenderState() {
        if (config.enableDepthTesting) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        }

        if (config.enableFaceCulling) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
        }

        // Blending will be enabled/disabled per pass
        if (config.enableBlending) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    }

    void ChunkRenderer::RenderOpaquePass(const std::vector<ChunkMesh*>& meshes) {
        // Opaque pass: depth writes enabled, no blending
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        for (ChunkMesh* mesh : meshes) {
            if (!mesh->opaque.IsEmpty()) {
                mesh->opaque.Draw();
                stats.opaqueMeshes++;
                stats.totalIndices += mesh->opaque.indexCount;
            }
        }
    }

    void ChunkRenderer::RenderCutoutPass(const std::vector<ChunkMesh*>& meshes) {
        // Cutout pass: depth writes enabled, alpha testing in shader
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        // Note: Alpha testing is handled in the fragment shader
        // The shader should discard pixels with alpha < threshold

        for (ChunkMesh* mesh : meshes) {
            if (!mesh->cutout.IsEmpty()) {
                mesh->cutout.Draw();
                stats.cutoutMeshes++;
                stats.totalIndices += mesh->cutout.indexCount;
            }
        }
    }

    void ChunkRenderer::RenderTranslucentPass(const std::vector<ChunkRenderData>& sortedMeshes) {
        // Translucent pass: depth writes disabled, blending enabled
        glDepthMask(GL_FALSE);

        if (config.enableBlending) {
            glEnable(GL_BLEND);
        }

        // Render back-to-front for correct alpha blending
        for (auto it = sortedMeshes.rbegin(); it != sortedMeshes.rend(); ++it) {
            if (!it->mesh->translucent.IsEmpty()) {
                it->mesh->translucent.Draw();
                stats.translucentMeshes++;
                stats.totalIndices += it->mesh->translucent.indexCount;
            }
        }

        // Re-enable depth writes
        glDepthMask(GL_TRUE);
    }

    void ChunkRenderer::SetupShaderUniforms(const Camera& camera) {
        // Calculate view and projection matrices
        glm::mat4 view = camera.GetViewMatrix();
        glm::mat4 projection = glm::perspective(
            glm::radians(camera.fov),
            1.0f, // Aspect ratio should be set by caller
            0.01f, 500.0f
        );
        glm::mat4 mvp = projection * view;

        // Set MVP matrix uniform
        blockShader->SetMat4("uMVP", mvp);

        // Set other uniforms that might be used
        GLint texLoc = blockShader->GetUniformLocation("uTextureAtlas");
        if (texLoc != -1) {
            glUniform1i(texLoc, 0); // Texture unit 0
        }

        // Biome parameters (placeholder values)
        GLint tempLoc = blockShader->GetUniformLocation("uBiomeTemperature");
        if (tempLoc != -1) {
            glUniform1f(tempLoc, 0.8f);
        }

        GLint humidityLoc = blockShader->GetUniformLocation("uBiomeHumidity");
        if (humidityLoc != -1) {
            glUniform1f(humidityLoc, 0.4f);
        }

        GLint tintingLoc = blockShader->GetUniformLocation("uEnableBiomeTinting");
        if (tintingLoc != -1) {
            glUniform1i(tintingLoc, 1);
        }
    }

    void ChunkRenderer::BindTextures() {
        // Bind main texture atlas
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlas->GetAtlasTextureID());

        // Bind grass colormap if available
        GLuint grassColormap = atlas->GetGrassColormapID();
        if (grassColormap != 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, grassColormap);

            GLint grassLoc = blockShader->GetUniformLocation("uGrassColormap");
            if (grassLoc != -1) {
                glUniform1i(grassLoc, 1);
            }
        }

        // Bind foliage colormap if available
        GLuint foliageColormap = atlas->GetFoliageColormapID();
        if (foliageColormap != 0) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, foliageColormap);

            GLint foliageLoc = blockShader->GetUniformLocation("uFoliageColormap");
            if (foliageLoc != -1) {
                glUniform1i(foliageLoc, 2);
            }
        }

        // Reset to texture unit 0
        glActiveTexture(GL_TEXTURE0);
    }

    std::vector<ChunkRenderData> ChunkRenderer::SortMeshesByDistance(
        const std::vector<ChunkMesh*>& meshes,
        const std::vector<glm::vec3>& chunkPositions,
        const glm::vec3& cameraPos) {

        std::vector<ChunkRenderData> renderData;
        renderData.reserve(meshes.size());

        // Calculate distances and create render data
        for (size_t i = 0; i < meshes.size(); ++i) {
            float distance = glm::length(chunkPositions[i] - cameraPos);
            renderData.emplace_back(meshes[i], chunkPositions[i], distance);
        }

        // Sort by distance (front-to-back)
        std::sort(renderData.begin(), renderData.end(),
                 [](const ChunkRenderData& a, const ChunkRenderData& b) {
                     return a.distanceToCamera < b.distanceToCamera;
                 });

        return renderData;
    }

    glm::vec3 ChunkRenderer::CalculateChunkCenter(const glm::ivec2& chunkCoords) {
        return glm::vec3(
            chunkCoords.x * Game::Math::CHUNK_SIZE_X + Game::Math::CHUNK_SIZE_X * 0.5f,
            0.0f, // Y will be calculated differently based on chunk height
            chunkCoords.y * Game::Math::CHUNK_SIZE_Z + Game::Math::CHUNK_SIZE_Z * 0.5f
        );
    }

    void ChunkRenderer::SaveGLState(GLState& state) {
        state.depthTest = glIsEnabled(GL_DEPTH_TEST);
        state.blend = glIsEnabled(GL_BLEND);
        state.cullFace = glIsEnabled(GL_CULL_FACE);

        glGetBooleanv(GL_DEPTH_WRITEMASK, &state.depthMask);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &state.blendSrc);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &state.blendDst);
        glGetIntegerv(GL_FRONT_FACE, &state.frontFace);
        glGetIntegerv(GL_CULL_FACE_MODE, &state.cullFaceMode);
        glGetIntegerv(GL_CURRENT_PROGRAM, &state.currentProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &state.activeTexture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.boundTexture2D);
    }

    void ChunkRenderer::RestoreGLState(const GLState& state) {
        if (state.depthTest) {
            glEnable(GL_DEPTH_TEST);
        } else {
            glDisable(GL_DEPTH_TEST);
        }

        if (state.blend) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }

        if (state.cullFace) {
            glEnable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
        }

        glDepthMask(state.depthMask);
        glBlendFunc(state.blendSrc, state.blendDst);
        glFrontFace(state.frontFace);
        glCullFace(state.cullFaceMode);
        glUseProgram(state.currentProgram);
        glActiveTexture(state.activeTexture);
        glBindTexture(GL_TEXTURE_2D, state.boundTexture2D);
    }

} // namespace Render