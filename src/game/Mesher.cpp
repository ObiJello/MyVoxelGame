// File: src/game/Mesher.cpp (Enhanced with Texture Atlas Support)
#include "Mesher.hpp"
#include "Log.hpp"
#include "../render/Vertex.hpp"
#include "../render/TextureAtlas.hpp"  // Add texture atlas support
#include "../game/WorldMath.hpp"
#include "../game/ChunkSection.hpp"
#include "../game/BlockRegistry.hpp"
#include "../game/Chunk.hpp"
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>
#include <mutex>
#include <queue>
#include <cassert>

namespace Game {

    // Direction constants for neighbor checking
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

    // Thread-safe upload queue
    static std::mutex s_uploadMutex;
    static std::queue<MeshData*> s_uploadQueue;

    // Helper function to check if a block is opaque
    static bool IsBlockOpaque(BlockID blockId) {
        if (blockId == BlockID::Air) {
            return false;
        }
        const Block& block = BlockRegistry::Get(blockId);
        return block.opaque;
    }

    // Enhanced block lookup with full inter-chunk support
    static BlockID GetBlockFromNeighborContext(const NeighborContext& ctx, int localX, int localY, int localZ) {
        // Validate Y bounds first (common case) - return Air for out-of-world coordinates
        if (localY < 0 || localY >= Math::CHUNK_TOTAL_HEIGHT) {
            return BlockID::Air;
        }

        // Handle within-chunk coordinates (most common case)
        if (localX >= 0 && localX < Math::CHUNK_SIZE_X &&
            localZ >= 0 && localZ < Math::CHUNK_SIZE_Z) {
            return ctx.center->GetBlock(localX, localY, localZ);
        }

        // Handle cross-chunk coordinates - only for horizontal neighbors
        std::shared_ptr<Chunk> targetChunk = nullptr;
        int targetX = localX;
        int targetZ = localZ;

        // Determine which neighbor to use and adjust coordinates
        if (localX < 0 && localZ >= 0 && localZ < Math::CHUNK_SIZE_Z) {
            // West neighbor
            targetChunk = ctx.neighbors[0];
            targetX = localX + Math::CHUNK_SIZE_X;
        } else if (localX >= Math::CHUNK_SIZE_X && localZ >= 0 && localZ < Math::CHUNK_SIZE_Z) {
            // East neighbor
            targetChunk = ctx.neighbors[1];
            targetX = localX - Math::CHUNK_SIZE_X;
        } else if (localZ < 0 && localX >= 0 && localX < Math::CHUNK_SIZE_X) {
            // North neighbor
            targetChunk = ctx.neighbors[2];
            targetZ = localZ + Math::CHUNK_SIZE_Z;
        } else if (localZ >= Math::CHUNK_SIZE_Z && localX >= 0 && localX < Math::CHUNK_SIZE_X) {
            // South neighbor
            targetChunk = ctx.neighbors[3];
            targetZ = localZ - Math::CHUNK_SIZE_Z;
        } else {
            // Corner case or diagonal neighbor - not supported, return Air
            return BlockID::Air;
        }

        // Return air if neighbor isn't available or coordinates are still out of bounds
        if (!targetChunk || targetX < 0 || targetX >= Math::CHUNK_SIZE_X ||
            targetZ < 0 || targetZ >= Math::CHUNK_SIZE_Z) {
            return BlockID::Air;
        }

        return targetChunk->GetBlock(targetX, localY, targetZ);
    }

    // Optimized block lookup for intra-chunk access
    static BlockID GetBlockFromChunk(const Chunk* chunk, int localX, int localY, int localZ) {
        if (localX < 0 || localX >= Math::CHUNK_SIZE_X ||
            localZ < 0 || localZ >= Math::CHUNK_SIZE_Z ||
            localY < 0 || localY >= Math::CHUNK_TOTAL_HEIGHT) {
            return BlockID::Air;
        }

        int sectionIdx = localY / Math::SECTION_HEIGHT;
        int yInSection = localY % Math::SECTION_HEIGHT;

        if (sectionIdx < 0 || sectionIdx >= Math::SECTIONS_PER_CHUNK) {
            return BlockID::Air;
        }

        if (!chunk->sections[sectionIdx]) {
            return BlockID::Air;
        }

        return chunk->sections[sectionIdx]->GetBlockID(localX, yInSection, localZ);
    }

