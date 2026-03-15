// File: src/client/renderer/culling/SectionOcclusionGraph.hpp
// BFS-based occlusion culling that determines which sections are visible from
// the player's position by propagating through non-solid terrain faces.
// Mirrors Minecraft's SectionOcclusionGraph — sections behind solid terrain
// are never added to the render list, typically eliminating 60-70% of sections.
#pragma once

#include "VisibilitySet.hpp"
#include "../core/Frustum.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/core/Config.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace Render {

    struct SectionRenderData;
    struct GPUSectionData;

    namespace ClientMeshManagerAccess {
        // Forward declaration for the lookup function (defined in SectionOcclusionGraph.cpp)
        GPUSectionData* GetGPUData(Game::Math::ChunkPos chunkPos, int sectionY);
        bool IsChunkLoaded(Game::Math::ChunkPos chunkPos);
        int GetRenderDistance();
    }

    class SectionOcclusionGraph {
    public:
        // Mark that a full BFS rebuild is needed (view distance changed, world reload, etc.)
        void invalidate() { m_needsFullUpdate = true; }

        // Run BFS from the player's section and populate outVisible with reachable sections.
        // smartCull: if true, use VisibilitySet to occlude; if false, skip occlusion (debug).
        void update(const glm::vec3& cameraPos, const Frustum& frustum,
                    bool smartCull, int renderDistance,
                    std::vector<SectionRenderData>& outVisible);

        // Stats
        int getLastVisitedCount() const { return m_lastVisitedCount; }
        int getLastOccludedCount() const { return m_lastOccludedCount; }
        int getLastRenderedCount() const { return m_lastRenderedCount; }

    private:
        bool m_needsFullUpdate = true;

        // Stats from last update
        int m_lastVisitedCount = 0;
        int m_lastOccludedCount = 0;
        int m_lastRenderedCount = 0;

        // Flat 3D grid for BFS visited set — avoids hash map overhead.
        // Persists between frames to avoid re-allocation.
        // Uses generation counter to avoid zeroing the entire grid each frame.
        struct GridNode {
            uint32_t generation;        // Compared against m_currentGeneration
            uint8_t sourceDirections;
            GPUSectionData* gpuData;
        };
        std::vector<GridNode> m_gridNodes;
        uint32_t m_currentGeneration = 0;  // Bumped each update — nodes with stale generation are "unvisited"

        // Cached chunk loading status (lazy: uses generation to avoid full pre-scan)
        struct ChunkCacheEntry {
            uint32_t generation;
            bool loaded;
        };
        std::vector<ChunkCacheEntry> m_chunkLoaded;

        // Double-buffered BFS queues (cache-friendly alternative to std::queue/deque)
        struct QueueEntry {
            int16_t rx, rz;
            int8_t sy;
            uint8_t sourceDirections;
        };
        std::vector<QueueEntry> m_bfsCurrentQueue;
        std::vector<QueueEntry> m_bfsNextQueue;

        // Get neighbor in direction
        static void getNeighbor(Game::Math::ChunkPos pos, int sectionY, int dir,
                               Game::Math::ChunkPos& outPos, int& outSY);
    };

} // namespace Render
