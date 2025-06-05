#pragma once

#include <vector>
#include <cstdint>
#include "../render/Vertex.hpp"
#include "ChunkSection.hpp"
#include "WorldMath.hpp"
#include <glm/vec2.hpp>

namespace Game {

    struct MeshData {
        glm::ivec2                  chunkXZ;       // (x,z) of the chunk
        int                         sectionIndex;  // sub‐chunk (0..15)
        std::vector<Render::Vertex> vertices;      // vertex data for this section
        std::vector<uint32_t>       indices;       // index buffer data
    };

    // This gets run on a worker thread. section != nullptr, outData is a fresh MeshData*
    void MesherJob(ChunkSection* section, MeshData* outData);

    // Push a completed MeshData* into the thread‐safe queue
    void PushMeshData(MeshData* data);

    // Pop a MeshData* from the queue. Returns false if queue is empty.
    bool PopMeshData(MeshData*& outData);

} // namespace Game