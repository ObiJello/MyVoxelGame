#include "Mesher.hpp"
#include "Log.hpp"
#include "../render/Vertex.hpp"
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

    void MesherJob(ChunkSection* section, MeshData* outData) {
        assert(section != nullptr);
        assert(outData != nullptr);

        outData->vertices.clear();
        outData->indices.clear();

        // Iterate all voxels in the 16×16×16 section
        for (int y = 0; y < ChunkSection::SIZE; ++y) {
            for (int z = 0; z < ChunkSection::SIZE; ++z) {
                for (int x = 0; x < ChunkSection::SIZE; ++x) {
                    uint16_t raw = section->Get(x, y, z);
                    if (raw == static_cast<uint16_t>(BlockID::Air)) {
                        continue; // skip air
                    }

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
                        uint32_t baseIndex = static_cast<uint32_t>(outData->vertices.size());

                        // Build 4 vertices
                        for (int corner = 0; corner < 4; ++corner) {
                            // Local voxel-space position; each block is 1×1×1, so
                            // pos = (sectionOffset + (x,y,z) +/- offsets)
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
                            outData->vertices.push_back(vert);
                        }

                        // Two triangles per quad, using baseIndex
                        // Winding order is CCW looking at the face from outside
                        outData->indices.push_back(baseIndex + 0);
                        outData->indices.push_back(baseIndex + 1);
                        outData->indices.push_back(baseIndex + 2);

                        outData->indices.push_back(baseIndex + 2);
                        outData->indices.push_back(baseIndex + 3);
                        outData->indices.push_back(baseIndex + 0);
                    }
                }
            }
        }

        // Once done, enqueue for upload
        PushMeshData(outData);
    }

    void PushMeshData(MeshData* data) {
        std::lock_guard<std::mutex> lock(s_uploadMutex);
        s_uploadQueue.push(data);
    }

    bool PopMeshData(MeshData*& outData) {
        std::lock_guard<std::mutex> lock(s_uploadMutex);
        if (s_uploadQueue.empty()) {
            return false;
        }
        outData = s_uploadQueue.front();
        s_uploadQueue.pop();
        return true;
    }

} // namespace Game