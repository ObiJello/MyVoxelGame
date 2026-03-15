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
#include <unordered_map>
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
        struct GridNode {
            uint8_t visited;
            uint8_t sourceDirections;
            GPUSectionData* gpuData;
        };
        std::vector<GridNode> m_gridNodes;
        std::vector<bool> m_chunkLoaded;  // Cached chunk loading status

        // Get neighbor in direction
        static void getNeighbor(Game::Math::ChunkPos pos, int sectionY, int dir,
                               Game::Math::ChunkPos& outPos, int& outSY);
    };

} // namespace Render
