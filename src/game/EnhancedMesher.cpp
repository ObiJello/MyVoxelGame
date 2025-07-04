// File: src/game/EnhancedMesher.cpp - FIXED
// This file provides the integration between the new model-based meshing and existing system
#include "Mesher.hpp"
#include "ModelBasedMesher.hpp"
#include "EnhancedBlockRegistry.hpp"
#include "BlockRegistry.hpp"  // ADDED: For Block struct
#include "Chunk.hpp"  // ADDED: For Chunk class definition
#include "../render/AtlasBuilder.hpp"
#include "../render/TextureAtlas.hpp"  // ADDED: For g_textureAtlas
#include "../core/Log.hpp"
#include "../core/Config.hpp"
#include <glm/glm.hpp>
#include <mutex>
#include <queue>

namespace Game {

    // Helper function for legacy UV generation - SPECIFIC for EnhancedBlock
    void GenerateUVsForEnhancedBlock(int face, const EnhancedBlock& block, glm::vec2 uvs[4]) {
        // Get the atlas index for this face
        uint16_t atlasIndex = block.legacyTexIdx[face];

        // Get the UV tile from the legacy texture atlas
        Render::AtlasTile tile = Render::g_textureAtlas.GetTile(atlasIndex);

        // Map the 4 corners of the quad to the atlas tile with corrected orientation
        uvs[0] = glm::vec2(tile.uvMin.x, tile.uvMax.y); // Bottom-left
        uvs[1] = glm::vec2(tile.uvMax.x, tile.uvMax.y); // Bottom-right
        uvs[2] = glm::vec2(tile.uvMax.x, tile.uvMin.y); // Top-right
        uvs[3] = glm::vec2(tile.uvMin.x, tile.uvMin.y); // Top-left
    }

    // Enhanced mesher that can use both model-based and legacy rendering
    void EnhancedInterChunkMesherJob(ChunkSection* sectionPtr, MeshData* meshData, const NeighborContext& ctx) {
        assert(sectionPtr != nullptr);
        assert(meshData != nullptr);
        assert(ctx.center != nullptr);

        /*Log::Debug("EnhancedInterChunkMesherJob starting for chunk (%d, %d) section %d",
                  meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex);*/

        // Validate inputs
        if (meshData->sectionIndex < 0 || meshData->sectionIndex >= Math::SECTIONS_PER_CHUNK) {
            Log::Error("Invalid section index: %d", meshData->sectionIndex);
            delete meshData;
            return;
        }

        // Build mesh data
        std::vector<Render::Vertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve(24 * 64); // Estimate: 64 blocks * max 6 faces * 4 vertices
        indices.reserve(36 * 64);  // Estimate: 64 blocks * max 6 faces * 6 indices

        // Performance counters
        int solidBlocks = 0;
        int facesGenerated = 0;
        int facesCulled = 0;
        int modelBasedBlocks = 0;
        int legacyBlocks = 0;

        // Calculate the world Y offset for this section
        int sectionWorldYOffset = Config::MinY + (meshData->sectionIndex * Math::SECTION_HEIGHT);

        // Setup model mesh context
        ModelMeshContext modelContext;
        modelContext.neighborCtx = &ctx;
        modelContext.atlasBuilder = Render::g_atlasBuilder.get();
        modelContext.facesGenerated = &facesGenerated;
        modelContext.facesCulled = &facesCulled;
        // TODO: Add biome data lookup for actual biome tinting
        modelContext.biomeTemperature = 128;
        modelContext.biomeHumidity = 128;

        // Process each voxel in the 16×16×16 section
        for (int y = 0; y < ChunkSection::SIZE; ++y) {
            for (int z = 0; z < ChunkSection::SIZE; ++z) {
                for (int x = 0; x < ChunkSection::SIZE; ++x) {
                    BlockID blockId = sectionPtr->GetBlockID(x, y, z);
                    if (blockId == BlockID::Air) {
                        continue;
                    }

                    solidBlocks++;

                    // Check if this block uses model-based rendering
                    if (EnhancedBlockRegistry::UsesModelRendering(blockId)) {
                        // Use model-based meshing
                        modelBasedBlocks++;

                        const BlockModel& model = EnhancedBlockRegistry::GetBlockModel(blockId);
                        glm::ivec3 worldPos(x, sectionWorldYOffset + y, z);

                        ModelBasedMesher::MeshBlockWithModel(
                            model, worldPos, modelContext, vertices, indices
                        );
                    } else {
                        // Use legacy meshing for this block
                        legacyBlocks++;

                        const EnhancedBlock& block = EnhancedBlockRegistry::Get(blockId);

                        // Use the existing legacy meshing code
                        LegacyMeshBlock(block, x, y, z, sectionWorldYOffset, ctx,
                                      vertices, indices, facesGenerated, facesCulled);
                    }
                }
            }
        }

        /*Log::Debug("EnhancedInterChunkMesherJob stats: %d solid blocks (%d model-based, %d legacy), "
                  "%d faces generated, %d faces culled",
                  solidBlocks, modelBasedBlocks, legacyBlocks, facesGenerated, facesCulled);*/

        // Finalize mesh data
        {
            extern std::mutex s_uploadMutex;
            extern std::queue<MeshData*> s_uploadQueue;

            std::lock_guard<std::mutex> lock(s_uploadMutex);

            meshData->vertices = std::move(vertices);
            meshData->indices = std::move(indices);

            if (!meshData->vertices.empty()) {
                /*Log::Debug("Enqueueing enhanced mesh for chunk (%d, %d) section %d with %zu vertices",
                          meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex,
                          meshData->vertices.size());*/
                s_uploadQueue.push(meshData);
            } else {
                /*Log::Debug("Enhanced mesh for chunk (%d, %d) section %d is empty, discarding",
                          meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex);*/
                delete meshData;
            }
        }
    }

