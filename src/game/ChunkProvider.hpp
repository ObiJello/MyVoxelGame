// File: src/game/ChunkProvider.hpp (Enhanced with Inter-Chunk Support)
#pragma once

#include "WorldMath.hpp"
#include "Chunk.hpp"
#include "Mesher.hpp"

// Path to FastNoiseLite.h (adjust if you put it somewhere else):
#include "../../ext/FastNoiseLite.h"

namespace Game {

    class ChunkProvider {
    public:
        // Enqueue a background job to generate the chunk at chunk-coordinates (pos.x, pos.z).
        // After filling in block data, this system will automatically handle neighbor detection
        // and trigger appropriate meshing (standard or enhanced with inter-chunk culling).
        static void RequestChunk(Math::ChunkPos pos);

        // Unload a chunk and trigger remeshing of dependent neighbors
        static void UnloadChunk(Math::ChunkPos pos);

    private:
        ChunkProvider() = delete; // Static class
    };

} // namespace Game