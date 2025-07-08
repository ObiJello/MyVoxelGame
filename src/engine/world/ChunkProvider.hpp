// File: src/engine/world/ChunkProvider.hpp
#pragma once

#include "../../game/WorldMath.hpp"
#include "Chunk.hpp"
#include "../../render/mesh/Mesher.hpp"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <atomic>

// Path to FastNoiseLite.h (adjust if you put it somewhere else):
#include "../../../ext/FastNoiseLite.h"

namespace Game {

    // Forward declare ChunkData for external access
    struct ChunkData {
        std::shared_ptr<Chunk> chunk;
        std::unordered_set<uint64_t> dependents;  // Chunks that depend on this one for meshing
        std::atomic<bool> isGenerated{false};     // Block data is complete
        std::atomic<bool> hasPendingMesh{false};  // Mesh generation is queued
        std::atomic<int> neighborCount{0};        // Number of available neighbors (0-4)

        ChunkData(std::shared_ptr<Chunk> c) : chunk(std::move(c)) {}
    };

    // External declarations for chunk registry (defined in ChunkProvider.cpp)
    extern std::unordered_map<uint64_t, std::unique_ptr<ChunkData>> s_chunkRegistry;
    extern std::shared_mutex s_registryMutex;

    class ChunkProvider {
    public:
        // Enqueue a background job to generate the chunk at chunk-coordinates (pos.x, pos.z).
        // After filling in block data, this system will automatically handle neighbor detection and trigger appropriate meshing.
        static void RequestChunk(Math::ChunkPos pos);

        // Unload a chunk and trigger remeshing of dependent neighbors
        static void UnloadChunk(Math::ChunkPos pos);

    private:
        ChunkProvider() = delete; // Static class
    };

} // namespace Game