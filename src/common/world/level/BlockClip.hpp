// File: src/common/world/level/BlockClip.hpp
//
// MC BlockGetter.traverseBlocks + BlockGetter.clip — the exact voxel walk
// (Amanatides–Woo) and the collider clip built on top of it.
//
// This lives on its own because three unrelated callers need the SAME walk and
// a near-miss in any of them is a behaviour bug that is very hard to see:
//
//   * Explosion exposure (ExplosionSeenPercent) — a ray that misses one cell
//     reports open sky through a wall, which is how TNT used to knock a player
//     about through an obsidian corner seam;
//   * FallingBlockEntity's concrete-powder swept test — powder moving more
//     than a block per tick must still notice the thin sheet of water it flew
//     through;
//   * anything later that needs MC's clip (fluid pushing, projectile ray).
//
// The traversal is a header template rather than a std::function callback on
// purpose: the explosion path runs it 1352 times per blast and an indirect
// call per visited cell shows up.
#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <limits>

namespace Game {

    struct IBlockAccess;

    // MC AABB.clip reduced to the boolean the callers actually need: does the
    // segment a + t*d, t in [0,1], touch this box at all? The slab method,
    // which is what AABB.clip is underneath — it finds the nearest entry point
    // and we only care whether one exists.
    bool SegmentHitsBox(const glm::dvec3& a, const glm::dvec3& d,
                        const glm::dvec3& boxMin, const glm::dvec3& boxMax);

    // MC BlockGetter.traverseBlocks(from, to, …).
    //
    // Visits every cell the segment passes through, in order, starting with
    // the cell containing `from`. Stops and returns true the first time `stop`
    // returns true, writing that cell to `outHit` when non-null; returns false
    // if the segment reaches `to` without stopping.
    //
    // Both endpoints are nudged a hair OUTWARD along the segment before
    // flooring (MC's Mth.lerp(-1.0E-7, …)), so a ray that starts or ends
    // exactly on a cell boundary resolves consistently instead of by
    // floating-point luck.
    template <typename StopFn>
    bool TraverseBlocks(const glm::dvec3& from, const glm::dvec3& to,
                        StopFn&& stop, glm::ivec3* outHit = nullptr) {
        constexpr double kNudge = 1.0e-7;
        const glm::dvec3 a = from - (to - from) * kNudge;
        const glm::dvec3 b = to   - (from - to) * kNudge;

        const glm::dvec3 d = b - a;
        if (glm::dot(d, d) < 1.0e-14) return false;

        glm::ivec3 cell(static_cast<int>(std::floor(a.x)),
                        static_cast<int>(std::floor(a.y)),
                        static_cast<int>(std::floor(a.z)));

        // MC tests the starting cell too — standing inside a block counts.
        if (stop(cell, a, d)) {
            if (outHit) *outHit = cell;
            return true;
        }

        const auto sign = [](double v) { return v > 0.0 ? 1 : (v < 0.0 ? -1 : 0); };
        const glm::ivec3 stepDir(sign(d.x), sign(d.y), sign(d.z));

        const auto frac = [](double v) { return v - std::floor(v); };
        const double kInf = std::numeric_limits<double>::max();

        // tDelta: how far along the segment one whole cell costs per axis.
        // tMax: how far to the FIRST boundary crossing on each axis.
        const glm::dvec3 tDelta(
            stepDir.x == 0 ? kInf : static_cast<double>(stepDir.x) / d.x,
            stepDir.y == 0 ? kInf : static_cast<double>(stepDir.y) / d.y,
            stepDir.z == 0 ? kInf : static_cast<double>(stepDir.z) / d.z);
        glm::dvec3 tMax(
            stepDir.x == 0 ? kInf
                : tDelta.x * (stepDir.x > 0 ? 1.0 - frac(a.x) : frac(a.x)),
            stepDir.y == 0 ? kInf
                : tDelta.y * (stepDir.y > 0 ? 1.0 - frac(a.y) : frac(a.y)),
            stepDir.z == 0 ? kInf
                : tDelta.z * (stepDir.z > 0 ? 1.0 - frac(a.z) : frac(a.z)));

        while (tMax.x <= 1.0 || tMax.y <= 1.0 || tMax.z <= 1.0) {
            if (tMax.x < tMax.y) {
                if (tMax.x < tMax.z) { cell.x += stepDir.x; tMax.x += tDelta.x; }
                else                 { cell.z += stepDir.z; tMax.z += tDelta.z; }
            } else if (tMax.y < tMax.z) {
                cell.y += stepDir.y; tMax.y += tDelta.y;
            } else {
                cell.z += stepDir.z; tMax.z += tDelta.z;
            }
            if (stop(cell, a, d)) {
                if (outHit) *outHit = cell;
                return true;
            }
        }
        return false;
    }

