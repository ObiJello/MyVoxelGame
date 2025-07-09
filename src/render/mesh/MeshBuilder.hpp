// File: src/render/mesh/MeshBuilder.hpp
#pragma once

#include "../Vertex.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include "../../engine/world/World.hpp"
#include "../../engine/block/BlockRegistry.hpp"
#include "../../engine/block/BlockModel.hpp"
#include "../../game/WorldMath.hpp"
#include <vector>
#include <cstdint>
#include <memory>

namespace Render {

    // Forward declarations
    class FluidMeshBuilder;

    // Layer-specific buffers for the three render passes
    struct LayerBuffers {
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;

        void Clear() {
            verts.clear();
            indices.clear();
        }

        size_t GetVertexCount() const { return verts.size(); }
        size_t GetIndexCount() const { return indices.size(); }

        bool IsEmpty() const { return verts.empty(); }
    };

    // Complete mesh data for a chunk, separated into rendering layers
    struct ChunkMeshData {
        LayerBuffers opaque;      // Solid blocks (depth write, no blend)
        LayerBuffers cutout;      // Alpha-test blocks (leaves, grass)
        LayerBuffers translucent; // Blended blocks (glass, water, ice)

        void Clear() {
            opaque.Clear();
            cutout.Clear();
            translucent.Clear();
        }

        size_t GetTotalVertices() const {
            return opaque.GetVertexCount() + cutout.GetVertexCount() + translucent.GetVertexCount();
        }

        size_t GetTotalIndices() const {
            return opaque.GetIndexCount() + cutout.GetIndexCount() + translucent.GetIndexCount();
        }

        bool IsEmpty() const {
            return opaque.IsEmpty() && cutout.IsEmpty() && translucent.IsEmpty();
        }
    };

    class MeshBuilder {
    public:
        MeshBuilder(Game::World& world, AtlasBuilder& atlas);
        ~MeshBuilder() = default;

        // Main entry point: build mesh data for a chunk
        ChunkMeshData Build(int chunkX, int chunkZ);

        // Get statistics from last build operation
        struct BuildStats {
            int blocksProcessed = 0;
            int facesGenerated = 0;
            int verticesGenerated = 0;
            int indicesGenerated = 0;
            float buildTimeMs = 0.0f;

            // Per-layer breakdown
            int opaqueVertices = 0;
            int cutoutVertices = 0;
            int translucentVertices = 0;
        };

        const BuildStats& GetLastBuildStats() const { return lastStats; }

    private:
        Game::World& world;
        AtlasBuilder& atlas;
        BuildStats lastStats;

        // Fluid mesh builder for specialized water/lava rendering
        std::unique_ptr<FluidMeshBuilder> fluidBuilder;

        // Core mesh building methods
        void ProcessChunk(int chunkX, int chunkZ, ChunkMeshData& meshData);
        void ProcessBlock(int worldX, int worldY, int worldZ, Game::BlockID blockId, ChunkMeshData& meshData);

        // Block classification for render layers
        LayerBuffers& GetLayerForBlock(Game::BlockID blockId, ChunkMeshData& meshData);
        bool IsBlockOpaque(Game::BlockID blockId);
        bool IsBlockCutout(Game::BlockID blockId);
        bool IsBlockTranslucent(Game::BlockID blockId);
        bool IsBlockFluid(Game::BlockID blockId);

        // Face culling and visibility
        bool ShouldRenderFace(int x, int y, int z, Game::FaceDir faceDir, Game::BlockID currentBlock);
        Game::BlockID GetNeighborBlock(int x, int y, int z, Game::FaceDir faceDir);
        glm::ivec3 GetFaceOffset(Game::FaceDir faceDir);

        // Vertex generation from block models
        void GenerateBlockFaces(int worldX, int worldY, int worldZ,
                               const Game::BlockModel& model,
                               LayerBuffers& targetLayer);

        void GenerateFace(const Game::Element& element, Game::FaceDir faceDir,
                         const Game::FaceDef& faceDef, int worldX, int worldY, int worldZ,
                         LayerBuffers& targetLayer);

        // UV mapping and texture atlas integration
        bool GetTextureUV(const std::string& texturePath, AtlasUVRect& uvRect);
        glm::vec2 InterpolateUV(const glm::vec2& uvMin, const glm::vec2& uvMax, const glm::vec2& localUV);

        // Face normal calculation
        glm::vec3 GetFaceNormal(Game::FaceDir faceDir);

        // Vertex generation helpers
        void AddQuadVertices(const std::vector<glm::vec3>& positions,
                           const glm::vec3& normal,
                           const std::vector<glm::vec2>& uvs,
                           const glm::vec4& color,
                           LayerBuffers& targetLayer);

        // Generate the 4 corner positions for a face
        std::vector<glm::vec3> GenerateFacePositions(const Game::Element& element,
                                                    Game::FaceDir faceDir,
                                                    int worldX, int worldY, int worldZ);

        // Generate UV coordinates for a face
        std::vector<glm::vec2> GenerateFaceUVs(const Game::FaceDef& faceDef,
                                              const AtlasUVRect& atlasUV);

        // Biome tinting support
        glm::vec4 CalculateBiomeTint(int worldX, int worldY, int worldZ,
                                   const Game::FaceDef& faceDef,
                                   Game::BlockID blockId);

        // Default white color for non-tinted blocks
        static constexpr glm::vec4 DEFAULT_COLOR = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);

        // Face direction to offset mapping
        static const std::array<glm::ivec3, 6> FACE_OFFSETS;
        static const std::array<glm::vec3, 6> FACE_NORMALS;
    };

} // namespace Render