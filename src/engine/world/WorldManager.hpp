// File: src/engine/world/WorldManager.hpp (Simplified Header)
#pragma once

#include "../../game/WorldMath.hpp"
#include "../../render/mesh/Mesher.hpp"
#include <unordered_set>
#include <vector>
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
        // Manages chunk loading/unloading and coordinates with the meshing system
        static void Update(const glm::vec3& cameraPos);

        // Force remesh a specific chunk (useful for debugging or after block changes)
        static void ForceRemeshChunk(Math::ChunkPos pos);

        // Query functions for debugging and performance monitoring
        static size_t GetLoadedChunkCount();
        static size_t GetRenderedSectionCount();
        static bool IsChunkLoaded(Math::ChunkPos pos);
        static std::vector<Math::ChunkPos> GetLoadedChunks();

    private:
        // Set of currently loaded chunk coordinates (simplified - no time tracking)
        static std::unordered_set<Math::ChunkPos, ChunkPosHash> s_loaded;

        // Internal helper functions
        static void LoadChunk(Math::ChunkPos pos);
        static void UnloadChunk(Math::ChunkPos pos);
    };

} // namespace Game