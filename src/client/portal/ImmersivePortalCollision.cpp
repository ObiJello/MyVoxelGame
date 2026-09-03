// File: src/client/portal/ImmersivePortalCollision.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "ImmersivePortalCollision.hpp"

#include "ClientImmersivePortals.hpp"
#include "client/world/ClientLevel.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace Client {

    ImmersivePortalCollision g_immersivePortalCollision;

    namespace {
        using Game::Immersive::Portal;
        namespace PortalFlag = Game::Immersive::PortalFlag;

        // Portals farther than this from the player's box are not consulted.
        constexpr double kNearDistance = 6.0;
        // Cells this far behind the plane are governed by the far side; the
        // player's box is under two blocks in every axis, so this covers the
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

    bool ImmersivePortalCollision::BoxEngages(const Portal& portal, const Game::AABB& box) {
        const glm::dvec3 c = CenterOf(box);
        // These hooks run for EVERY entity's collision on the client — the
        // far level's mobs included, dozens of them, each against every
        // portal near the player — so the common case (nowhere near this
        // portal) leaves before the per-corner work below.
        {
            const double reach = kEngageDepth + portal.HalfWidth() + portal.HalfHeight() + 2.0;
            const glm::dvec3 toOrigin = c - portal.origin;
            if (glm::dot(toOrigin, toOrigin) > reach * reach) return false;
        }
        const double d = portal.SignedDistanceToPlane(c);
        // Still engaged a little PAST the plane: the eye decides the
        // crossing, and physics tests the moved box first — a body whose
        // centre has just gone through must not find the wall behind a
        // gun portal solid again before the crossing fires.
        if (d < -1.0 || d > kEngageDepth) return false;
        // A two-faced portal — each face of a nether portal is its own
        // record, the flipped twin faces the other way — is never
        // approached from BEHIND: its back is the twin's front, and the
        // twin is the one that engages. Read from behind, the cells behind
        // this face are the cells the player is standing in, and mapping
        // THOSE through the portal turned them solid whenever the far frame
        // had netherrack in front of it — the invisible wall a block before
        // a nether portal.
        if (d < 0.0 && portal.flippedPortalId != Game::Immersive::kInvalidPortalId) return false;
        // The WHOLE box must fit inside the opening in the plane, like a
        // body fitting through a hole: a box hanging out past the opening's
        // side would walk into the wall block's side face, and one a block
        // too high would slip through a hole it cannot fit. The opening is
        // the gun's full 1×2 for a gun portal (its oval is the picture, the
        // two wall cells are the hole), the outline for anything else.
        const bool gun = portal.kind == Game::Immersive::PortalKind::PortalGun;
        const double hw = gun ? 0.5 : portal.HalfWidth();
        const double hh = gun ? 1.0 : portal.HalfHeight();
        constexpr double kFitSlack = 0.08;
        double uMin = 1e300, uMax = -1e300, vMin = 1e300, vMax = -1e300;
        for (int i = 0; i < 8; ++i) {
            const glm::dvec3 corner((i & 1) ? box.max.x : box.min.x,
                                    (i & 2) ? box.max.y : box.min.y,
                                    (i & 4) ? box.max.z : box.min.z);
            const glm::dvec3 local = portal.WorldToLocal(corner);
            uMin = std::min(uMin, local.x); uMax = std::max(uMax, local.x);
            vMin = std::min(vMin, local.y); vMax = std::max(vMax, local.y);
        }
        return uMin >= -hw - kFitSlack && uMax <= hw + kFitSlack &&
               vMin >= -hh - kFitSlack && vMax <= hh + kFitSlack;
    }

    bool ImmersivePortalCollision::CellBehind(const Portal& portal, int x, int y, int z, double maxDepth) {
        const glm::dvec3 c(x + 0.5, y + 0.5, z + 0.5);
        const double d = portal.SignedDistanceToPlane(c);
        if (d >= 0.0 || d < -maxDepth) return false;
        return portal.IsInProjection(c, kCellLeniency);
    }

    void ImmersivePortalCollision::Update(const glm::dvec3& playerFeet, const Game::AABB& playerBox) {
        m_near.clear();
        if (!ClientLevels::HasSession()) return;
        (void)playerFeet;

        const glm::dvec3 c = CenterOf(playerBox);
        GetClientImmersivePortals().ForEach([&](const Portal& p) {
            if (!p.Has(PortalFlag::CrossPortalCollision) || !p.Has(PortalFlag::Teleportable)) return;
            glm::dvec3 mn, mx;
            p.BoundingBox(mn, mx, 0.0);
            if (glm::length(glm::clamp(c, mn, mx) - c) > kNearDistance) return;
            m_near.push_back(p);
        });
    }

    bool ImmersivePortalCollision::IsBlockBehindPortal(int x, int y, int z, const Game::AABB& box) const {
        if (m_near.empty()) return false;
        for (const Portal& p : m_near) {
            if (!BoxEngages(p, box)) continue;
            if (CellBehind(p, x, y, z, kBehindDepth)) return true;
        }
        return false;
    }

    bool ImmersivePortalCollision::IsFarSideSolid(int x, int y, int z, const Game::AABB& box) const {
        if (m_near.empty()) return false;
        for (const Portal& p : m_near) {
            // Gun portals never had far-side solidity and are placed on
            // walls with open floor in front of both ends; the far floor
            // holding you up is a frame-portal concern.
            if (p.kind == Game::Immersive::PortalKind::PortalGun) continue;
            if (!BoxEngages(p, box)) continue;
            if (!CellBehind(p, x, y, z, kBehindDepth)) continue;

            const Game::DimensionId dest = p.IsMirror() ? p.dimension : p.destDimension;
            ClientLevel* level = ClientLevels::Get(dest);
            if (!level || !level->Blocks()) continue;

            const glm::dvec3 far = p.TransformPoint(glm::dvec3(x + 0.5, y + 0.5, z + 0.5));
            const int fx = static_cast<int>(std::floor(far.x));
            const int fy = static_cast<int>(std::floor(far.y));
            const int fz = static_cast<int>(std::floor(far.z));
            const Game::BlockID id = level->Blocks()->GetBlock(fx, fy, fz);
            if (Game::BlockRegistry::HasCollision(id)) return true;
        }
        return false;
    }

    bool ImmersivePortalCollision::PassthroughHook(int x, int y, int z, const Game::AABB& box) {
        return g_immersivePortalCollision.IsBlockBehindPortal(x, y, z, box);
    }

    bool ImmersivePortalCollision::ExtraSolidHook(int x, int y, int z, const Game::AABB& box) {
        return g_immersivePortalCollision.IsFarSideSolid(x, y, z, box);
    }

} // namespace Client

#endif // ENABLE_IMMERSIVE_PORTALS
