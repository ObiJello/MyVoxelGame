// File: src/render/mesh/FluidMeshBuilder.hpp
#pragma once

#include "MeshBuilder.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include "../../engine/world/World.hpp"
#include "../../game/WorldMath.hpp"
#include <array>

namespace Render {

    // Fluid level data for corner height calculations
    struct FluidLevel {
        float height = 0.0f;  // Height from 0.0 to 1.0
        bool isFluid = false; // Whether this position contains fluid
        Game::BlockID fluidType = Game::BlockID::Air; // Type of fluid
    };

    class FluidMeshBuilder {
    public:
        FluidMeshBuilder(Game::World& world, AtlasBuilder& atlas);
        ~FluidMeshBuilder() = default;

        // Main entry point: append fluid quads to existing mesh data
        void AppendFluidQuads(int chunkX, int chunkZ, ChunkMeshData& meshData);

        // Configuration
        struct FluidConfig {
            float waterLevel = 0.875f;    // Default water level (14/16 blocks)
            float lavaLevel = 0.75f;      // Default lava level (12/16 blocks)
            bool enableFlowAnimation = true;
            bool generateSideQuads = true;
            bool generateTopQuads = true;
        };

        void SetConfig(const FluidConfig& config) { this->config = config; }
        const FluidConfig& GetConfig() const { return config; }

        // Statistics from last build
        struct FluidStats {
            int fluidsProcessed = 0;
            int topQuadsGenerated = 0;
            int sideQuadsGenerated = 0;
            int verticesGenerated = 0;
            float buildTimeMs = 0.0f;
        };

        const FluidStats& GetLastStats() const { return lastStats; }

    private:
        Game::World& world;
        AtlasBuilder& atlas;
        FluidConfig config;
        FluidStats lastStats;

        // Core fluid processing
        void ProcessChunkFluids(int chunkX, int chunkZ, ChunkMeshData& meshData);
        void ProcessFluidBlock(int worldX, int worldY, int worldZ, Game::BlockID fluidType, ChunkMeshData& meshData);

        // Fluid level sampling and calculation
        FluidLevel GetFluidLevel(int x, int y, int z, Game::BlockID expectedFluid);
        std::array<float, 4> CalculateCornerHeights(int worldX, int worldY, int worldZ, Game::BlockID fluidType);

        // Corner sampling for sloped surfaces
        float SampleCornerHeight(int worldX, int worldY, int worldZ, int cornerX, int cornerZ, Game::BlockID fluidType);

        // Quad generation
        void GenerateFluidTopQuad(int worldX, int worldY, int worldZ, Game::BlockID fluidType,
                                 const std::array<float, 4>& cornerHeights, ChunkMeshData& meshData);

        void GenerateFluidSideQuads(int worldX, int worldY, int worldZ, Game::BlockID fluidType,
                                   const std::array<float, 4>& cornerHeights, ChunkMeshData& meshData);

        void GenerateFluidSideQuad(int worldX, int worldY, int worldZ, Game::BlockID fluidType,
                                  Game::FaceDir faceDir, const std::array<float, 4>& cornerHeights,
                                  ChunkMeshData& meshData);

        // Texture handling for fluids
        bool GetFluidTexture(Game::BlockID fluidType, bool isStill, AtlasUVRect& uvRect);
        std::string GetFluidTextureName(Game::BlockID fluidType, bool isStill);

        // Fluid flow detection
        bool IsFluidFlowing(int worldX, int worldY, int worldZ, Game::BlockID fluidType);
        bool HasFluidNeighbor(int worldX, int worldY, int worldZ, Game::BlockID fluidType, Game::FaceDir direction);

        // Height calculation helpers
        float GetDefaultFluidHeight(Game::BlockID fluidType);
        bool ShouldRenderFluidSide(int worldX, int worldY, int worldZ, Game::FaceDir faceDir, Game::BlockID fluidType);

        // Vertex generation for fluid quads
        void AddFluidQuadVertices(const std::vector<glm::vec3>& positions,
                                 const glm::vec3& normal,
                                 const std::vector<glm::vec2>& uvs,
                                 const glm::vec4& color,
                                 LayerBuffers& targetLayer);

        // UV generation for fluid faces
        std::vector<glm::vec2> GenerateFluidUVs(const AtlasUVRect& atlasUV, bool isTopFace = true);

        // Fluid colors and effects
        glm::vec4 GetFluidColor(Game::BlockID fluidType);

        // Constants
        static constexpr float MIN_FLUID_HEIGHT = 0.0625f; // 1/16 block
        static constexpr float MAX_FLUID_HEIGHT = 1.0f;    // Full block

        // Corner indices for height sampling (clockwise from bottom-left)
        static constexpr int CORNER_OFFSETS[4][2] = {
            {0, 0}, // Bottom-left
            {1, 0}, // Bottom-right
            {1, 1}, // Top-right
            {0, 1}  // Top-left
        };
    };

} // namespace Render