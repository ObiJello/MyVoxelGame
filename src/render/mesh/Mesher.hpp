// File: src/render/mesh/Mesher.hpp (Enhanced with Layered Meshing)
#pragma once

#include "../../engine/world/ChunkSection.hpp"
#include "../../engine/world/Chunk.hpp"
#include "../../game/WorldMath.hpp"
#include "../../engine/block/BlockModel.hpp"
#include "../../engine/block/BlockRegistry.hpp"
#include "../Vertex.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include <vector>
#include <memory>
#include <array>
#include <functional>

namespace Game {

    // Forward declaration for neighbor context
    struct NeighborContext;

    // ENHANCED: Layered mesh data structure for three render passes
    struct LayeredMeshData {
        // Opaque layer (solid blocks)
        std::vector<Render::Vertex> opaqueVertices;
        std::vector<uint32_t> opaqueIndices;

        // Cutout layer (alpha-test blocks like leaves)
        std::vector<Render::Vertex> cutoutVertices;
        std::vector<uint32_t> cutoutIndices;

        // Translucent layer (fluids, glass, etc.)
        std::vector<Render::Vertex> translucentVertices;
        std::vector<uint32_t> translucentIndices;

        // Metadata
        Math::ChunkPos chunkXZ;
        int sectionIndex;

        LayeredMeshData() = default;

        // Check if any layer has geometry
        bool HasAnyGeometry() const {
            return !opaqueVertices.empty() || !cutoutVertices.empty() || !translucentVertices.empty();
        }

        // Get total vertex count across all layers
        size_t GetTotalVertexCount() const {
            return opaqueVertices.size() + cutoutVertices.size() + translucentVertices.size();
        }
    };

    // Legacy mesh data for backward compatibility
    struct MeshData {
        std::vector<Render::Vertex> vertices;
        std::vector<uint32_t> indices;
        Math::ChunkPos chunkXZ;
        int sectionIndex;

        MeshData() = default;
    };

    // Callback types
    using MeshUploadCallback = std::function<void(MeshData*)>;
    using LayeredMeshUploadCallback = std::function<void(LayeredMeshData*)>;

    void SetMeshUploadCallback(MeshUploadCallback callback);
    void SetLayeredMeshUploadCallback(LayeredMeshUploadCallback callback);

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

    // ENHANCED: Face render type classification
    enum class FaceRenderType {
        Opaque = 0,      // Solid blocks
        Cutout = 1,      // Alpha-test (leaves, glass)
        Translucent = 2  // Blended (fluids, stained glass)
    };

    // ENHANCED: Fluid level representation
    struct FluidLevel {
        float height = 0.0f;  // 0.0 to 1.0
        BlockID fluidType = BlockID::Air;
        bool isFluid = false;

        FluidLevel() = default;
        FluidLevel(float h, BlockID type) : height(h), fluidType(type), isFluid(true) {}
    };

    // Enhanced Mesher class with fluid support
    class Mesher {
    public:
        // ENHANCED: Layered meshing functions
        static void MeshSectionLayered(ChunkSection* section, LayeredMeshData* meshData,
                                     const NeighborContext& context);

        // Legacy functions for backward compatibility
        static void MeshSection(ChunkSection* section, MeshData* meshData, Chunk* parentChunk);
        static void MeshSectionWithNeighbors(ChunkSection* section, MeshData* meshData,
                                           const NeighborContext& context);

        // ADDED: Inter-chunk meshing job as static member
        static void InterChunkMesherJob(ChunkSection* section, MeshData* meshData,
                                      const NeighborContext& context);

    private:
        // ENHANCED: Internal layered meshing implementation
        static void MeshSectionLayeredInternal(ChunkSection* section, LayeredMeshData* meshData,
                                             Math::ChunkPos chunkPos, int sectionIndex,
                                             const NeighborContext& context);

        // ENHANCED: Block meshing with layer classification
        static void MeshBlockLayered(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                                   BlockID blockId, LayeredMeshData* meshData,
                                   const NeighborContext& context);

        // ENHANCED: Separate solid and fluid block meshing
        static void MeshSolidBlock(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                                 BlockID blockId, LayeredMeshData* meshData,
                                 const NeighborContext& context);

