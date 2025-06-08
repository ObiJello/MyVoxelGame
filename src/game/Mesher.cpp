// File: src/game/Mesher.cpp
#include "Mesher.hpp"
#include "Log.hpp"
#include "../render/Vertex.hpp"
#include "../game/WorldMath.hpp"
#include "../game/ChunkSection.hpp"
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
    // Ordered so that the winding is counter‐clockwise from the outside.
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

        // Iterate all voxels in the 16×16×16 section
        for (int y = 0; y < ChunkSection::SIZE; ++y) {
            for (int z = 0; z < ChunkSection::SIZE; ++z) {
                for (int x = 0; x < ChunkSection::SIZE; ++x) {
                    uint16_t raw = section->Get(x, y, z);
                    if (raw == static_cast<uint16_t>(BlockID::Air)) {
                        continue; // skip air
                    }

                    solidBlocks++;

                    // For each of 6 possible faces:
                    for (int face = 0; face < 6; ++face) {
                        int nx = x + DX[face];
                        int ny = y + DY[face];
                        int nz = z + DZ[face];

                        bool emitFace = false;

                        // If neighbor is outside [0..15], treat as Air (emit face)
                        if (nx < 0 || nx >= ChunkSection::SIZE ||
                            ny < 0 || ny >= ChunkSection::SIZE ||
                            nz < 0 || nz >= ChunkSection::SIZE) {
                            emitFace = true;
                        } else {
                            uint16_t neighborRaw = section->Get(nx, ny, nz);
                            if (neighborRaw == static_cast<uint16_t>(BlockID::Air)) {
                                emitFace = true;
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

        Log::Debug("MesherJob: found %d solid blocks, generated %d faces, %zu vertices, %zu indices",
                  solidBlocks, facesGenerated, tempMesh.vertices.size(), tempMesh.indices.size());

        // CRITICAL FIX: Only after the mesh is completely built,
        // copy it to the output MeshData atomically
        {
            std::lock_guard<std::mutex> lock(s_uploadMutex);

            // Move the completed data to the output mesh
            meshData->vertices = std::move(tempMesh.vertices);
            meshData->indices = std::move(tempMesh.indices);

            // Validate the final mesh data before enqueueing
            if (meshData->vertices.size() > 0) {
                Log::Debug("Enqueueing mesh for chunk (%d, %d) section %d with %zu vertices",
                          meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex,
                          meshData->vertices.size());
                s_uploadQueue.push(meshData);
            } else {
                Log::Debug("Mesh for chunk (%d, %d) section %d is empty, not enqueueing",
                          meshData->chunkXZ.x, meshData->chunkXZ.y, meshData->sectionIndex);
                delete meshData; // Don't enqueue empty meshes
            }
        }
    }

    void PushMeshData(MeshData* data) {
        // This function is now called from within MesherJob under lock
        // So we don't need to do anything here, but keep it for API compatibility
        // The actual push happens in MesherJob after the mesh is complete
    }

    bool PopMeshData(MeshData*& outData) {
        std::lock_guard<std::mutex> lock(s_uploadMutex);
        if (s_uploadQueue.empty()) {
            outData = nullptr;
            return false;
        }
        outData = s_uploadQueue.front();
        s_uploadQueue.pop();

        // Validate the popped data
        if (outData) {
            Log::Debug("Popped mesh data for chunk (%d, %d) section %d with %zu vertices",
                      outData->chunkXZ.x, outData->chunkXZ.y, outData->sectionIndex,
                      outData->vertices.size());
        }

        return true;
    }

} // namespace Game