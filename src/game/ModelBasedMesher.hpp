// File: src/game/ModelBasedMesher.hpp
#pragma once

#include "Mesher.hpp"
#include "BlockModel.hpp"
#include "../render/AtlasBuilder.hpp"
#include "../render/Vertex.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace Game {

    struct ModelMeshContext {
        const NeighborContext* neighborCtx = nullptr;
        const Render::AtlasBuilder* atlasBuilder = nullptr;
        
        // Biome data for tinting (expandable for future biome system)
        int biomeTemperature = 128;  // 0-255 range, 128 = temperate
        int biomeHumidity = 128;     // 0-255 range, 128 = moderate
        
        // Performance tracking
        int* facesGenerated = nullptr;
        int* facesCulled = nullptr;
    };

    class ModelBasedMesher {
    public:
        // Enhanced mesher that uses block models and AtlasBuilder
        static void MeshBlockWithModel(
            const BlockModel& model,
            const glm::ivec3& blockPos,
            const ModelMeshContext& context,
            std::vector<Render::Vertex>& vertices,
            std::vector<uint32_t>& indices
        );
        
        // Get UV coordinates from AtlasBuilder for a texture reference
        static bool GetModelTextureUV(
            const std::string& textureRef,
            const BlockModel& model,
            const Render::AtlasBuilder& atlas,
            Render::AtlasUVRect& uvRect
        );
        
        // Calculate biome tint color for a face
        static glm::vec3 CalculateBiomeTint(
            int tintIndex,
            int temperature,
            int humidity,
            const Render::AtlasBuilder& atlas
        );
        
        // Check if a face should be culled based on neighbor context
        static bool ShouldCullFace(
            const glm::ivec3& blockPos,
            FaceDir faceDir,
            const std::string& cullface,
            const ModelMeshContext& context
        );
        
    private:
        // Convert BlockModel face direction to normal vector
        static glm::vec3 GetFaceNormal(FaceDir dir);
        
        // Generate quad vertices and UVs for a specific face
        static void GenerateQuadForFace(
            FaceDir faceDir,
            const glm::vec3& elementMin,
            const glm::vec3& elementMax,
            const glm::vec4& faceUV,
            const Render::AtlasUVRect& atlasUV,
            std::array<glm::vec3, 4>& quadVertices,
            std::array<glm::vec2, 4>& quadUVs
        );
        
        // Get block at specific position using neighbor context
        static BlockID GetBlockAtPosition(
            const glm::ivec3& pos,
            const ModelMeshContext& context
        );
        
        // Check if a block is opaque (for culling decisions)
        static bool IsBlockOpaque(BlockID blockId);
        
        // Direction offsets for face checking
        static const glm::ivec3 FACE_OFFSETS[6];
    };

} // namespace Game