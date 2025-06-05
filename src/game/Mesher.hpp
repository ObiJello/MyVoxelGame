#pragma once

#include <vector>
#include <cstdint>
#include <mutex>
#include <queue>
#include "ChunkSection.hpp"

namespace Game {

    // MeshData will hold a list of vertex attributes and indices.
    // In Step 4, replace VertexPlaceholder with the real Vertex type from render/Vertex.hpp.
    struct MeshData {
        std::vector<float>      vertices; // placeholder: flatten your vertex attributes here
        std::vector<uint32_t>   indices;
    };

    // The mesher function that runs on a worker thread.
    // - section: pointer to the ChunkSection to mesh
    // - outData: pointer to a pre-allocated MeshData object where we write v/i
    void MesherJob(ChunkSection* section, MeshData* outData);

    // Push completed MeshData onto a thread-safe upload queue.
    // The render thread will call PopMeshData() to retrieve and upload to GPU.
    void PushMeshData(MeshData* data);

    // Pop one MeshData from the queue. Returns true if an entry was popped.
    // If true, outData will be set to a valid pointer that the caller must delete
    // (or otherwise free). If false, the queue was empty.
    bool PopMeshData(MeshData*& outData);

} // namespace Game
