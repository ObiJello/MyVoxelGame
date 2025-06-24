// File: src/game/Mesher.hpp (Enhanced with Inter-Chunk Support)
#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <array>
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

    // Neighbor context for inter-chunk meshing
    struct NeighborContext {
        std::shared_ptr<Chunk> center;           // The chunk being meshed
        std::array<std::shared_ptr<Chunk>, 4> neighbors; // 4-directional neighbors: W, E, N, S
        bool hasAllNeighbors;                    // True if all 4 neighbors are available

        // Constructor
        NeighborContext(std::shared_ptr<Chunk> centerChunk)
            : center(std::move(centerChunk)), hasAllNeighbors(false) {
            neighbors.fill(nullptr);
        }
    };

    // **NEW** Inter-chunk mesher job with full neighbor support
    // This is the most advanced mesher that can perform face culling across chunk boundaries
    // when all 4 horizontal neighbors are available
    void InterChunkMesherJob(ChunkSection* section, MeshData* outData, const NeighborContext& ctx);

    // Enhanced mesher job that takes the entire chunk for inter-section face culling
    // This version can cull faces between sections within the same chunk
    void MesherJob(ChunkSection* section, MeshData* outData, const Chunk* chunk);

    // Legacy mesher job for backward compatibility - DEPRECATED
    // This version cannot perform inter-section culling and will log a warning
    // Use one of the enhanced versions above for optimal performance
    void MesherJob(ChunkSection* section, MeshData* outData);

    // Push a completed MeshData* into the thread‐safe queue
    void PushMeshData(MeshData* data);

    // Pop a MeshData* from the queue. Returns false if queue is empty.
    bool PopMeshData(MeshData*& outData);

} // namespace Game