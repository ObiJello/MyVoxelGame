// File: src/game/ChunkProvider.hpp
#pragma once

#include "WorldMath.hpp"
#include "Chunk.hpp"
#include "Mesher.hpp"

// Path to FastNoiseLite.h (adjust if you put it somewhere else):
#include "../../ext/FastNoiseLite.h"

namespace Game {

    class ChunkProvider {
    public:
        // Enqueue a background job to generate the chunk at chunk‐coordinates (pos.x, pos.z).
        // After filling in block data, this job will enqueue MesherJob for every non‐empty section.
        static void RequestChunk(Math::ChunkPos pos);
    };

} // namespace Game
