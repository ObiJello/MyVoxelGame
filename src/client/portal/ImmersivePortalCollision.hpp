// File: src/client/portal/ImmersivePortalCollision.hpp
//
// Cross-portal collision for the local player (the Immersive Portals mod's
// PortalCollisionHandler, reduced to what this engine's AABB physics can
// express through its two hooks):
//
//   1. PASSTHROUGH — a cell of THIS world behind a portal's plane, inside
//      the portal's footprint, does not collide while the player is in
//      front of that portal and within its footprint. The mod cuts the
//      entity's collision box at the plane; here the wall behind a portal
//      simply stops being solid for the cell the player would enter.
//
//   2. FAR-SIDE SOLIDITY — the same cells are solid if the FAR world is
//      solid where the portal maps them to. The far floor holds the player
//      up before their eye has crossed (the crossing is eye-based, so the
//      feet lead by ~1.6 blocks going down a floor portal), and a far wall
//      stops them. The mapping is cell centre → TransformPoint → far cell,
//      which is exact for translation-only portals, a rotation of 90° in
//      any axis, and mirrors; a scaled portal gets a coarser answer (one
//      near cell samples one far cell) that still keeps the floor.
//
// Both hooks are called from the physics hot loop per candidate cell, so
// the portals considered are refreshed once per frame (Update) and kept in
// a tiny list: the ones within a few blocks of the player.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/portal/ImmersivePortal.hpp"
#include "common/physics/Physics.hpp"

#include <glm/glm.hpp>

#include <vector>

namespace Client {

    class ImmersivePortalCollision {
    public:
        // Once per frame, before the player's physics, with the ACTIVE level
        // bound: collects the collision-carrying portals near the player.
        void Update(const glm::dvec3& playerFeet, const Game::AABB& playerBox);

        bool IsBlockBehindPortal(int x, int y, int z, const Game::AABB& box) const;
        bool IsFarSideSolid(int x, int y, int z, const Game::AABB& box) const;

        // Plain-pointer adapters for Game::SetPortalPassthroughFn /
        // SetPortalExtraSolidFn.
        static bool PassthroughHook(int x, int y, int z, const Game::AABB& box);
        static bool ExtraSolidHook(int x, int y, int z, const Game::AABB& box);

        size_t NearCount() const { return m_near.size(); }

    private:
        // Is `box` in front of `portal` and inside its footprint, close
        // enough to the plane that the cells behind it matter?
        static bool BoxEngages(const Game::Immersive::Portal& portal, const Game::AABB& box);
        // Is the cell behind the plane and inside the footprint?
        static bool CellBehind(const Game::Immersive::Portal& portal, int x, int y, int z,
                               double maxDepth);

        std::vector<Game::Immersive::Portal> m_near;
    };

    extern ImmersivePortalCollision g_immersivePortalCollision;

} // namespace Client

#endif // ENABLE_IMMERSIVE_PORTALS
