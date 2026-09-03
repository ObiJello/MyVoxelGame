// File: src/client/portal/ClientImmersivePortals.hpp
//
// The client's copy of every immersive portal the server has told it about.
//
// Deliberately dumb, like ClientPortalManager: it stores what
// ImmersivePortalSyncS2C says and forgets what ImmersivePortalRemoveS2C
// names. Every decision — where a portal may go, what it links to, who may
// cross — is the server's. The renderer, the teleport prediction and the
// collision hooks READ from here; only the packet handler and the chunk
// manager WRITE.
//
// Lifetime rule: a portal lives in its ORIGIN chunk. The server sends it with
// that chunk, and ClientChunkManager calls OnChunkUnloaded when the chunk
// leaves the client, so a portal never outlives the terrain it is anchored
// to. Until the client holds more than one dimension (phase 2) the store is
// keyed by chunk position alone, exactly like the chunk manager it mirrors.
//
// Threading: main thread only. Packets are applied on the main thread by
// ClientConnection::DrainIncomingPackets; the renderer reads on the main
// thread. Nothing here is touched by a worker.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/portal/ImmersivePortal.hpp"
#include "common/world/math/WorldMath.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace Client {

    class ClientImmersivePortals {
    public:
        using Portal   = Game::Immersive::Portal;
        using PortalId = Game::Immersive::PortalId;

        // Create or replace by id. Returns true if anything changed.
        bool OnSync(const Portal& portal);

        // Drop by id. No-op when unknown (a remove can race a chunk unload
        // that already took the portal with it).
        void OnRemove(PortalId id);

        // The chunk left the client; every portal anchored in it goes too.
        void OnChunkUnloaded(Game::Math::ChunkPos chunk);

        // Everything (disconnect, dimension change through ClearAllChunks).
        void Clear();

        const Portal* Get(PortalId id) const;

        template<typename F>
        void ForEach(F&& fn) const {
            for (const auto& [id, portal] : m_portals) fn(portal);
        }

        template<typename F>
        void ForEachInChunk(Game::Math::ChunkPos chunk, F&& fn) const {
            auto it = m_byChunk.find(chunk);
            if (it == m_byChunk.end()) return;
            for (PortalId id : it->second) {
                auto p = m_portals.find(id);
                if (p != m_portals.end()) fn(p->second);
            }
        }

        size_t Count() const { return m_portals.size(); }

    private:
        void Index(const Portal& portal);
        void Unindex(const Portal& portal);

        std::unordered_map<PortalId, Portal> m_portals;
        std::unordered_map<Game::Math::ChunkPos, std::vector<PortalId>,
                           Game::Math::ChunkPosHash> m_byChunk;
    };

    // The store of the level the globals are BOUND to (ClientLevel.hpp) —
    // defined in ClientLevel.cpp. Each level keeps its own portals.
    ClientImmersivePortals& GetClientImmersivePortals();

} // namespace Client

#endif // ENABLE_IMMERSIVE_PORTALS
