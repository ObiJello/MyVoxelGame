// File: src/render/mesh/FluidMeshBuilder.hpp
#pragma once

#include "SectionMesh.hpp"
#include "../../engine/world/Chunk.hpp"
#include "../../engine/block/Blocks.hpp"
#include <glm/glm.hpp>

namespace Render {

    // Block face enum for fluid builder (matching Mesher.hpp)
    enum class BlockFace : int;

    // Fluid meshing configuration
    struct FluidMeshConfig {
        bool enableFluidSlopes = true;        // Create sloped fluid surfaces
        bool enableFluidCulling = true;       // Cull internal fluid faces
        float fluidHeight = 0.9f;            // Height of fluid surface (0.0-1.0)
        float flowAnimationSpeed = 1.0f;     // Speed of flow texture animation

        // Visual settings
        glm::vec4 waterTint{0.3f, 0.5f, 1.0f, 0.8f};  // Blue-ish water
        glm::vec4 lavaTint{1.0f, 0.4f, 0.1f, 1.0f};   // Orange-red lava

        bool enableFluidTransparency = true;
        float fluidAlpha = 0.8f;
    };

    // Fluid flow direction for surface sloping
    enum class FlowDirection {
        None,
        North, South, East, West,
        NorthEast, NorthWest, SouthEast, SouthWest
    };

    // Specialized mesh builder for fluid blocks (water, lava)
    class FluidMeshBuilder {
    public:
        explicit FluidMeshBuilder(const FluidMeshConfig& config = FluidMeshConfig{});

        // Build fluid geometry for one block
        void BuildFluidBlock(const Game::Chunk& chunk, int localX, int localY, int localZ,
                           int sectionY, SectionMesh& outMesh);

        // Build fluid geometry for entire section (called from Mesher)
        void BuildFluidSection(const Game::Chunk& chunk, int sectionY, SectionMesh& outMesh);

        // Update configuration
        void SetConfig(const FluidMeshConfig& config) { m_config = config; }
        const FluidMeshConfig& GetConfig() const { return m_config; }

    private:
        FluidMeshConfig m_config;

        // Core fluid meshing functions
        void AddFluidFace(Game::BlockID fluidType, glm::vec3 blockPos, BlockFace face,
                         float height, const glm::vec4& tint, SectionMesh& mesh);

        void CreateFluidTopSurface(Game::BlockID fluidType, glm::vec3 blockPos,
                                  float height, FlowDirection flow,
                                  const glm::vec4& tint, SectionMesh& mesh);

        void CreateFluidSideFace(Game::BlockID fluidType, glm::vec3 blockPos, BlockFace face,
                               float height, const glm::vec4& tint, SectionMesh& mesh);

        // Flow detection and surface calculation
        FlowDirection DetectFlowDirection(const Game::Chunk& chunk, int x, int y, int z);
        float CalculateFluidHeight(const Game::Chunk& chunk, int x, int y, int z, Game::BlockID fluidType);
        bool IsFluidFaceExposed(const Game::Chunk& chunk, int x, int y, int z, BlockFace face);

        // Helper functions
        bool IsFluid(Game::BlockID blockId) const;
        bool IsSameFluid(Game::BlockID a, Game::BlockID b) const;
        std::string GetFluidTexture(Game::BlockID fluidType, bool isFlowing = false) const;
        glm::vec4 GetFluidTint(Game::BlockID fluidType) const;

        // Geometry creation
        std::vector<Vertex> CreateSlopedSurface(glm::vec3 blockPos, float height,
                                               FlowDirection flow, const glm::vec4& uvRect,
                                               const glm::vec4& tint);

        std::vector<Vertex> CreateFluidQuad(glm::vec3 blockPos, BlockFace face, float height,
                                           const glm::vec4& uvRect, const glm::vec4& tint);

        // Texture and UV helpers
        bool GetFluidTextureUV(const std::string& texturePath, glm::vec4& uvRect);
        glm::vec2 GetFlowTextureOffset(Game::BlockID fluidType) const;
    };

} // namespace Render