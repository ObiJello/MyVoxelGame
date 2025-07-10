// File: src/render/mesh/ChunkRenderer.hpp
#pragma once

#include "ChunkMeshData.hpp"
#include "../gfx/Camera.hpp"
#include "../gfx/Shader.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include <vector>
#include <memory>

namespace Render {

    // Render statistics for performance monitoring
    struct RenderStats {
        int chunksRendered = 0;
        int opaqueMeshes = 0;
        int cutoutMeshes = 0;
        int translucentMeshes = 0;
        size_t totalVertices = 0;
        size_t totalIndices = 0;
        float renderTimeMs = 0.0f;

        void Reset() {
            chunksRendered = 0;
            opaqueMeshes = 0;
            cutoutMeshes = 0;
            translucentMeshes = 0;
            totalVertices = 0;
            totalIndices = 0;
            renderTimeMs = 0.0f;
        }
    };

    // Chunk data for distance-based sorting
    struct ChunkRenderData {
        ChunkMesh* mesh;
        glm::vec3 chunkCenter;
        float distanceToCamera;

        ChunkRenderData(ChunkMesh* m, const glm::vec3& center, float distance)
            : mesh(m), chunkCenter(center), distanceToCamera(distance) {}
    };

    class ChunkRenderer {
    public:
        ChunkRenderer();
        ~ChunkRenderer() = default;

        // Initialize the renderer (call once)
        bool Initialize(Shader& blockShader, AtlasBuilder& atlas);

        // Main render function - renders all chunks in the correct order
        void Render(const std::vector<ChunkMesh*>& meshes,
                   const std::vector<glm::vec3>& chunkPositions,
                   const Camera& camera,
                   const glm::mat4& viewProjectionMatrix);

        // Alternative render function with chunk positions inferred
        void Render(const std::vector<std::pair<ChunkMesh*, glm::ivec2>>& meshesWithPositions,
                   const Camera& camera,
                    const glm::mat4& viewProjectionMatrix);

        // Get rendering statistics from last frame
        const RenderStats& GetStats() const { return stats; }

        // Configuration
        struct Config {
            bool enableDepthTesting = true;
            bool enableFaceCulling = true;
            bool enableBlending = true;
            bool sortTransparentMeshes = true;
            float maxRenderDistance = 256.0f; // Maximum render distance in blocks
        };

        void SetConfig(const Config& config) { this->config = config; }
        const Config& GetConfig() const { return config; }

    private:
        Shader* blockShader = nullptr;
        AtlasBuilder* atlas = nullptr;
        Config config;
        RenderStats stats;

        // Render pass management
        void SetupRenderState();
        void RestoreRenderState();

        // Individual render passes
        void RenderOpaquePass(const std::vector<ChunkMesh*>& meshes);
        void RenderCutoutPass(const std::vector<ChunkMesh*>& meshes);
        void RenderTranslucentPass(const std::vector<ChunkRenderData>& sortedMeshes);

        // Shader uniform setup
        void SetupShaderUniforms(const Camera& camera, const glm::mat4& viewProjectionMatrix);
        void BindTextures();

        // Distance-based sorting for transparency
        std::vector<ChunkRenderData> SortMeshesByDistance(
            const std::vector<ChunkMesh*>& meshes,
            const std::vector<glm::vec3>& chunkPositions,
            const glm::vec3& cameraPos);

        // Helper to calculate chunk center from chunk coordinates
        glm::vec3 CalculateChunkCenter(const glm::ivec2& chunkCoords);

        // OpenGL state management
        struct GLState {
            GLboolean depthTest;
            GLboolean depthMask;
            GLboolean blend;
            GLboolean cullFace;
            GLint blendSrc, blendDst;
            GLint frontFace;
            GLint cullFaceMode;
            GLint currentProgram;
            GLint activeTexture;
            GLint boundTexture2D;
        };

        void SaveGLState(GLState& state);
        void RestoreGLState(const GLState& state);
    };

    // Global chunk renderer instance (optional)
    extern std::unique_ptr<ChunkRenderer> g_chunkRenderer;

} // namespace Render