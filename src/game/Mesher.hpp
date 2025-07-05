// File: src/game/Mesher.hpp (FIXED - Updated Function Signatures)
#pragma once

#include "ChunkSection.hpp"
#include "Chunk.hpp"
#include "WorldMath.hpp"
#include "BlockModel.hpp"
#include "EnhancedBlockRegistry.hpp"
#include "../render/Vertex.hpp"
#include "../render/AtlasBuilder.hpp"
#include <vector>
#include <memory>
#include <array>

namespace Game {

    // Forward declaration for neighbor context
    struct NeighborContext;

    using MeshUploadCallback = std::function<void(MeshData*)>;
    void SetMeshUploadCallback(MeshUploadCallback callback);

    // Data structure for a meshed chunk section
    struct MeshData {
        std::vector<Render::Vertex> vertices;
        std::vector<uint32_t> indices;
        Math::ChunkPos chunkXZ;
        int sectionIndex;

        MeshData() = default;
    };

    // Neighbor context for inter-chunk meshing
    struct NeighborContext {
        std::shared_ptr<Chunk> center;
        std::array<std::shared_ptr<Chunk>, 4> neighbors; // West, East, North, South
        bool hasAllNeighbors;

        NeighborContext(std::shared_ptr<Chunk> centerChunk)
            : center(centerChunk), hasAllNeighbors(false) {
            neighbors.fill(nullptr);
        }
    };

    // Face direction enumeration for culling
    enum class FaceDirection {
        PosX = 0, // East  (+X)
        NegX = 1, // West  (-X)
        PosY = 2, // Up    (+Y)
        NegY = 3, // Down  (-Y)
        PosZ = 4, // South (+Z)
        NegZ = 5  // North (-Z)
    };

    // Mesher class for converting chunks to renderable meshes
    class Mesher {
    public:
        // Main meshing function - processes a single 16x16x16 section
        static void MeshSection(ChunkSection* section, MeshData* meshData, Chunk* parentChunk);

        // Enhanced meshing with inter-chunk context for better face culling
        static void MeshSectionWithNeighbors(ChunkSection* section, MeshData* meshData,
                                           const NeighborContext& context);

        // Legacy entry point for backward compatibility
        static void MesherJob(ChunkSection* section, MeshData* meshData, Chunk* parentChunk);

        // Enhanced entry point for inter-chunk meshing
        static void InterChunkMesherJob(ChunkSection* section, MeshData* meshData,
                                      const NeighborContext& context);

    private:
        // Core meshing implementation
        static void MeshSectionInternal(ChunkSection* section, MeshData* meshData,
                                      Math::ChunkPos chunkPos, int sectionIndex,
                                      const NeighborContext* neighborContext = nullptr);

        // Block access with neighbor support
        static BlockID GetBlockWithNeighbors(const NeighborContext& context,
                                           int localX, int worldY, int localZ);

        // Standard block access without neighbors
        static BlockID GetBlockStandard(Chunk* chunk, int localX, int worldY, int localZ);

        // Face culling check
        static bool ShouldCullFace(BlockID currentBlock, BlockID neighborBlock);

        // FIXED: Get face offset for face direction
        static void GetFaceOffset(FaceDirection faceDir, int& dx, int& dy, int& dz);

        // Convert model face direction to mesher face direction
        static FaceDirection ModelFaceToMesherFace(FaceDir modelFace);

        // FIXED: Mesh a single element from a block model (now includes currentBlockId)
        static void MeshElement(const Element& element, const BlockModel& model,
                              const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                              BlockID currentBlockId, MeshData* meshData, bool enableBiomeTinting,
                              const NeighborContext* neighborContext = nullptr,
                              Chunk* chunk = nullptr);

        // FIXED: Mesh a single face of an element (now includes currentBlockId)
        static void MeshFace(const Element& element, const FaceDef& faceDef,
                           FaceDir faceDir, const BlockModel& model,
                           const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                           BlockID currentBlockId, MeshData* meshData, bool enableBiomeTinting);

        // Get face normal vector
        static glm::vec3 GetFaceNormal(FaceDir faceDir);

        // Get face vertices in model space (0-16 range)
        static std::array<glm::vec3, 4> GetFaceVertices(const Element& element, FaceDir faceDir);

        // FIXED: Get UV coordinates for face corners (now includes texture path for dimensions)
        static std::array<glm::vec2, 4> GetFaceUVs(const FaceDef& faceDef, const std::string& texturePath);

        // Convert model space to world space
        static glm::vec3 ModelToWorldSpace(const glm::vec3& modelPos,
                                         const glm::ivec3& blockPos,
                                         const glm::ivec3& worldBlockPos);

        // Sample biome tinting color (placeholder for now)
        static glm::vec3 SampleBiomeTinting(int tintIndex, const glm::ivec3& worldPos);

        // Get atlas UVs for a texture
        static bool GetAtlasUVs(const std::string& texturePath,
                              const glm::vec2& modelUV, glm::vec2& atlasUV);
    };

    // Convenience functions for external use
    void MesherJob(ChunkSection* section, MeshData* meshData, Chunk* parentChunk);
    void InterChunkMesherJob(ChunkSection* section, MeshData* meshData,
                           const NeighborContext& context);

} // namespace Game