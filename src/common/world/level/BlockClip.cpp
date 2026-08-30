// File: src/common/world/level/BlockClip.cpp
#include "common/world/level/BlockClip.hpp"

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cassert>

namespace Game {

    bool SegmentHitsBox(const glm::dvec3& a, const glm::dvec3& d,
                        const glm::dvec3& boxMin, const glm::dvec3& boxMax) {
        double tEnter = 0.0;
        double tExit  = 1.0;
        for (int axis = 0; axis < 3; ++axis) {
            const double da = d[axis];
            if (std::abs(da) < 1.0e-12) {
                // Parallel to this slab: either always inside it or never.
                if (a[axis] < boxMin[axis] || a[axis] > boxMax[axis]) return false;
                continue;
            }
            const double inv = 1.0 / da;
            double t0 = (boxMin[axis] - a[axis]) * inv;
            double t1 = (boxMax[axis] - a[axis]) * inv;
            if (t0 > t1) std::swap(t0, t1);
            tEnter = std::max(tEnter, t0);
            tExit  = std::min(tExit,  t1);
            if (tEnter > tExit) return false;
        }
        return true;
    }

    bool CollisionGrid::AnySetInBox(const glm::ivec3& lo, const glm::ivec3& hi) const {
        // Coarse plane: one bit per 4x4x4 cell block, set if ANY cell in it is
        // collidable. Conservative in the safe direction — a set coarse bit may
        // mean nothing in [lo,hi] is collidable, in which case the caller just
        // does the work it would have done anyway.
        const glm::ivec3 a = (lo - origin) >> 2;
        const glm::ivec3 b = (hi - origin) >> 2;
        for (int y = a.y; y <= b.y; ++y) {
            for (int z = a.z; z <= b.z; ++z) {
                const size_t row = (static_cast<size_t>(y) << coarseShiftY) |
                                   (static_cast<size_t>(z) << coarseShiftZ);
                for (int x = a.x; x <= b.x; ++x) {
                    const size_t i = row | static_cast<size_t>(x);
                    if ((coarse[i >> 6] >> (i & 63)) & 1ull) return true;
                }
            }
        }
        return false;
    }

    bool ClipBlocksCollider(const IBlockAccess& blocks,
                            const glm::dvec3& from, const glm::dvec3& to,
                            glm::ivec3& outHit, bool includeWaterSource,
                            const CollisionGrid* grid) {
        // A grid is only valid for the no-water-source form. Water's
        // hasCollision is force-cleared in the registry, so a water SOURCE
        // carries a CLEAR bit while the walk below must STOP on it — handing a
        // grid to the includeWaterSource path would tunnel straight through
        // water. FallingBlockEntity's swept concrete-powder clip is a live
        // caller of that form.
        assert(!(grid && includeWaterSource));
        return TraverseBlocks(from, to,
            [&](const glm::ivec3& c, const glm::dvec3& a, const glm::dvec3& d) {
                // Bit test first. A clear bit means HasCollision is false for
                // this cell, which is exactly the condition the early-out below
                // tests after paying for a block read. A cell OUTSIDE the grid
                // falls through to the original path verbatim, which makes the
                // grid's box bound a performance question and never a
                // correctness one.
                if (grid && grid->Contains(c) && !grid->Test(c)) return false;

                const BlockState st = blocks.GetBlockState(c.x, c.y, c.z);

                // MC ClipContext.Fluid.SOURCE_ONLY: the fluid shape is a full
                // block for a source and empty otherwise, so a source cell
                // stops the clip without any shape test.
                if (includeWaterSource && BlockRegistry::IsWaterSource(st)) {
                    return true;
                }

                // MC LiquidBlock and AirBlock return Shapes.empty() from
                // getCollisionShape. This engine spells the same fact as
                // hasCollision=false, and checking it first skips the shape
                // fetch for the overwhelmingly common empty cell.
                if (!BlockRegistry::HasCollision(st.Block())) return false;

                const glm::dvec3 cellOrigin(static_cast<double>(c.x),
                                            static_cast<double>(c.y),
                                            static_cast<double>(c.z));
                const auto shape = BlockRegistry::GetBlockCollisionShapeSet(st);
                for (const auto& box : shape) {
                    if (SegmentHitsBox(a, d,
                                       cellOrigin + glm::dvec3(box.min),
                                       cellOrigin + glm::dvec3(box.max))) {
                        return true;
                    }
                }
                return false;
            },
            &outHit);
    }

} // namespace Game
