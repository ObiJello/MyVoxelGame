// File: src/server/world/watch/ChunkTrackingView.hpp
//
// Port of net.minecraft.server.level.ChunkTrackingView.
//
// The set of chunks one player tracks, held as a CENTRE AND A RADIUS rather
// than as a materialised container. Two consequences, both of them the point:
//
//   1. It is a CIRCLE (dx^2 + dz^2 < viewDistance^2), not a square. At view
//      distance 16 the tracked set is 1057 chunks against the 1369 a
//      (viewDistance + 2) square covers — ~23% fewer to generate, store,
//      serialise and send, and a correspondingly smaller burst on join.
//      (921 is the stricter isInViewDistance count, buffer 1; the set actually
//      sent goes through Contains() with the neighbour buffer of 2.)
//
//   2. Membership and diffing are pure arithmetic. MC's difference() walks the
//      union bounding box of the two views and asks contains() per cell, so a
//      player stepping across a chunk border allocates nothing at all. The set
//      this replaced rebuilt a 1369-entry unordered_set and ran two full
//      set-difference passes on every update.
//
// MC's includeNeighbors buffer is preserved verbatim: bufferRange 1 for
// "in view distance", 2 for "in view distance, or adjacent to it". The buffer
// is what stops a chunk from being dropped and immediately re-sent as the
// player jitters along a boundary.
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/world/math/ChunkViewDistance.hpp"
#include <algorithm>
#include <cstdlib>

namespace Server {

    struct ChunkTrackingView {
        Game::Math::ChunkPos center{0, 0};
        int viewDistance = 0;
        // MC models "no view yet" as the ChunkTrackingView.EMPTY singleton,
        // whose contains() is always false. A joining player is given EMPTY and
        // then immediately diffed against their real view, so the whole initial
        // set arrives through the same enter/leave path as every later move —
        // there is no separate "first time" code path to keep in sync.
        bool valid = false;

        static ChunkTrackingView Of(Game::Math::ChunkPos c, int viewDistance) {
            return ChunkTrackingView{c, viewDistance, true};
        }
        static ChunkTrackingView Empty() { return ChunkTrackingView{}; }

        // MC ChunkTrackingView.isWithinDistance. The formula lives in
        // common/ because the client needs the identical test to decide what it
        // renders — see ChunkViewDistance.hpp for why the two buffer ranges
        // must stay exactly one apart.
        static bool IsWithinDistance(int centerX, int centerZ, int viewDistance,
                                     int chunkX, int chunkZ, bool includeNeighbors) {
            return Game::Math::IsWithinChunkViewDistance(centerX, centerZ, viewDistance,
                                                         chunkX, chunkZ, includeNeighbors);
        }

        bool Contains(int chunkX, int chunkZ, bool includeNeighbors = true) const {
            if (!valid) return false;
            return IsWithinDistance(center.x, center.z, viewDistance,
                                    chunkX, chunkZ, includeNeighbors);
        }
        bool Contains(Game::Math::ChunkPos pos, bool includeNeighbors = true) const {
            return Contains(pos.x, pos.z, includeNeighbors);
        }

        // MC isInViewDistance: the stricter test, without the neighbour buffer.
        bool IsInViewDistance(int chunkX, int chunkZ) const {
            return Contains(chunkX, chunkZ, false);
        }

        bool SameAs(const ChunkTrackingView& other) const {
            return valid == other.valid && center == other.center &&
                   viewDistance == other.viewDistance;
        }

        // Bounding box of the circle, inclusive. MC Positioned.minX/maxX/etc.
        int MinX() const { return center.x - viewDistance - 1; }
        int MinZ() const { return center.z - viewDistance - 1; }
        int MaxX() const { return center.x + viewDistance + 1; }
        int MaxZ() const { return center.z + viewDistance + 1; }

        bool SquareIntersects(const ChunkTrackingView& other) const {
            return MinX() <= other.MaxX() && MaxX() >= other.MinX() &&
                   MinZ() <= other.MaxZ() && MaxZ() >= other.MinZ();
        }

        // MC Positioned.forEach.
        template <class Consumer>
        void ForEach(Consumer consumer) const {
            if (!valid) return;
            for (int x = MinX(); x <= MaxX(); ++x) {
                for (int z = MinZ(); z <= MaxZ(); ++z) {
                    if (Contains(x, z)) consumer(Game::Math::ChunkPos{x, z});
                }
            }
        }

        // MC ChunkTrackingView.difference.
        //
        // When the two views overlap, only the union bounding box is walked and
        // each cell is classified by comparing membership in both — no sets, no
        // allocation. When they do not overlap (a teleport, a dimension change)
        // there is nothing to salvage, so everything leaves and everything
        // enters, exactly as MC falls back to.
        template <class OnEnter, class OnLeave>
        static void Difference(const ChunkTrackingView& from, const ChunkTrackingView& to,
                               OnEnter onEnter, OnLeave onLeave) {
            if (from.SameAs(to)) return;

            if (from.valid && to.valid && from.SquareIntersects(to)) {
                const int minX = std::min(from.MinX(), to.MinX());
                const int minZ = std::min(from.MinZ(), to.MinZ());
                const int maxX = std::max(from.MaxX(), to.MaxX());
                const int maxZ = std::max(from.MaxZ(), to.MaxZ());

                for (int x = minX; x <= maxX; ++x) {
                    for (int z = minZ; z <= maxZ; ++z) {
                        const bool saw = from.Contains(x, z);
                        const bool sees = to.Contains(x, z);
                        if (saw == sees) continue;
                        if (sees) onEnter(Game::Math::ChunkPos{x, z});
                        else      onLeave(Game::Math::ChunkPos{x, z});
                    }
                }
                return;
            }

            from.ForEach(onLeave);
            to.ForEach(onEnter);
        }
    };

} // namespace Server