    // MC Level.clip(new ClipContext(from, to, Block.COLLIDER, Fluid.X, entity)).
    //
    // Stops at the first cell whose COLLISION SHAPE the segment actually
    // intersects — not merely the first cell that contains a block, which is
    // what makes a slab, a fence arm or a wall gap clip the way vanilla's
    // does. When `includeWaterSource` is set the walk also stops on a water
    // SOURCE cell (MC ClipContext.Fluid.SOURCE_ONLY, whose shape is a full
    // block); flowing water never stops it.
    //
    // Returns true and writes the cell to `outHit` on a hit; false on a miss
    // (MC's HitResult.Type.MISS).
    // A precomputed "does this cell have a collision shape at all" bitset over
    // one axis-aligned box, so a clip walk can answer the overwhelmingly common
    // empty cell with a bit test instead of a block-accessor read.
    //
    // The bit IS BlockRegistry::HasCollision(GetBlockState(c).Block()) — the
    // same expression the walk evaluates first today — so a CLEAR bit skips a
    // cell the walk would have skipped anyway, and a SET bit falls through to
    // the unchanged shape path. Nothing about shape behaviour moves.
    //
    // Index arithmetic uses power-of-two strides so a cell address is shifts
    // and ors, but Contains() bounds-tests the TRUE extents, never the padded
    // stride. Testing against the pad would read bits that were never
    // populated: zero-filled they say "not collidable", a real occluder stops
    // occluding, and the damage it changes feeds the shared level RNG stream.
    struct CollisionGrid {
        glm::ivec3      origin{0};      // world coords of cell (0,0,0)
        glm::ivec3      size{0};        // true extents, in cells
        int             shiftZ = 0;     // log2 of the padded x stride
        int             shiftY = 0;     // log2 of the padded x*z stride
        const uint64_t* words  = nullptr;
        const uint64_t* coarse = nullptr;   // 1 bit per 4x4x4, for the hull early-out
        glm::ivec3      coarseSize{0};
        int             coarseShiftZ = 0;
        int             coarseShiftY = 0;
        uint64_t        writeEpoch = 0;     // World block-write epoch at build

        bool Contains(const glm::ivec3& c) const {
            const glm::ivec3 d = c - origin;
            return static_cast<unsigned>(d.x) < static_cast<unsigned>(size.x) &&
                   static_cast<unsigned>(d.y) < static_cast<unsigned>(size.y) &&
                   static_cast<unsigned>(d.z) < static_cast<unsigned>(size.z);
        }
        // Precondition: Contains(c).
        bool Test(const glm::ivec3& c) const {
            const glm::ivec3 d = c - origin;
            const size_t i = (static_cast<size_t>(d.y) << shiftY) |
                             (static_cast<size_t>(d.z) << shiftZ) |
                             static_cast<size_t>(d.x);
            return (words[i >> 6] >> (i & 63)) & 1ull;
        }
        // True if any collidable cell lies in the inclusive world-cell box
        // [lo, hi]. Precondition: the box lies entirely inside the true extent.
        bool AnySetInBox(const glm::ivec3& lo, const glm::ivec3& hi) const;
    };

    bool ClipBlocksCollider(const IBlockAccess& blocks,
                            const glm::dvec3& from, const glm::dvec3& to,
                            glm::ivec3& outHit, bool includeWaterSource = false,
                            const CollisionGrid* grid = nullptr);

} // namespace Game