        static void MeshFluidBlock(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                                 BlockID fluidType, LayeredMeshData* meshData,
                                 const NeighborContext& context);

        // ENHANCED: Fluid-specific functions
        static bool IsFluidBlock(BlockID blockId);
        static FluidLevel GetFluidLevel(const glm::ivec3& worldPos, const NeighborContext& context);

        static void MeshFluidTopFace(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                                   BlockID fluidType, LayeredMeshData* meshData,
                                   const std::array<FluidLevel, 4>& cornerLevels);

        static void MeshFluidSideFace(const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                                    BlockID fluidType, FaceDirection faceDir,
                                    LayeredMeshData* meshData, const NeighborContext& context);

        // ENHANCED: Face render type classification
        static FaceRenderType ClassifyFaceRenderType(const FaceDef& faceDef, BlockID blockId);

        static void GetLayerArrays(LayeredMeshData* meshData, FaceRenderType renderType,
                                 std::vector<Render::Vertex>*& vertices,
                                 std::vector<uint32_t>*& indices);

        // ENHANCED: Element meshing with layer support
        static void MeshElement(const Element& element, const BlockModel& model,
                              const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                              BlockID currentBlockId, LayeredMeshData* meshData, bool enableBiomeTinting,
                              const NeighborContext& context);

        static void MeshFace(const Element& element, const FaceDef& faceDef,
                           FaceDir faceDir, const BlockModel& model,
                           const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                           BlockID currentBlockId, LayeredMeshData* meshData, bool enableBiomeTinting);

        // Legacy functions (unchanged)
        static void MeshSectionInternal(ChunkSection* section, MeshData* meshData,
                                      Math::ChunkPos chunkPos, int sectionIndex,
                                      const NeighborContext* neighborContext = nullptr);

        static BlockID GetBlockWithNeighbors(const NeighborContext& context,
                                           int localX, int worldY, int localZ);
        static BlockID GetBlockStandard(Chunk* chunk, int localX, int worldY, int localZ);
        static bool ShouldCullFace(BlockID currentBlock, BlockID neighborBlock);
        static void GetFaceOffset(FaceDirection faceDir, int& dx, int& dy, int& dz);
        static FaceDirection ModelFaceToMesherFace(FaceDir modelFace);
        static glm::vec3 GetFaceNormal(FaceDir faceDir);
        static std::array<glm::vec3, 4> GetFaceVertices(const Element& element, FaceDir faceDir);
        static std::array<glm::vec2, 4> GetFaceUVs(const FaceDef& faceDef, const std::string& texturePath);
        static glm::vec3 ModelToWorldSpace(const glm::vec3& modelPos,
                                         const glm::ivec3& blockPos,
                                         const glm::ivec3& worldBlockPos);
        static glm::vec3 SampleGrassTinting(const glm::ivec3& worldPos);
        static glm::vec3 SampleFoliageTinting(const glm::ivec3& worldPos);
        static bool GetAtlasUVs(const std::string& texturePath,
                              const glm::vec2& modelUV, glm::vec2& atlasUV);

        // Legacy element/face meshing
        static void MeshElement(const Element& element, const BlockModel& model,
                              const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                              BlockID currentBlockId, MeshData* meshData, bool enableBiomeTinting,
                              const NeighborContext* neighborContext = nullptr,
                              Chunk* chunk = nullptr);

        static void MeshFace(const Element& element, const FaceDef& faceDef,
                           FaceDir faceDir, const BlockModel& model,
                           const glm::ivec3& blockPos, const glm::ivec3& worldBlockPos,
                           BlockID currentBlockId, MeshData* meshData, bool enableBiomeTinting);
    };

    // ENHANCED: Convenience functions for layered meshing
    void LayeredMesherJob(ChunkSection* section, LayeredMeshData* meshData,
                        const NeighborContext& context);

    // Legacy convenience functions
    void MesherJob(ChunkSection* section, MeshData* meshData, Chunk* parentChunk);
    void InterChunkMesherJob(ChunkSection* section, MeshData* meshData,
                           const NeighborContext& context);

} // namespace Game