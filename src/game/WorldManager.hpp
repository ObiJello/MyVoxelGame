#pragma once
#include "WorldMath.hpp"
#include <unordered_set>

namespace Game {

    struct ChunkPosHash {
        size_t operator()(ChunkPos const &p) const noexcept {
            return std::hash<int>()(p.x) ^ (std::hash<int>()(p.z) << 1);
        }
    };

    class WorldManager {
    public:
        static constexpr int RENDER_RADIUS = 4; // change as you like

        // Call once per frame with camera world‐pos
        static void Update(const glm::vec3 &cameraPos);

    private:
        // which chunk‐coords are currently loaded
        static std::unordered_set<ChunkPos,ChunkPosHash> s_loaded;

        // Helpers:
        static void LoadChunk(ChunkPos p);
        static void UnloadChunk(ChunkPos p);
    };

} // namespace Game