    // Helper function to generate UV coordinates for a face using texture atlas
    static void GenerateUVsForFace(int face, const Block& block, glm::vec2 uvs[4]) {
        // Get the atlas index for this face
        uint8_t atlasIndex = block.texIdx[face];

        // Get the UV tile from the texture atlas
        Render::AtlasTile tile = Render::g_textureAtlas.GetTile(atlasIndex);

        // Map the 4 corners of the quad to the atlas tile
        // The UV coordinates correspond to the 4 corners in the OFFSETS array
        uvs[0] = glm::vec2(tile.uvMin.x, tile.uvMin.y); // Bottom-left
        uvs[1] = glm::vec2(tile.uvMax.x, tile.uvMin.y); // Bottom-right
        uvs[2] = glm::vec2(tile.uvMax.x, tile.uvMax.y); // Top-right
        uvs[3] = glm::vec2(tile.uvMin.x, tile.uvMax.y); // Top-left
    }

    // **NEW** Inter-chunk meshing function with full neighbor support and texture atlas
    void InterChunkMesherJob(ChunkSection* section, MeshData* meshData, const NeighborContext& ctx) {
        assert(section != nullptr);
        assert(meshData != nullptr);
        assert(ctx.center != nullptr);

        Log::Debug("InterChunkMesherJob starting for chunk (%d, %d) section %d with texture atlas support",
                  meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex);

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
        int facesCulledIntraSection = 0;
        int facesCulledInterSection = 0;
        int facesCulledInterChunk = 0;

        // Calculate the Y offset for this section within the chunk
        int sectionYOffset = meshData->sectionIndex * Math::SECTION_HEIGHT;

        // Process each voxel in the 16×16×16 section
        for (int y = 0; y < ChunkSection::SIZE; ++y) {
            for (int z = 0; z < ChunkSection::SIZE; ++z) {
                for (int x = 0; x < ChunkSection::SIZE; ++x) {
                    BlockID blockId = section->GetBlockID(x, y, z);
                    if (blockId == BlockID::Air || !IsBlockOpaque(blockId)) {
                        continue;
                    }

                    solidBlocks++;
                    const Block& block = BlockRegistry::Get(blockId);

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
                            neighborId = section->GetBlockID(nx, ny, nz);
                            emitFace = !IsBlockOpaque(neighborId);
                            if (!emitFace) {
                                facesCulledIntraSection++;
                            }
                        }
                        else {
                            // Neighbor is outside current section - need enhanced lookup
                            int chunkLocalX = x + DX[face];
                            int chunkLocalY = (sectionYOffset + y) + DY[face];
                            int chunkLocalZ = z + DZ[face];

                            // Check if neighbor is within the same chunk
                            if (chunkLocalX >= 0 && chunkLocalX < Math::CHUNK_SIZE_X &&
                                chunkLocalZ >= 0 && chunkLocalZ < Math::CHUNK_SIZE_Z &&
                                chunkLocalY >= 0 && chunkLocalY < Math::CHUNK_TOTAL_HEIGHT) {

                                // Inter-section lookup within the same chunk
                                neighborId = GetBlockFromChunk(ctx.center.get(), chunkLocalX, chunkLocalY, chunkLocalZ);
                                emitFace = !IsBlockOpaque(neighborId);
                                if (!emitFace) {
                                    facesCulledInterSection++;
                                }
                            }
                            else if (chunkLocalY < 0 || chunkLocalY >= Math::CHUNK_TOTAL_HEIGHT) {
                                // Neighbor is outside world Y bounds - always emit face (top/bottom of world)
                                emitFace = true;
                            }
                            else if (ctx.hasAllNeighbors) {
                                // **ENHANCED** Inter-chunk lookup with neighbor context
                                // Only do this for horizontal neighbors (X/Z), not vertical (Y)
                                neighborId = GetBlockFromNeighborContext(ctx, chunkLocalX, chunkLocalY, chunkLocalZ);
                                emitFace = !IsBlockOpaque(neighborId);
                                if (!emitFace) {
                                    facesCulledInterChunk++;
                                }
                            }
                            else {
                                // No neighbor available - assume air (will be corrected when neighbor loads)
                                emitFace = true;
                            }
                        }

                        if (!emitFace) {
                            continue; // Face is culled
                        }

                        // Generate quad for this face with proper texture atlas UVs
                        auto baseIndex = static_cast<uint32_t>(vertices.size());

                        // Get UV coordinates for this face from texture atlas
                        glm::vec2 faceUVs[4];
                        GenerateUVsForFace(face, block, faceUVs);

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
                            vert.uv = faceUVs[corner]; // Use atlas UV coordinates
                            vert.ao = 255; // No ambient occlusion in this implementation

                            vertices.push_back(vert);
                        }

                        // Generate two triangles (6 indices) for the quad
                        // Use counter-clockwise winding when viewed from outside the block
                        indices.insert(indices.end(), {
                            baseIndex + 0, baseIndex + 2, baseIndex + 1,
                            baseIndex + 0, baseIndex + 3, baseIndex + 2
                        });

                        facesGenerated++;
                    }
                }
            }
        }

        Log::Debug("InterChunkMesherJob stats: %d solid blocks, %d faces generated, "
                  "%d culled intra-section, %d culled inter-section, %d culled inter-chunk",
                  solidBlocks, facesGenerated, facesCulledIntraSection,
                  facesCulledInterSection, facesCulledInterChunk);

        // Finalize mesh data
        {
            std::lock_guard<std::mutex> lock(s_uploadMutex);

            meshData->vertices = std::move(vertices);
            meshData->indices = std::move(indices);

            if (!meshData->vertices.empty()) {
                Log::Debug("Enqueueing inter-chunk mesh for chunk (%d, %d) section %d with %zu vertices",
                          meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex,
                          meshData->vertices.size());
                s_uploadQueue.push(meshData);
            } else {
                Log::Debug("Inter-chunk mesh for chunk (%d, %d) section %d is empty, discarding",
                          meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex);
                delete meshData;
            }
        }
    }

    // Enhanced meshing function with improved intra-chunk culling and texture atlas support
    void MesherJob(ChunkSection* section, MeshData* meshData, const Chunk* chunk) {
        assert(section != nullptr);
        assert(meshData != nullptr);
        assert(chunk != nullptr);

        Log::Debug("MesherJob starting for chunk (%d, %d) section %d with texture atlas support",
                  meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex);

        // Validate input data
        if (meshData->chunkXZ.x < -1000000 || meshData->chunkXZ.x > 1000000 ||
            meshData->chunkXZ.y < -1000000 || meshData->chunkXZ.y > 1000000) {
            Log::Error("Invalid chunk coordinates in MeshData: (%d, %d)",
                      meshData->chunkXZ.x, meshData->chunkXZ.y);
            delete meshData;
            return;
        }

        if (meshData->sectionIndex < 0 || meshData->sectionIndex >= Math::SECTIONS_PER_CHUNK) {
            Log::Error("Invalid section index in MeshData: %d", meshData->sectionIndex);
            delete meshData;
            return;
        }

        // Build mesh data
        std::vector<Render::Vertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve(24 * 64);
        indices.reserve(36 * 64);

        int solidBlocks = 0;
        int facesGenerated = 0;
        int facesCulledIntraSection = 0;
        int facesCulledInterSection = 0;

        // Calculate the Y offset for this section within the chunk
        int sectionYOffset = meshData->sectionIndex * Math::SECTION_HEIGHT;

        // Iterate all voxels in the 16×16×16 section
        for (int y = 0; y < ChunkSection::SIZE; ++y) {
            for (int z = 0; z < ChunkSection::SIZE; ++z) {
                for (int x = 0; x < ChunkSection::SIZE; ++x) {
                    BlockID blockId = section->GetBlockID(x, y, z);
                    if (blockId == BlockID::Air || !IsBlockOpaque(blockId)) {
                        continue;
                    }

                    solidBlocks++;
                    const Block& block = BlockRegistry::Get(blockId);

                    // For each of 6 possible faces:
                    for (int face = 0; face < 6; ++face) {
                        int nx = x + DX[face];
                        int ny = y + DY[face];
                        int nz = z + DZ[face];

                        bool emitFace = false;
                        BlockID neighborId = BlockID::Air;

                        // Check neighbor location and determine culling strategy
                        if (nx >= 0 && nx < ChunkSection::SIZE &&
                            ny >= 0 && ny < ChunkSection::SIZE &&
                            nz >= 0 && nz < ChunkSection::SIZE) {

                            // Neighbor is within current section - use fast intra-section lookup
                            neighborId = section->GetBlockID(nx, ny, nz);
                            emitFace = !IsBlockOpaque(neighborId);

                            if (!emitFace) {
                                facesCulledIntraSection++;
                            }
                        }
                        else {
                            // Neighbor is outside current section - need inter-section lookup
                            int chunkLocalX = x + DX[face];
                            int chunkLocalY = (sectionYOffset + y) + DY[face];
                            int chunkLocalZ = z + DZ[face];

                            // Check if neighbor is within the chunk bounds
                            if (chunkLocalX >= 0 && chunkLocalX < Math::CHUNK_SIZE_X &&
                                chunkLocalZ >= 0 && chunkLocalZ < Math::CHUNK_SIZE_Z &&
                                chunkLocalY >= 0 && chunkLocalY < Math::CHUNK_TOTAL_HEIGHT) {

                                // Use enhanced lookup function for cross-section access
                                neighborId = GetBlockFromChunk(chunk, chunkLocalX, chunkLocalY, chunkLocalZ);
                                emitFace = !IsBlockOpaque(neighborId);

                                if (!emitFace) {
                                    facesCulledInterSection++;
                                }
                            }
                            else {
                                // Neighbor is outside chunk bounds - assume it's air/transparent
                                // This will be handled by inter-chunk culling when neighbors are available
                                emitFace = true;
                            }
                        }

                        if (!emitFace) {
                            continue;
                        }

                        // Generate quad for this face with proper texture atlas UVs
                        auto baseIndex = static_cast<uint32_t>(vertices.size());

                        // Get UV coordinates for this face from texture atlas
                        glm::vec2 faceUVs[4];
                        GenerateUVsForFace(face, block, faceUVs);

                        // Build 4 vertices
                        for (int corner = 0; corner < 4; ++corner) {
                            glm::vec3 pos = {
                                static_cast<float>(x + OFFSETS[face][corner][0]),
                                static_cast<float>(y + OFFSETS[face][corner][1]),
                                static_cast<float>(z + OFFSETS[face][corner][2])
                            };

                            Render::Vertex vert;
                            vert.pos = pos;
                            vert.nrm = NRM[face];
                            vert.uv  = faceUVs[corner]; // Use atlas UV coordinates
                            vert.ao  = 255;

                            vertices.push_back(vert);
                        }

                        // Two triangles per quad with correct winding
                        indices.insert(indices.end(), {
                            baseIndex + 0, baseIndex + 2, baseIndex + 1,
                            baseIndex + 0, baseIndex + 3, baseIndex + 2
                        });

                        facesGenerated++;
                    }
                }
            }
        }

        Log::Debug("MesherJob: found %d solid blocks, generated %d faces (%d culled intra-section, %d culled inter-section), %zu vertices, %zu indices",
                  solidBlocks, facesGenerated, facesCulledIntraSection, facesCulledInterSection,
                  vertices.size(), indices.size());

        // Copy the completed data to the output mesh
        {
            std::lock_guard<std::mutex> lock(s_uploadMutex);

            meshData->vertices = std::move(vertices);
            meshData->indices = std::move(indices);

            if (!meshData->vertices.empty()) {
                Log::Debug("Enqueueing mesh for chunk (%d, %d) section %d with %zu vertices",
                          meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex,
                          meshData->vertices.size());
                s_uploadQueue.push(meshData);
            } else {
                Log::Debug("Mesh for chunk (%d, %d) section %d is empty, not enqueueing",
                          meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex);
                delete meshData;
            }
        }
    }

    // Legacy function for backward compatibility - logs a warning and delegates
    void MesherJob(ChunkSection* section, MeshData* meshData) {
        Log::Warning("MesherJob called without chunk context - inter-section culling disabled");

        // Create a minimal temporary chunk with just this section for compatibility
        Chunk tempChunk;
        tempChunk.sections[meshData->sectionIndex] = std::unique_ptr<ChunkSection>(section);

        // Call the enhanced version
        MesherJob(section, meshData, &tempChunk);

        // Release the section from the temporary chunk to prevent double-deletion
        tempChunk.sections[meshData->sectionIndex].release();
    }

    void PushMeshData(MeshData* data) {
        // Keep for API compatibility
    }

    bool PopMeshData(MeshData*& outData) {
        std::lock_guard<std::mutex> lock(s_uploadMutex);
        if (s_uploadQueue.empty()) {
            outData = nullptr;
            return false;
        }
        outData = s_uploadQueue.front();
        s_uploadQueue.pop();

        if (outData) {
            Log::Debug("Popped mesh data for chunk (%d, %d) section %d with %zu vertices",
                      outData->chunkXZ.x, outData->chunkXZ.y, outData->sectionIndex,
                      outData->vertices.size());
        }

        return true;
    }

} // namespace Game