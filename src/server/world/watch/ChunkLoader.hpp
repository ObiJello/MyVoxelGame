// File: src/server/world/watch/ChunkLoader.hpp
//
// A chunk loader is one reason a player wants chunks: a circle of them in
// one dimension. Every player has one for where they stand (their view
// distance), and — with immersive portals — one more for the far side of
// every portal they might look through, centred on the portal's destination
// and shrinking with the player's distance from the portal. This is the
// Immersive Portals mod's `ChunkLoader`, keyed the same way.
//
// A session's watched set is the union of its loaders' views. Everything
// that used to ask "is chunk X in the player's tracking view" now asks "is
// (dimension, X) in the union" — see PlayerSession::IsWatching.
#pragma once

#include "ChunkTrackingView.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"

#include <cstdint>
#include <functional>

namespace Server {

    // (dimension, chunk) — the key a client-visible chunk actually has.
    struct DimChunkKey {
        int8_t  dimension;
        int32_t x;
        int32_t z;

        static DimChunkKey Of(Game::DimensionId d, Game::Math::ChunkPos pos) {
            return DimChunkKey{ static_cast<int8_t>(Game::DimensionToRaw(d)), pos.x, pos.z };
        }
        Game::DimensionId    Dimension() const { return Game::DimensionFromRaw(dimension); }
        Game::Math::ChunkPos Pos()       const { return Game::Math::ChunkPos{ x, z }; }

        bool operator==(const DimChunkKey& o) const {
            return dimension == o.dimension && x == o.x && z == o.z;
        }
        bool operator!=(const DimChunkKey& o) const { return !(*this == o); }
    };

    struct DimChunkKeyHash {
        size_t operator()(const DimChunkKey& k) const noexcept {
            uint64_t h = (static_cast<uint64_t>(static_cast<uint32_t>(k.x)) << 32) |
                         static_cast<uint32_t>(k.z);
            h ^= static_cast<uint64_t>(static_cast<uint8_t>(k.dimension)) << 61;
            h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return static_cast<size_t>(h);
        }
    };

    struct ChunkLoader {
        enum class Source : uint8_t {
            Player,          // the player's own view distance
            Portal,          // the far side of a portal the player is near
            IndirectPortal,  // the far side of a portal visible THROUGH such a portal
        };

        Game::DimensionId dimension = Game::DimensionId::Overworld;
        ChunkTrackingView view      = ChunkTrackingView::Empty();
        Source            source    = Source::Player;

        bool SameAs(const ChunkLoader& o) const {
            return dimension == o.dimension && source == o.source && view.SameAs(o.view);
        }
        bool Contains(Game::DimensionId d, Game::Math::ChunkPos pos) const {
            return d == dimension && view.Contains(pos);
        }
        int Radius() const { return view.viewDistance; }
        Game::Math::ChunkPos Center() const { return view.center; }
    };

} // namespace Server
