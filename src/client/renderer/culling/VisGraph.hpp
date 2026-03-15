// File: src/client/renderer/culling/VisGraph.hpp
// Flood-fill algorithm on a 16x16x16 voxel grid to determine which pairs of
// section faces can see through each other via connected air blocks.
// Mirrors Minecraft's VisGraph — used during mesh compilation on worker threads.
#pragma once

#include "VisibilitySet.hpp"
#include <bitset>
#include <array>

namespace Render {

    class VisGraph {
    public:
        // Mark a block position as opaque (coordinates 0-15 in each axis)
        void setOpaque(int x, int y, int z);

        // Run flood-fill from all boundary air voxels and build a VisibilitySet
        // encoding which direction pairs can see through this section.
        // This modifies internal state (marks visited voxels) — call only once.
        VisibilitySet resolve();

    private:
        // 16^3 = 4096 voxels. Bit is set if voxel is opaque OR already visited.
        std::bitset<4096> m_filled;
        int m_opaqueCount = 0;

        // Index encoding: x | (z << 4) | (y << 8) — 12-bit packed coordinates
        static int getIndex(int x, int y, int z) { return x | (z << 4) | (y << 8); }

        // Extract coordinates from packed index
        static int getX(int idx) { return idx & 15; }
        static int getZ(int idx) { return (idx >> 4) & 15; }
        static int getY(int idx) { return (idx >> 8) & 15; }

        // BFS flood-fill from a boundary air voxel.
        // Returns bitmask of which faces (directions) were reached.
        uint8_t floodFill(int startIndex);

        // Get neighbor index in direction, or -1 if at boundary
        static int getNeighbor(int index, int direction);

        // Check which faces a voxel touches and add to mask
        static uint8_t getFaceMask(int index);

        // Pre-computed indices of all 1352 boundary voxels (faces of the 16x16x16 cube)
        static const std::array<int, 1352>& getEdgeIndices();
    };

} // namespace Render
