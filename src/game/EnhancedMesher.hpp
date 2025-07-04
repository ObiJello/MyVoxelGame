// File: src/game/EnhancedMesher.hpp (NEW FILE - create this)
#pragma once

#include "Mesher.hpp"
#include "EnhancedBlockRegistry.hpp"
#include "../render/Vertex.hpp"

namespace Game {
    // Enhanced meshing integration functions
    void EnhancedInterChunkMesherJob(ChunkSection* section, MeshData* meshData, const NeighborContext& ctx);
    void LegacyMeshBlock(const EnhancedBlock& block, int x, int y, int z, int sectionWorldYOffset,
                        const NeighborContext& ctx, std::vector<Render::Vertex>& vertices,
                        std::vector<uint32_t>& indices, int& facesGenerated, int& facesCulled);
    BlockID GetBlockFromNeighborContext(const NeighborContext& ctx, int chunkLocalX, int worldY, int chunkLocalZ);
    void GenerateUVsForFaceLegacy(int face, const Block& block, glm::vec2 uvs[4]);
    bool IsBlockOpaque(BlockID blockId);
}