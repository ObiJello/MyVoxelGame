// File: src/render/mesh/Mesher.hpp (UPDATED - Uses WorldCoordinates)
#pragma once

#include "SectionMesh.hpp"
#include "../../engine/world/Chunk.hpp"
#include "../../engine/block/Blocks.hpp"
#include "../../engine/block/BlockModel.hpp"
#include "../../game/WorldCoordinates.hpp"  // **NEW**: Use centralized coordinates
#include "../atlas/AtlasBuilder.hpp"
#include <glm/glm.hpp>

// **NEW**: Forward declaration to avoid circular dependency
namespace Game {
    class World;
}

namespace Render {

    // Face directions for block meshing
    enum class BlockFace : int {
        PositiveY = 0,  // Top (+Y)
        NegativeY = 1,  // Bottom (-Y)
        PositiveZ = 2,  // Front (+Z)
        NegativeZ = 3,  // Back (-Z)
        PositiveX = 4,  // Right (+X)
        NegativeX = 5   // Left (-X)
    };

    // Mesh generation configuration
    struct MeshConfig {
        bool enableAmbientOcclusion = true;
        bool enableFaceCulling = true;
        bool enableBiomeTinting = true;
        float biomeTintStrength = 1.0f;

        // Performance settings
        bool enableGreedyMeshing = false;  // Future optimization
        int maxQuadsPerSection = 16384;    // Safety limit
    };

    // Render layer classification
    enum class RenderLayer {
        Opaque,      // Solid blocks (stone, dirt, wood)
        Cutout,      // Alpha-test blocks (leaves, grass, flowers)
        Translucent  // Blended blocks (glass, water, ice)
    };

    // Helper functions for render layer classification
    RenderLayer ClassifyBlock(Game::BlockID blockId);
    bool IsBlockOpaque(Game::BlockID blockId);
    bool IsBlockTranslucent(Game::BlockID blockId);

    // Core meshing class - turns block data into renderable geometry
    class Mesher {
    public:
        explicit Mesher(const MeshConfig& config = MeshConfig{});

        // **NEW**: Set world reference for cross-chunk neighbor access
        void SetWorld(Game::World* world);

        // Rebuild mesh for one 16x16x16 section
        void BuildSectionMesh(const Game::Chunk& chunk, int sectionY, SectionMesh& outMesh);

        // Convenience: rebuild entire chunk (all 24 sections)
        void BuildChunkMesh(const Game::Chunk& chunk, ChunkMesh& outMesh);

        // Update configuration
        void SetConfig(const MeshConfig& config) { m_config = config; }
        const MeshConfig& GetConfig() const { return m_config; }

        // Get statistics from last mesh operation
        struct MeshStats {
            int facesGenerated = 0;
            int facesCulled = 0;
            int quadsGenerated = 0;
            float buildTimeMs = 0.0f;
        };
        const MeshStats& GetLastStats() const { return m_lastStats; }

    private:
        MeshConfig m_config;
        mutable MeshStats m_lastStats;
        Game::World* m_world;  // **NEW**: World reference for cross-chunk access

        // Core meshing functions
        void ProcessBlock(const Game::Chunk& chunk, int localX, int localY, int localZ,
                         int sectionY, SectionMesh& mesh);

        // **UPDATED**: Now includes world coordinates for biome tinting
        void AddBlockFace(const Game::BlockModel& model, const Game::Element& element,
                         Game::FaceDir faceDir, const Game::FaceDef& faceDef,
                         glm::vec3 blockPos, glm::vec3 faceNormal, Game::BlockID blockId,
                         int worldX, int worldY, int worldZ, SectionMesh& mesh);

        void GenerateQuad(const std::vector<Vertex>& quadVerts,
                         std::vector<Vertex>& outVerts, std::vector<uint32_t>& outIndices);

        // Culling and optimization
        bool ShouldCullFace(const Game::Chunk& chunk, int x, int y, int z,
                           BlockFace face, Game::BlockID currentBlock);

        // **UPDATED**: Now supports cross-chunk neighbor lookup via world reference
        Game::BlockID GetNeighborBlock(const Game::Chunk& chunk, int x, int y, int z,
                                      BlockFace face);

        // Texture and material helpers
        bool GetTextureUV(const std::string& texturePath, glm::vec4& uvRect);

        // **NEW**: Biome tinting methods for different tint indices
        glm::vec4 CalculateGrassTint(Game::BlockID blockId, int worldX, int worldY, int worldZ);
        glm::vec4 CalculateFoliageTint(Game::BlockID blockId, int worldX, int worldY, int worldZ);
        glm::vec4 CalculateBiomeTint(Game::BlockID blockId, int worldX, int worldY, int worldZ);

        RenderLayer GetRenderLayer(Game::BlockID blockId);

        // Geometry helpers
        std::vector<Vertex> CreateFaceVertices(glm::vec3 blockPos, BlockFace face,
                                              const glm::vec4& uvRect, const glm::vec4& tint);
        glm::vec3 GetFaceNormal(BlockFace face);
        uint8_t CalculateAmbientOcclusion(const Game::Chunk& chunk, int x, int y, int z,
                                         BlockFace face, int vertexIndex);

        // **REMOVED**: WorldYToChunkY() - use Game::Math::WorldCoordinates instead

        // **UPDATED**: Use WorldCoordinates for coordinate conversion
        glm::vec3 LocalToWorldPos(const Game::Math::ChunkPos& chunkPos, int localX, int localY, int localZ) const {
            int worldX, worldY, worldZ;
            Game::Math::WorldCoordinates::LocalToWorld(chunkPos, localX, localY, localZ, worldX, worldY, worldZ);
            return glm::vec3(static_cast<float>(worldX), static_cast<float>(worldY), static_cast<float>(worldZ));
        }
    };

} // namespace Render