// File: src/game/WorldManager.hpp (Fixed Header)
#pragma once

#include "WorldMath.hpp"
#include "Mesher.hpp"  // Add this for NeighborContext
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <glm/vec3.hpp>

namespace Game {

    // Forward declarations
    class Chunk;

    struct ChunkPosHash {
        size_t operator()(const Math::ChunkPos& p) const noexcept {
            return std::hash<int>()(p.x) ^ (std::hash<int>()(p.z) << 1);
        }
    };

    class WorldManager {
    public:
        // Configurable render radius (number of chunks in each direction from camera)
        static constexpr int RENDER_RADIUS = 8; // Increased for better inter-chunk culling testing

        // Call once per frame with camera world position
        // Manages chunk loading/unloading and coordinates with the enhanced meshing system
        static void Update(const glm::vec3& cameraPos);

        // Force remesh a specific chunk (useful for debugging or after block changes)
        static void ForceRemeshChunk(Math::ChunkPos pos);

        // Query functions for debugging and performance monitoring
        static size_t GetLoadedChunkCount();
        static size_t GetRenderedSectionCount();
        static bool IsChunkLoaded(Math::ChunkPos pos);
        static std::vector<Math::ChunkPos> GetLoadedChunks();

    private:
        // Set of currently loaded chunk coordinates
        static std::unordered_set<Math::ChunkPos, ChunkPosHash> s_loaded;

        // Track when chunks were loaded for grace period management
        static std::unordered_map<Math::ChunkPos, std::chrono::steady_clock::time_point, ChunkPosHash> s_loadTimes;

        // Internal helper functions
        static void LoadChunk(Math::ChunkPos pos);
        static void UnloadChunk(Math::ChunkPos pos);

        // NOTE: Helper functions like MakeChunkKey, GetNeighborChunk, etc. are now
        // implemented as static functions inside WorldManager.cpp and don't need
        // to be declared in the header since they're only used internally.
    };

} // namespace Game