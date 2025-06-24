// File: src/game/Mesher.hpp (Updated with Inter-Section Support)
#pragma once

#include <vector>
#include <cstdint>
#include "../render/Vertex.hpp"
#include "ChunkSection.hpp"
#include "WorldMath.hpp"
#include <glm/vec2.hpp>

namespace Game {
    // Forward declarations to avoid circular includes
    class Chunk;

    struct MeshData {
        glm::ivec2                  chunkXZ;       // (x,z) of the chunk
        int                         sectionIndex;  // sub‐chunk (0..15)
        std::vector<Render::Vertex> vertices;      // vertex data for this section
        std::vector<uint32_t>       indices;       // index buffer data
    };

    // Enhanced mesher job that takes the entire chunk for inter-section face culling
    // This is the preferred method as it enables proper face culling between sections
    void MesherJob(ChunkSection* section, MeshData* outData, const Chunk* chunk);

    // Legacy mesher job for backward compatibility - DEPRECATED
    // This version cannot perform inter-section culling and will log a warning
    // Use the enhanced version above for optimal performance
    void MesherJob(ChunkSection* section, MeshData* outData);

    // Push a completed MeshData* into the thread‐safe queue
    void PushMeshData(MeshData* data);

    // Pop a MeshData* from the queue. Returns false if queue is empty.
    bool PopMeshData(MeshData*& outData);

} // namespace Game