// File: src/client/portal/ClientImmersivePortals.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "ClientImmersivePortals.hpp"
#include "common/core/Log.hpp"

#include <algorithm>

namespace Client {

    bool ClientImmersivePortals::OnSync(const Portal& portal) {
        if (portal.id == Game::Immersive::kInvalidPortalId) {
            Log::Warning("[ImmersivePortals] Ignoring sync of a portal with no id");
            return false;
        }
        auto it = m_portals.find(portal.id);
        if (it == m_portals.end()) {
            auto [inserted, ok] = m_portals.emplace(portal.id, portal);
            Index(inserted->second);
            Log::Info("[ImmersivePortals] + %s", portal.Describe().c_str());
            return true;
        }
        if (it->second == portal) return false;
        // The anchor chunk may have moved; re-index rather than trust it.
        Unindex(it->second);
        it->second = portal;
        Index(it->second);
        Log::Debug("[ImmersivePortals] ~ %s", portal.Describe().c_str());
        return true;
    }

    void ClientImmersivePortals::OnRemove(PortalId id) {
        auto it = m_portals.find(id);
        if (it == m_portals.end()) return;
        Log::Info("[ImmersivePortals] - %s", it->second.Describe().c_str());
        Unindex(it->second);
        m_portals.erase(it);
    }

    void ClientImmersivePortals::OnChunkUnloaded(Game::Math::ChunkPos chunk) {
        auto it = m_byChunk.find(chunk);
        if (it == m_byChunk.end()) return;
        // Take the list first: erasing from m_portals must not walk the
        // vector it is about to invalidate.
        std::vector<PortalId> ids = std::move(it->second);
        m_byChunk.erase(it);
        for (PortalId id : ids) m_portals.erase(id);
    }

    void ClientImmersivePortals::Clear() {
        m_portals.clear();
        m_byChunk.clear();
    }

    const ClientImmersivePortals::Portal* ClientImmersivePortals::Get(PortalId id) const {
        auto it = m_portals.find(id);
        return it == m_portals.end() ? nullptr : &it->second;
    }

    void ClientImmersivePortals::Index(const Portal& portal) {
        if (portal.Has(Game::Immersive::PortalFlag::Global)) return;   // outlives every chunk
        auto& ids = m_byChunk[portal.OriginChunk()];
        if (std::find(ids.begin(), ids.end(), portal.id) == ids.end()) ids.push_back(portal.id);
    }

    void ClientImmersivePortals::Unindex(const Portal& portal) {
        if (portal.Has(Game::Immersive::PortalFlag::Global)) return;
        auto it = m_byChunk.find(portal.OriginChunk());
        if (it == m_byChunk.end()) return;
        auto& ids = it->second;
        ids.erase(std::remove(ids.begin(), ids.end(), portal.id), ids.end());
        if (ids.empty()) m_byChunk.erase(it);
    }

} // namespace Client

#endif // ENABLE_IMMERSIVE_PORTALS
