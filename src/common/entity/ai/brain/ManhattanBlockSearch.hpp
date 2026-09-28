// File: src/common/entity/ai/brain/ManhattanBlockSearch.hpp
//
// MC BlockPos.withinBoxByManhattanDistance(origin, reachX, reachY, reachZ)
// (BlockPos.manhattanOrdered with maxDepth = reachX + reachY + reachZ) — the
// order LevelReader.findBlocksInBoxByManhattanDistance walks, which is what
// the piglin's and the hoglin's repellent sensors take the FIRST match of.
// Transcribed iterator-for-iterator: depth rings outward from the origin; in
// a ring x runs -maxX..maxX, y -maxY..maxY, and each cell off the z = 0
// plane is visited at +z then at the mirrored -z.
#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace Game {

    // Visit the box's cells in MC's order until `match(pos)` answers true;
    // returns that cell.
    template <typename Match>
    std::optional<glm::ivec3> FindFirstInBoxByManhattanDistance(const glm::ivec3& origin, int reachX,
                                                                int reachY, int reachZ, Match&& match) {
        const int maxDepth = reachX + reachY + reachZ;
        int currentDepth = 0;
        int maxX = 0, maxY = 0;
        int x = 0, y = 0;
        while (true) {
            // One computeNext: advance to the next cell of the ring walk.
            bool found = false;
            glm::ivec3 cell{};
            for (; !found; ++y) {
                if (y > maxY) {
                    ++x;
                    if (x > maxX) {
                        ++currentDepth;
                        if (currentDepth > maxDepth) return std::nullopt;
                        maxX = std::min(reachX, currentDepth);
                        x = -maxX;
                    }
                    maxY = std::min(reachY, currentDepth - std::abs(x));
                    y = -maxY;
                }
                const int zz = currentDepth - std::abs(x) - std::abs(y);
                if (zz <= reachZ) {
                    found = true;
                    cell = origin + glm::ivec3(x, y, zz);
                }
            }
            if (match(cell)) return cell;
            // zMirror: the same cell reflected through the origin's z.
            if (cell.z != origin.z) {
                const glm::ivec3 mirrored(cell.x, cell.y, origin.z - (cell.z - origin.z));
                if (match(mirrored)) return mirrored;
            }
        }
    }

} // namespace Game
