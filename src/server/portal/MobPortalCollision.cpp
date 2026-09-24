// File: src/server/portal/MobPortalCollision.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "MobPortalCollision.hpp"

#include "ImmersivePortalRegistry.hpp"

#include <cmath>

namespace Server {

    namespace {
        using Game::Immersive::Portal;
        namespace PortalFlag = Game::Immersive::PortalFlag;

        // The client's numbers (ImmersivePortalCollision.cpp), so a mob and
        // the player get the same wall to walk through.
        //
        // Cells this far behind the plane are opened; a mob that fits the
        // opening is under two blocks in every axis, so this covers the
        // cells it can overlap while its centre is still in front.
        constexpr double kBehindDepth  = 2.5;
        // The box's centre must be within this of the plane to engage.
        constexpr double kEngageDepth  = 2.0;
        // A cell counts as inside the footprint when its centre is within
        // half a cell of it — partially covered cells included.
        constexpr double kCellLeniency = 0.5;

        glm::dvec3 CenterOf(const Game::AABB& box) {
            return glm::dvec3((box.min + box.max) * 0.5f);
        }
    }

    void MobPortalCollision::Refresh(const ImmersivePortalRegistry* registry,
                                     Game::DimensionId dimension) {
        m_portals.clear();
        if (!registry) return;
        registry->ForEachInDimension(dimension, [&](const Portal& p) {
            if (p.kind != Game::Immersive::PortalKind::PortalGun) return;
            if (!p.Has(PortalFlag::CrossPortalCollision) || !p.Has(PortalFlag::Teleportable)) return;
            // A player's private portal is not a hole in the wall for a mob
            // (EntityPortalTravel::TickMobs will not cross it either).
            if (p.specificPlayerId != 0) return;
            m_portals.push_back(p);
        });
    }

    bool MobPortalCollision::BoxEngages(const Portal& portal, const Game::AABB& box) {
        const glm::dvec3 c = CenterOf(box);
        // Runs per solid cell a mob's swept box touches, for every mob near
        // any gun portal — the common case (nowhere near this portal)
        // leaves before the per-corner work below.
        {
            const double reach = kEngageDepth + portal.HalfWidth() + portal.HalfHeight() + 2.0;
            const glm::dvec3 toOrigin = c - portal.origin;
            if (glm::dot(toOrigin, toOrigin) > reach * reach) return false;
        }
        const double d = portal.SignedDistanceToPlane(c);
        // Still engaged a little PAST the plane: the eye decides the
        // crossing, and physics tests the moved box first — a body whose
        // centre has just gone through must not find the wall behind the
        // surface solid again before the crossing fires.
        if (d < -1.0 || d > kEngageDepth) return false;
        return portal.BoxFitsOpening(glm::dvec3(box.min), glm::dvec3(box.max),
                                     Game::Immersive::kFitSlack);
    }

    bool MobPortalCollision::CellBehind(const Portal& portal, int x, int y, int z, double maxDepth) {
        const glm::dvec3 c(x + 0.5, y + 0.5, z + 0.5);
        const double d = portal.SignedDistanceToPlane(c);
        if (d >= 0.0 || d < -maxDepth) return false;
        return portal.IsInProjection(c, kCellLeniency);
    }

    bool MobPortalCollision::IsBlockBehindPortal(int x, int y, int z, const Game::AABB& box) const {
        for (const Portal& p : m_portals) {
            if (!BoxEngages(p, box)) continue;
            if (CellBehind(p, x, y, z, kBehindDepth)) return true;
        }
        return false;
    }

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
