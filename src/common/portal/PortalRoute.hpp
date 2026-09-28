// File: src/common/portal/PortalRoute.hpp
//
// "How far away is this, counting the way through a portal?" — the one
// formula the server's chunk send order, the client's mesh scheduling and
// the join hand-over all share, so a far side that sits right behind a
// portal next to the player ranks like the ground around the player's feet
// instead of like a place hundreds of blocks away.
//
// A route is one portal seen from one eye:
//
//     S = the point of the portal's surface nearest the eye
//     D = where S comes out on the far side (Portal::TransformPoint)
//     route distance of X (a point on the far side)
//       = |eye − S| + |D − X| / scale
//
// The first leg is walked in the viewer's world, the second in the far one,
// shrunk by the portal's scale (a scale-8 portal makes the far side look
// eight times nearer). By the triangle inequality this bounds the true
// sight-line length through the aperture from above, and is exact for X in
// line with S — for a portal a few blocks wide, within a block or two of it.
// A section's PRIORITY distance is the minimum of its direct distance (when
// the viewer is in its level) and every route into its level.
//
// Only ORDER is derived from this. Which chunks load, and which sections
// are admitted for meshing, is decided elsewhere and never reads it.
#pragma once

#include "common/core/Features.hpp"
#include "common/world/level/DimensionId.hpp"

#if ENABLE_IMMERSIVE_PORTALS
#include "common/portal/ImmersivePortal.hpp"
#endif

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace Game::PortalRoute {

    struct Route {
        Game::DimensionId dimension = Game::DimensionId::Overworld;   // the far side's level
        glm::dvec3        farPoint{0.0};   // D: where the view comes out (far-level block coords)
        double            entryCost = 0.0; // |eye − S| in blocks
        double            invScale  = 1.0; // far-side distances count this much
        uint32_t          portalId  = 0;   // immersive portal id, or 0 for a vanilla gun pair (logs only)

        // Route distance, in blocks, of a far-side point `farDistance`
        // blocks from D.
        double Through(double farDistance) const { return entryCost + farDistance * invScale; }
    };

    // An eye this far BEHIND a surface still counts as in front: a player
    // walking out of a portal, or pressed against a wall-mounted gun oval,
    // is looking through it.
    inline constexpr double kBehindTolerance = 0.5;

#if ENABLE_IMMERSIVE_PORTALS
    // The route through one immersive portal (gun pairs in immersive mode,
    // nether frames, command portals, mirrors, global surfaces), or nullopt
    // when the eye cannot look through it: not visible, the eye behind it,
    // or the surface farther than `maxEntry` blocks. `playerId` filters
    // player-specific portals (0 = no filter).
    inline std::optional<Route> ThroughImmersive(const Game::Immersive::Portal& p,
                                                 const glm::dvec3& eye, double maxEntry,
                                                 uint32_t playerId = 0) {
        if (!p.Has(Game::Immersive::PortalFlag::Visible)) return std::nullopt;
        if (p.specificPlayerId != 0 && playerId != 0 && p.specificPlayerId != playerId) return std::nullopt;
        const glm::dvec3 local = p.WorldToLocal(eye);
        if (local.z < -kBehindTolerance) return std::nullopt;
        // Nearest point of the surface's rectangle (a meshed shape lies
        // inside it; the difference is under a block for any real frame).
        const double u = std::clamp(local.x, -p.HalfWidth(),  p.HalfWidth());
        const double v = std::clamp(local.y, -p.HalfHeight(), p.HalfHeight());
        const glm::dvec3 s = p.LocalToWorld(u, v);
        const double entry = glm::length(eye - s);
        if (!(entry <= maxEntry)) return std::nullopt;
        Route r;
        r.dimension = p.IsMirror() ? p.dimension : p.destDimension;
        r.farPoint  = p.TransformPoint(s);
        r.entryCost = entry;
        r.invScale  = (p.IsMirror() || !(p.scale > 1e-6)) ? 1.0 : 1.0 / p.scale;
        r.portalId  = p.id;
        return r;
    }
#endif

    // The route through one portal of a vanilla (non-immersive) gun pair:
    // a 1×2 oval at `origin` facing `normal`, leading to its partner at
    // `destOrigin`. The opening is small enough that its centre stands in
    // for the nearest point.
    inline std::optional<Route> ThroughGun(const glm::dvec3& origin, const glm::dvec3& normal,
                                           Game::DimensionId destDimension,
                                           const glm::dvec3& destOrigin, const glm::dvec3& destNormal,
                                           const glm::dvec3& eye, double maxEntry) {
        if (glm::dot(eye - origin, normal) < -kBehindTolerance) return std::nullopt;
        const double entry = glm::length(eye - origin);
        if (!(entry <= maxEntry)) return std::nullopt;
        Route r;
        r.dimension = destDimension;
        // Half a block out of the far wall: the section the view opens
        // into, not the wall block the oval is mounted on.
        r.farPoint  = destOrigin + destNormal * 0.5;
        r.entryCost = entry;
        r.invScale  = 1.0;
        r.portalId  = 0;
        return r;
    }

} // namespace Game::PortalRoute
