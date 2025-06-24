// File: src/game/Mesher.cpp (Enhanced Version - Simple)
#include "Mesher.hpp"
#include "Log.hpp"
#include "../render/Vertex.hpp"
#include "../game/WorldMath.hpp"
#include "../game/ChunkSection.hpp"
#include "../game/BlockRegistry.hpp"
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>
#include <mutex>
#include <queue>
#include <cassert>

namespace Game {

    // Use these 6 directions to test neighbors and build face quads.
    static const int DX[6] = { +1, -1,  0,  0,  0,  0 };
    static const int DY[6] = {  0,  0, +1, -1,  0,  0 };
    static const int DZ[6] = {  0,  0,  0,  0, +1, -1 };

    // Corresponding face normals
    static const glm::vec3 NRM[6] = {
        {+1,  0,  0},  // +X
        {-1,  0,  0},  // -X
        { 0, +1,  0},  // +Y
        { 0, -1,  0},  // -Y
        { 0,  0, +1},  // +Z
        { 0,  0, -1}   // -Z
    };

    // For each face, these are the 4 local‐voxel offsets (x,y,z) of the quad corners
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

    // Simple UVs for each corner (full‐face)
    static const glm::vec2 UVS[4] = {
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f}
    };

    // Internal upload queue and mutex
    static std::mutex             s_uploadMutex;
    static std::queue<MeshData*>  s_uploadQueue;

    // Helper function to check if a block is opaque
    static bool IsBlockOpaque(BlockID blockId) {
        if (blockId == BlockID::Air) {
            return false;
        }

        const Block& block = BlockRegistry::Get(blockId);
        return block.opaque;
    }

    void MesherJob(ChunkSection* section, MeshData* meshData) {
        assert(section != nullptr);
        assert(meshData != nullptr);

        Log::Debug("MesherJob starting for chunk (%d, %d) section %d",
                  meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex);

        // Validate input data
        if (meshData->chunkXZ.x < -1000000 || meshData->chunkXZ.x > 1000000 ||
            meshData->chunkXZ.y < -1000000 || meshData->chunkXZ.y > 1000000) {
            Log::Error("Invalid chunk coordinates in MeshData: (%d, %d)",
                      meshData->chunkXZ.x, meshData->chunkXZ.y);
            delete meshData;
            return;
        }

        // Create a temporary MeshData to build the mesh in
        MeshData tempMesh;
        tempMesh.chunkXZ = meshData->chunkXZ;
        tempMesh.sectionIndex = meshData->sectionIndex;
        tempMesh.vertices.clear();
        tempMesh.indices.clear();

        int solidBlocks = 0;
        int facesGenerated = 0;
        int facesCulled = 0;

        // Iterate all voxels in the 16×16×16 section
        for (int y = 0; y < ChunkSection::SIZE; ++y) {
            for (int z = 0; z < ChunkSection::SIZE; ++z) {
                for (int x = 0; x < ChunkSection::SIZE; ++x) {
                    BlockID blockId = section->GetBlockID(x, y, z);
                    if (blockId == BlockID::Air) {
                        continue; // skip air
                    }

                    // Only render faces for opaque blocks
                    if (!IsBlockOpaque(blockId)) {
                        continue;
                    }

                    solidBlocks++;

                    // Get the block info for texture indices
                    const Block& block = BlockRegistry::Get(blockId);

                    // For each of 6 possible faces:
                    for (int face = 0; face < 6; ++face) {
                        int nx = x + DX[face];
                        int ny = y + DY[face];
                        int nz = z + DZ[face];

                        bool emitFace = false;

                        // Check if neighbor is outside section bounds
                        if (nx < 0 || nx >= ChunkSection::SIZE ||
                            ny < 0 || ny >= ChunkSection::SIZE ||
                            nz < 0 || nz >= ChunkSection::SIZE) {

                            // Neighbor is outside current section
                            // For now, assume section boundaries are exposed (render face)
                            // This is a simplification - for full optimization you'd need
                            // to check neighboring sections/chunks
                            emitFace = true;
                        } else {
                            // Neighbor is within current section - check if it's opaque
                            BlockID neighborId = section->GetBlockID(nx, ny, nz);

                            // Only emit face if neighbor is not opaque (air or transparent)
                            emitFace = !IsBlockOpaque(neighborId);

                            if (!emitFace) {
                                facesCulled++;
                            }
                        }

                        if (!emitFace) {
                            continue;
                        }

                        // At this point, we must emit a quad for this face.
                        auto baseIndex = static_cast<uint32_t>(tempMesh.vertices.size());

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
                            vert.uv  = UVS[corner];
                            vert.ao  = 255; // no AO in MVP

                            // Note: You could use block.texIdx[face] here if you want
                            // per-face texturing, but you'd need to modify the Vertex
                            // struct and shader to handle texture atlas indices

                            tempMesh.vertices.push_back(vert);
                        }

                        // Two triangles per quad, using baseIndex
                        tempMesh.indices.push_back(baseIndex + 0);
                        tempMesh.indices.push_back(baseIndex + 1);
                        tempMesh.indices.push_back(baseIndex + 2);

                        tempMesh.indices.push_back(baseIndex + 2);
                        tempMesh.indices.push_back(baseIndex + 3);
                        tempMesh.indices.push_back(baseIndex + 0);

                        facesGenerated++;
                    }
                }
            }
        }

        Log::Debug("MesherJob: found %d solid blocks, generated %d faces (%d culled), %zu vertices, %zu indices",
                  solidBlocks, facesGenerated, facesCulled, tempMesh.vertices.size(), tempMesh.indices.size());

        // Copy the completed data to the output mesh
        {
            std::lock_guard<std::mutex> lock(s_uploadMutex);

            meshData->vertices = std::move(tempMesh.vertices);
            meshData->indices = std::move(tempMesh.indices);

            if (meshData->vertices.size() > 0) {
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