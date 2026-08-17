// File: src/common/world/math/ChunkViewDistance.hpp
//
// MC ChunkTrackingView.isWithinDistance — the one formula that decides which
// chunks a player tracks, shared by both sides because both sides must agree.
//
// The `includeNeighbors` flag is not a convenience. MC uses the two settings
// for two different questions, and the gap between them is load-bearing:
//
//   bufferRange 1 (isInViewDistance) — what the client RENDERS
//   bufferRange 2 (contains)         — what the server SENDS
//
// Because a neighbour is at most one chunk away on each axis, widening the
// buffer by exactly one guarantees that every chunk in the rendered set has all
// eight of its neighbours in the sent set. That is what makes MC's
// hasAllNeighbors() gate safe: a section admitted for its first compile can
// always eventually satisfy it.
//
// Get this wrong — render a square while sending a circle, say — and the outer
// ring of renderable chunks has neighbours that are never sent, so their
// sections fail hasAllNeighbors() forever and stay unmeshed. Measured at view
// distance 16 that was 76 permanently empty chunks ringing the horizon.
#pragma once

#include <algorithm>
#include <cstdlib>

namespace Game::Math {

    inline bool IsWithinChunkViewDistance(int centerX, int centerZ, int viewDistance,
                                          int chunkX, int chunkZ, bool includeNeighbors) {
        const int bufferRange = includeNeighbors ? 2 : 1;
        const long dx = std::max(0, std::abs(chunkX - centerX) - bufferRange);
        const long dz = std::max(0, std::abs(chunkZ - centerZ) - bufferRange);
        return dx * dx + dz * dz <
               static_cast<long>(viewDistance) * static_cast<long>(viewDistance);
    }

} // namespace Game::Math