    // Legacy meshing function for blocks that still use the old system
    void LegacyMeshBlock(const EnhancedBlock& block, int x, int y, int z, int sectionWorldYOffset,
                        const NeighborContext& ctx, std::vector<Render::Vertex>& vertices,
                        std::vector<uint32_t>& indices, int& facesGenerated, int& facesCulled) {

        // Direction constants for neighbor checking (same as original mesher)
        static const int DX[6] = { +1, -1,  0,  0,  0,  0 };
        static const int DY[6] = {  0,  0, +1, -1,  0,  0 };
        static const int DZ[6] = {  0,  0,  0,  0, +1, -1 };

        // Face normals
        static const glm::vec3 NRM[6] = {
            {+1,  0,  0},  // +X
            {-1,  0,  0},  // -X
            { 0, +1,  0},  // +Y
            { 0, -1,  0},  // -Y
            { 0,  0, +1},  // +Z
            { 0,  0, -1}   // -Z
        };

        // Quad corner offsets for each face
        static const int OFFSETS[6][4][3] = {
            // +X face: (x+1, y, z) quad
            {{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}},
            // -X face: (x, y, z) quad
            {{0, 0, 1}, {0, 0, 0}, {0, 1, 0}, {0, 1, 1}},
            // +Y face: (x, y+1, z) quad
            {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
            // -Y face: (x, y, z) quad
            {{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}},
            // +Z face: (x, y, z+1) quad
            {{1, 0, 1}, {0, 0, 1}, {0, 1, 1}, {1, 1, 1}},
            // -Z face: (x, y, z) quad
            {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}
        };

        // Get the section pointer from the neighbor context
        // We need to find the right section in the chunk for intra-section lookups
        ChunkSection* currentSection = nullptr;
        int sectionIndex = (sectionWorldYOffset - Config::MinY) / Math::SECTION_HEIGHT;
        if (sectionIndex >= 0 && sectionIndex < Math::SECTIONS_PER_CHUNK) {
            currentSection = ctx.center->sections[sectionIndex].get();
        }

        // Check each of the 6 potential faces
        for (int face = 0; face < 6; ++face) {
            int nx = x + DX[face];
            int ny = y + DY[face];
            int nz = z + DZ[face];

            bool emitFace = false;
            BlockID neighborId = BlockID::Air;

            // Determine neighbor location and culling strategy
            if (nx >= 0 && nx < ChunkSection::SIZE &&
                ny >= 0 && ny < ChunkSection::SIZE &&
                nz >= 0 && nz < ChunkSection::SIZE) {

                // Neighbor is within current section - fast path
                if (currentSection) {
                    neighborId = currentSection->GetBlockID(nx, ny, nz);
                    emitFace = !IsBlockOpaque(neighborId);
                    if (!emitFace) {
                        facesCulled++;
                    }
                } else {
                    // Section not available, assume air
                    emitFace = true;
                }
            }
            else {
                // Neighbor is outside current section - need enhanced lookup
                int chunkLocalX = x + DX[face];
                int worldY = sectionWorldYOffset + y + DY[face];
                int chunkLocalZ = z + DZ[face];

                // Use the existing neighbor context lookup
                neighborId = GetBlockFromNeighborContext(ctx, chunkLocalX, worldY, chunkLocalZ);
                emitFace = !IsBlockOpaque(neighborId);
                if (!emitFace) {
                    facesCulled++;
                }
            }

            if (!emitFace) {
                continue; // Face is culled
            }

            // Generate quad for this face using legacy texture atlas
            auto baseIndex = static_cast<uint32_t>(vertices.size());

            // Get UV coordinates for this face from legacy texture atlas
            glm::vec2 faceUVs[4];
            // Call our EnhancedBlock-specific function
            GenerateUVsForEnhancedBlock(face, block, faceUVs);

            // Build 4 vertices for the quad
            for (int corner = 0; corner < 4; ++corner) {
                glm::vec3 pos = {
                    static_cast<float>(x + OFFSETS[face][corner][0]),
                    static_cast<float>(y + OFFSETS[face][corner][1]),
                    static_cast<float>(z + OFFSETS[face][corner][2])
                };

                Render::Vertex vert;
                vert.pos = pos;
                vert.nrm = NRM[face];
                vert.uv = faceUVs[corner];
                vert.ao = 255; // No ambient occlusion in this implementation

                vertices.push_back(vert);
            }

            // Generate two triangles (6 indices) for the quad
            indices.insert(indices.end(), {
                baseIndex + 0, baseIndex + 2, baseIndex + 1,
                baseIndex + 0, baseIndex + 3, baseIndex + 2
            });

            facesGenerated++;
        }
    }

    // Helper function to get neighbor block using existing system
    // Note: This calls the implementation from Mesher.cpp to avoid duplication
    // The actual GetBlockFromNeighborContext is implemented in Mesher.cpp

    bool IsBlockOpaque(BlockID blockId); // Forward declaration - implemented in Mesher.cpp

    // Helper function for legacy UV generation - SPECIFIC for EnhancedBlock

    // Note: IsBlockOpaque and GetBlockFromNeighborContext are implemented in Mesher.cpp
    // to avoid duplicate symbols

} // namespace Game