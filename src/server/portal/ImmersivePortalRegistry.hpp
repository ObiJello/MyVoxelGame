// File: src/server/portal/ImmersivePortalRegistry.hpp
//
// Server-authoritative store of every immersive portal in every dimension.
//
// ONE registry for the whole server, not one per ServerLevel, for two
// reasons that both come from portals being the thing that CONNECTS levels:
//   • ids must be unique across dimensions, because a client will hold
//     portals from several at once and keys them by id alone;
//   • a portal's cluster partners (its reverse, its flipped twin) live in
//     the other dimension, and a per-level store would need a cross-level
//     lookup for every link anyway.
// The per-dimension question — "which portals are in this chunk" — is a
// secondary index keyed by (dimension, chunk).
//
// WHAT IT DOES
//   • Assigns ids, stores records, keeps the (dimension, chunk) index.
//   • Tells clients: an add/update reaches every player who has been SENT
//     the origin chunk; a removal likewise; and when a chunk is sent to a
//     player, the portals anchored in it follow (SyncChunkToClient, called
//     from IntegratedServer::OnChunkSentToClient). That is the whole
//     visibility rule, and it matches how the mod ties portal entities to
//     chunk watch records.
//   • Persists to <save>/data/immersive_portals.json — JSON rather than the
//     wire format so a world's portals can be read and edited by hand.
//
// WHAT IT DOES NOT DO (later phases)
//   • Decide crossings, load the far side's chunks, or render anything.
//   • Break nether portals when their frame breaks (phase 6 owns that).
//
// THREADING: server thread only, like every other world-state container.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/portal/ImmersivePortal.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace Server {

    class IntegratedServer;
    class ServerConnection;

    class ImmersivePortalRegistry {
    public:
        using Portal   = Game::Immersive::Portal;
        using PortalId = Game::Immersive::PortalId;

        explicit ImmersivePortalRegistry(IntegratedServer& server);

        // ── Mutation ────────────────────────────────────────────────────
        // Store a portal, assigning an id if it has none, and tell every
        // client that holds its chunk. Returns the id, or kInvalidPortalId
        // when the geometry is unusable (logged).
        PortalId Add(Portal portal);

        // Replace the record with this id in full. False if unknown or invalid.
        bool Update(const Portal& portal);

        // Drop one record and tell its chunk's clients. Cluster partners are
        // left alone — see RemoveCluster.
        bool Remove(PortalId id);

        // Drop a portal and every partner reachable through reverse/flipped/
        // parallel links (transitively, so all four faces of a two-way
        // portal go together). Returns how many were removed.
        size_t RemoveCluster(PortalId id);

        // Build the two-way, two-faced cluster the mod makes for a nether
        // portal — `front` plus its flipped twin, the reverse at the
        // destination, and that reverse's flipped twin — link all four and
        // store them. Returns the id of `front`.
        PortalId AddBiWayBiFaced(Portal front);

        // Build a two-way single-faced pair: `front` and its reverse.
        PortalId AddBiWay(Portal front);

        // ── Query ───────────────────────────────────────────────────────
        const Portal* Get(PortalId id) const;
        size_t Count() const { return m_portals.size(); }

        void ForEach(const std::function<void(const Portal&)>& fn) const;
        void ForEachInDimension(Game::DimensionId dimension,
                                const std::function<void(const Portal&)>& fn) const;
        void ForEachInChunk(Game::DimensionId dimension, Game::Math::ChunkPos chunk,
                            const std::function<void(const Portal&)>& fn) const;

        // Portals in `dimension` whose surface bounding box comes within
        // `radius` of `pos`. Cheap enough for per-tick use by the teleport
        // and collision checks: it walks the chunks the radius covers.
        std::vector<const Portal*> CollectNear(Game::DimensionId dimension,
                                               const glm::dvec3& pos, double radius) const;

        // ── Client sync ─────────────────────────────────────────────────
        // Send every portal anchored in `chunk` to one connection. Called
        // right after that chunk's data goes out, so the client always has
        // the terrain a portal sits in before the portal.
        // The dimension's global (chunkless) surfaces, sent when a client first
        // holds the dimension (IntegratedServer::OnChunkSentToClient).
        void SyncGlobalsToClient(ServerConnection& connection, Game::DimensionId dimension) const;
        void SyncChunkToClient(ServerConnection& connection, Game::DimensionId dimension,
                               Game::Math::ChunkPos chunk) const;

        // ── Persistence ─────────────────────────────────────────────────
        // Both are no-ops for a world with no save path. Save only writes
        // when something changed since the last save.
        bool Load();
        bool Save();
        bool IsDirty() const { return m_dirty; }

    private:
        struct ChunkKey {
            int8_t  dimension;
            int32_t x;
            int32_t z;
            bool operator==(const ChunkKey& o) const {
                return dimension == o.dimension && x == o.x && z == o.z;
            }
            bool operator!=(const ChunkKey& o) const { return !(*this == o); }
        };
        struct ChunkKeyHash {
            size_t operator()(const ChunkKey& k) const noexcept {
                // Same finalizer as ChunkPosHash, with the dimension folded
                // in so Overworld (0,0) and Nether (0,0) never collide.
                uint64_t h = (static_cast<uint64_t>(static_cast<uint32_t>(k.x)) << 32) |
                             static_cast<uint32_t>(k.z);
                h ^= static_cast<uint64_t>(static_cast<uint8_t>(k.dimension)) << 61;
                h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
                h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
                h ^= h >> 33;
                return static_cast<size_t>(h);
            }
        };

        static ChunkKey KeyOf(const Portal& p);
        static ChunkKey KeyOf(Game::DimensionId dimension, Game::Math::ChunkPos chunk);

        void Index(const Portal& portal);
        void Unindex(const Portal& portal);

        // To every session that has been SENT the portal's origin chunk.
        void BroadcastSync(const Portal& portal) const;
        void BroadcastRemove(const Portal& portal) const;

        std::string SavePath() const;

        IntegratedServer& m_server;
        PortalId          m_nextId = 1;
        bool              m_dirty  = false;

        std::unordered_map<PortalId, Portal> m_portals;
        std::unordered_map<ChunkKey, std::vector<PortalId>, ChunkKeyHash> m_byChunk;
    };

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
