// File: src/server/portal/MobPortalCollision.hpp
//
// Cross-portal collision for a server level's MOBS — the half of the
// client's ImmersivePortalCollision a mob needs to walk through a gun
// portal, on the server thread, against the level's own portal list.
//
// WHY IT EXISTS
//   A gun portal sits ON a wall (or a floor). The client lets the local
//   player through by making the wall cells behind the surface non-solid
//   for a body that fits the 1×2 opening (PortalPassthroughFn). A mob's
//   physics runs on the server with no such hook: it walked up to the wall
//   the picture was painted on and stopped there, its eye a half-body
//   short of the plane, and the crossing test in EntityPortalTravel never
//   saw it pierce the surface. Nether-frame portals never had the problem —
//   their interior is air.
//
// WHAT IT ANSWERS
//   PASSTHROUGH only, for gun portals only: a cell behind a gun surface,
//   inside its footprint, does not collide with a box that is in front of
//   the surface, near it, and FITS THROUGH THE OPENING (Portal::
//   BoxFitsOpening with the shared slack — the "fits in a 2×1" rule). No
//   far-side solidity: gun portals never had it on the client either (both
//   ends stand on open floor), and mobs stay out of the frame portals'
//   far-floor machinery, which is the player's.
//
// THREADING
//   Refresh runs on the server thread before the level's mobs tick
//   (IntegratedServer::TickMobs); the mob tick — which may run across the
//   worker pool — only reads. The portals are copied out of the registry
//   so a per-tick registry edit (a gun re-fired mid-tick cannot happen,
//   but the copy makes that a fact rather than a schedule) is harmless.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/physics/Physics.hpp"
#include "common/portal/ImmersivePortal.hpp"
#include "common/world/level/DimensionId.hpp"

#include <cstddef>
#include <vector>

namespace Server {

    class ImmersivePortalRegistry;

    class MobPortalCollision final : public Game::PortalCollisionProvider {
    public:
        // Collect this dimension's gun surfaces. Server thread, before the
        // mobs move. A null registry (no immersive portals) clears the list.
        void Refresh(const ImmersivePortalRegistry* registry, Game::DimensionId dimension);
        void Clear() { m_portals.clear(); }

        // ── Game::PortalCollisionProvider ───────────────────────────────────
        bool HasFarSideSolidity() const override { return false; }
        bool IsBlockBehindPortal(int x, int y, int z, const Game::AABB& box) const override;
        bool IsFarSideSolid(int x, int y, int z, const Game::AABB& box) const override {
            (void)x; (void)y; (void)z; (void)box;
            return false;
        }

        size_t Count() const { return m_portals.size(); }

    private:
        // Is `box` in front of `portal`, close to its plane, and does the
        // whole box fit through the opening?
        static bool BoxEngages(const Game::Immersive::Portal& portal, const Game::AABB& box);
        // Is the cell behind the plane, within `maxDepth`, inside the footprint?
        static bool CellBehind(const Game::Immersive::Portal& portal, int x, int y, int z,
                               double maxDepth);

        std::vector<Game::Immersive::Portal> m_portals;
    };

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
