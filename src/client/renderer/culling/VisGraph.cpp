// File: src/client/renderer/culling/VisGraph.cpp
#include "VisGraph.hpp"
#include <queue>

namespace Render {

    void VisGraph::setOpaque(int x, int y, int z) {
        int idx = getIndex(x, y, z);
        if (!m_filled.test(idx)) {
            m_filled.set(idx);
            m_opaqueCount++;
        }
    }

    VisibilitySet VisGraph::resolve() {
        VisibilitySet vis;

        // Fast path: mostly air (< 256 opaque out of 4096 = < 6.25% filled)
        // Minecraft uses this threshold — nearly-empty sections are all-visible
        if (m_opaqueCount < 256) {
            vis.setAll(true);
            return vis;
        }

        // Fast path: completely solid — nothing can see through
        if (m_opaqueCount == 4096) {
            return vis;  // All zeros
        }

        // Flood-fill from each unvisited boundary air voxel
        const auto& edges = getEdgeIndices();
        for (int edgeIdx : edges) {
            if (!m_filled.test(edgeIdx)) {
                // This boundary voxel is air and unvisited — flood-fill from it
                uint8_t reachedFaces = floodFill(edgeIdx);
                // All pairs of reached faces can see through each other
                vis.addFaces(reachedFaces);
            }
        }

        return vis;
    }

    uint8_t VisGraph::floodFill(int startIndex) {
        uint8_t faceMask = 0;
        std::queue<int> queue;

        m_filled.set(startIndex);
        queue.push(startIndex);

        while (!queue.empty()) {
            int idx = queue.front();
            queue.pop();

            // Check if this voxel is on any boundary face
            faceMask |= getFaceMask(idx);

            // Explore 6 neighbors
            for (int dir = 0; dir < 6; dir++) {
                int neighbor = getNeighbor(idx, dir);
                if (neighbor >= 0 && !m_filled.test(neighbor)) {
                    m_filled.set(neighbor);
                    queue.push(neighbor);
                }
            }
        }

        return faceMask;
    }

    int VisGraph::getNeighbor(int index, int direction) {
        // Direction: 0=Down(-Y), 1=Up(+Y), 2=North(-Z), 3=South(+Z), 4=West(-X), 5=East(+X)
        switch (direction) {
            case 0: return (getY(index) == 0)  ? -1 : index - 256;  // Down: y-1
            case 1: return (getY(index) == 15) ? -1 : index + 256;  // Up: y+1
            case 2: return (getZ(index) == 0)  ? -1 : index - 16;   // North: z-1
            case 3: return (getZ(index) == 15) ? -1 : index + 16;   // South: z+1
            case 4: return (getX(index) == 0)  ? -1 : index - 1;    // West: x-1
            case 5: return (getX(index) == 15) ? -1 : index + 1;    // East: x+1
            default: return -1;
        }
    }

    uint8_t VisGraph::getFaceMask(int index) {
        uint8_t mask = 0;
        int x = getX(index), y = getY(index), z = getZ(index);
        if (y == 0)  mask |= (1 << Direction::Down);
        if (y == 15) mask |= (1 << Direction::Up);
        if (z == 0)  mask |= (1 << Direction::North);
        if (z == 15) mask |= (1 << Direction::South);
        if (x == 0)  mask |= (1 << Direction::West);
        if (x == 15) mask |= (1 << Direction::East);
        return mask;
    }

    const std::array<int, 1352>& VisGraph::getEdgeIndices() {
        // Pre-compute all boundary voxel indices (voxels where x, y, or z is 0 or 15)
        // 16^3 = 4096 total, interior = 14^3 = 2744, boundary = 4096 - 2744 = 1352
        static std::array<int, 1352> indices = []() {
            std::array<int, 1352> result{};
            int count = 0;
            for (int y = 0; y < 16; y++) {
                for (int z = 0; z < 16; z++) {
                    for (int x = 0; x < 16; x++) {
                        if (x == 0 || x == 15 || y == 0 || y == 15 || z == 0 || z == 15) {
                            result[count++] = getIndex(x, y, z);
                        }
                    }
                }
            }
            return result;
        }();
        return indices;
    }

} // namespace Render
