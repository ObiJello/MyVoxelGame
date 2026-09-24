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
        // Which face of a two-faced portal engages is decided once per
        // frame in Update, by the player's EYE — see the note there. By
        // the time a box reaches this test its record is one the eye is in
        // front of, so no per-box side test is needed here.
        // The WHOLE box must fit inside the opening in the plane, like a
        // body fitting through a hole (Portal::BoxFitsOpening — the same
        // test the server makes for a mob, so what a mob can walk through
        // is exactly what the player can).
        return portal.BoxFitsOpening(glm::dvec3(box.min), glm::dvec3(box.max),
                                     Game::Immersive::kFitSlack);
    }

    bool ImmersivePortalCollision::CellBehind(const Portal& portal, int x, int y, int z, double maxDepth) {
        const glm::dvec3 c(x + 0.5, y + 0.5, z + 0.5);
        const double d = portal.SignedDistanceToPlane(c);
        if (d >= 0.0 || d < -maxDepth) return false;
        return portal.IsInProjection(c, kCellLeniency);
    }

    void ImmersivePortalCollision::Update(const glm::dvec3& playerEye, const Game::AABB& playerBox) {
        m_near.clear();
        Game::SetPortalCollisionActive(false);
        if (!ClientLevels::HasSession()) return;

        const glm::dvec3 c = CenterOf(playerBox);
        GetClientImmersivePortals().ForEach([&](const Portal& p) {
            if (!p.Has(PortalFlag::CrossPortalCollision) || !p.Has(PortalFlag::Teleportable)) return;
            glm::dvec3 mn, mx;
            p.BoundingBox(mn, mx, 0.0);
            if (glm::length(glm::clamp(c, mn, mx) - c) > kNearDistance) return;
            // A two-faced portal — each face of a nether or wand portal is
            // its own record, the flipped twin faces the other way — is
            // only ever gone through from the front of ONE face, and only
            // that face may engage this frame: the twin's "cells behind
            // the plane" are the room the player is standing in, and
            // mapping those through the portal makes the far WALL solid
            // where the player's own air is and pass-through where their
            // floor is. With the twin allowed in while the body straddled
            // the plane, the floor vanished under the player for the last
            // quarter block of every crossing and the arrival box landed
            // inside a phantom cube — the dip on the way through and the
            // lift out of it on arrival.
            //
            // The EYE picks the face, not the box's centre or its corners:
            // the crossing is decided on the eye, which is always in front
            // of the face being entered until the very moment of the
            // teleport (the traveler fires a margin before the surface and
            // lands a margin beyond the far one). The centre is the wrong
            // judge for a floor portal — it passes the plane 0.7 blocks
            // before the eye does, and the ground under the frame turned
            // solid with the player standing in the surface. Decided from
            // the frame's starting eye, so a fast step that carries the
            // moved box's eye past the plane within one physics step keeps
            // the face it entered.
            if (p.flippedPortalId != Game::Immersive::kInvalidPortalId &&
                p.SignedDistanceToPlane(playerEye) < 0.0) {
                return;
            }
            m_near.push_back(p);
        });
        // The physics' all-air early-out must stand aside while a portal
        // is this close — the cells the far side makes solid are air here.
        Game::SetPortalCollisionActive(!m_near.empty());
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

            const glm::dvec3 farPos = p.TransformPoint(glm::dvec3(x + 0.5, y + 0.5, z + 0.5));
            const int fx = static_cast<int>(std::floor(farPos.x));
            const int fy = static_cast<int>(std::floor(farPos.y));
            const int fz = static_cast<int>(std::floor(farPos.z));
            // A global seam's far side that has not streamed in yet counts
            // as solid: an unloaded chunk reads as air, and air under the
            // world's floor drops the player through the seam into whatever
            // the chunk turns out to hold — the Nether's roof, from the
            // inside. An invisible floor for the moment it takes the chunk
            // to arrive is the lesser evil. Doorway portals keep the mod's
            // rule (unloaded = open): their far side is loaded long before
            // anyone can reach the surface.
            if (p.Has(PortalFlag::Global) && !level->Blocks()->IsChunkLoaded(fx >> 4, fz >> 4)) return true;
            const Game::BlockState farState = level->Blocks()->GetBlockState(fx, fy, fz);
            if (!Game::BlockRegistry::HasCollision(farState.Block())) continue;

            // The block's REAL collision shape, not its whole cell. "Has
            // collision" used to be the answer, which made every far-side
            // block a full cube: an open door (a thin slab against one
            // jamb), a fence post, a slab all barred the doorway from this
            // side while being walkable from the other. The body box goes
            // through the portal — its eight corners, re-boxed, which is
            // exact for a same-orientation pair and a conservative hull for
            // a rotated one — and is tested against the shape's boxes at
            // the far cell.
            Game::AABB farBox;
            farBox.min = glm::vec3( 1e30f);
            farBox.max = glm::vec3(-1e30f);
            for (int corner = 0; corner < 8; ++corner) {
                const glm::dvec3 c((corner & 1) ? box.max.x : box.min.x,
                                   (corner & 2) ? box.max.y : box.min.y,
                                   (corner & 4) ? box.max.z : box.min.z);
                const glm::dvec3 t = p.TransformPoint(c);
                farBox.min = glm::min(farBox.min, glm::vec3(t));
                farBox.max = glm::max(farBox.max, glm::vec3(t));
            }
            const auto shapes = Game::BlockRegistry::GetBlockCollisionShapeSet(farState);
            for (uint8_t i = 0; i < shapes.count; ++i) {
                Game::AABB cell;
                cell.min = glm::vec3(fx, fy, fz) + shapes.boxes[i].min;
                cell.max = glm::vec3(fx, fy, fz) + shapes.boxes[i].max;
                if (farBox.Intersects(cell)) return true;
            }
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
