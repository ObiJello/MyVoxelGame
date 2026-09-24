// File: src/server/portal/SurfaceGate.cpp
#include "SurfaceGate.hpp"

#include "common/core/Log.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/level/World.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <vector>

namespace Server::SurfaceGate {

    namespace {

        using Game::Immersive::FrameShape;

        // The ground a gate stands on: the cells one below the frame's bottom
        // row, and one below the lowest clearance cells (where a traveller
        // steps out), as x/z columns relative to minCell.
        struct Footprint {
            std::vector<glm::ivec2> columns;   // relative to (minCell.x, minCell.z)
        };

        Footprint FootprintOf(const FrameShape& shape) {
            Footprint fp;
            const int bottomFrameY = shape.minCell.y - 1;
            auto add = [&](const glm::ivec3& c) {
                const glm::ivec2 rel(c.x - shape.minCell.x, c.z - shape.minCell.z);
                if (std::find(fp.columns.begin(), fp.columns.end(), rel) == fp.columns.end()) {
                    fp.columns.push_back(rel);
                }
            };
            for (const glm::ivec3& c : shape.frame) {
                if (c.y == bottomFrameY) add(c);
            }
            for (const glm::ivec3& c : shape.Clearance()) {
                if (c.y == shape.minCell.y) add(c);
            }
            return fp;
        }

        // Topmost motion-blocking block (not leaves) of a column: the ground
        // a gate may rest on. GetSurfaceHeight answers the topmost matching
        // block itself, so the first air above is +1.
        int GroundTop(const Game::World& world, int x, int z) {
            return world.GetSurfaceHeight(x, z, Game::HeightmapType::MotionBlockingNoLeaves);
        }

    } // namespace

    std::optional<glm::ivec3> Plan(const Game::World& world, const FrameShape& shape,
                                   const glm::ivec3& around, int radius) {
        if (shape.axis == Game::Axis::Y || shape.area.empty()) return std::nullopt;

        const Footprint fp = FootprintOf(shape);
        const glm::ivec3 half = (shape.maxCell - shape.minCell) / 2;
        const glm::ivec2 size = shape.Size();
        const int dimMin = Game::DimensionMinY(world.GetDimension());
        const int dimTop = dimMin + Game::DimensionLogicalHeight(world.GetDimension());

        bool   found = false;
        double bestCost = 0.0;
        glm::ivec3 best{0};

        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                const int baseX = around.x + dx - half.x;
                const int baseZ = around.z + dz - half.z;
                int hMax = INT_MIN, hMin = INT_MAX;
                bool loaded = true, water = false;
                for (const glm::ivec2& rel : fp.columns) {
                    const int x = baseX + rel.x, z = baseZ + rel.y;
                    if (!world.IsChunkLoaded(x >> 4, z >> 4)) { loaded = false; break; }
                    const int h = GroundTop(world, x, z);
                    if (world.GetBlock(x, h, z) == Game::BlockID::Water) water = true;
                    hMax = std::max(hMax, h);
                    hMin = std::min(hMin, h);
                }
                if (!loaded || hMax == INT_MIN) continue;
                // The frame's bottom row replaces the highest ground block;
                // the opening and its top must fit under the build limit.
                if (hMax + size.y + 2 >= dimTop || hMax <= dimMin + 1) continue;

                // Unevenness dominates (every block of dip is a block of
                // plinth); distance breaks ties; standing in a lake is last.
                const double cost = (hMax - hMin) * 4.0
                                  + std::sqrt(static_cast<double>(dx * dx + dz * dz))
                                  + (water ? 40.0 : 0.0);
                if (!found || cost < bestCost) {
                    found = true;
                    bestCost = cost;
                    best = glm::ivec3(baseX, hMax + 1, baseZ);
                }
            }
        }
        if (!found) return std::nullopt;
        return best;
    }

    void BuildPlinth(Game::World& world, const FrameShape& built) {
        if (built.axis == Game::Axis::Y) return;
        constexpr int kMaxDepth = 48;
        const Game::BlockID plinth = Game::BlockID::HushstoneBricks;
        const int dimMin = Game::DimensionMinY(world.GetDimension());

        auto fillDown = [&](int x, int yTop, int z) {
            for (int y = yTop, n = 0; y > dimMin && n < kMaxDepth; --y, ++n) {
                if (world.IsBlockSolid(x, y, z)) break;
                world.SetBlock(x, y, z, plinth);
            }
        };

        const int bottomFrameY = built.minCell.y - 1;
        for (const glm::ivec3& c : built.frame) {
            if (c.y == bottomFrameY) fillDown(c.x, c.y - 1, c.z);
        }
        // The two corner cells of the bottom row (FrameWithCorners has them;
        // `frame` does not).
        for (const glm::ivec3& c : built.FrameWithCorners()) {
            if (c.y == bottomFrameY) fillDown(c.x, c.y - 1, c.z);
        }
        // A walkway on both faces at the frame's bottom row, so a traveller
        // steps out onto something rather than into a drop.
        for (const glm::ivec3& c : built.Clearance()) {
            if (c.y != built.minCell.y) continue;
            if (!world.IsBlockSolid(c.x, bottomFrameY, c.z)) {
                world.SetBlock(c.x, bottomFrameY, c.z, plinth);
            }
            fillDown(c.x, bottomFrameY - 1, c.z);
        }
        Log::Info("[SurfaceGate] Stood a %dx%d frame on the surface at (%d,%d,%d)",
                  built.Size().x, built.Size().y, built.minCell.x, built.minCell.y, built.minCell.z);
    }

} // namespace Server::SurfaceGate
