// File: src/game/EnhancedMesher.hpp - FIXED
#pragma once

#include "Mesher.hpp"
#include "EnhancedBlockRegistry.hpp"
#include "BlockRegistry.hpp"  // ADDED: For Block struct
#include "../render/Vertex.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace Game {
    // Enhanced meshing integration functions
    void EnhancedInterChunkMesherJob(ChunkSection* section, MeshData* meshData, const NeighborContext& ctx);

    void LegacyMeshBlock(const EnhancedBlock& block, int x, int y, int z, int sectionWorldYOffset,
                        const NeighborContext& ctx, std::vector<Render::Vertex>& vertices,
                        std::vector<uint32_t>& indices, int& facesGenerated, int& facesCulled);

    // Note: GetBlockFromNeighborContext and IsBlockOpaque are declared in Mesher.hpp

    // SPECIFIC function for EnhancedBlock to avoid conflicts with Mesher.hpp
    void GenerateUVsForEnhancedBlock(int face, const EnhancedBlock& block, glm::vec2 uvs[4]);
}