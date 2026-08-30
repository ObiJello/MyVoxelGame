// File: src/server/level/PortalForcer.hpp
//
// Port of net.minecraft.world.level.portal.PortalForcer — the half of nether
// travel that decides WHERE you come out.
//
// Two jobs, tried in order:
//   1. Is there already a portal near the scaled destination? Link to it.
//   2. If not, carve one out of the terrain.
//
// Step 2 is why a nether portal always drops you somewhere sane instead of
// inside a wall: it does not just place blocks, it searches a 16-block spiral
// for a column that can HOST a 4x5 frame with air in front of it, prefers a
// spot with clearance on both sides, and only falls back to bulldozing a
// platform at y>=70 when the whole spiral came up empty.
#pragma once

#include "common/world/block/Direction.hpp"
#include "common/world/portal/BlockUtil.hpp"

#include <optional>
#include <glm/glm.hpp>

namespace Server {

    class ServerLevel;

    namespace PortalForcer {

        // MC PortalForcer.TICKET_RADIUS (PortalForcer.java:24). The chunks
        // around an exit portal are held loaded for a moment after arrival so
        // the player does not land in an unloaded void while their own watch
        // set fills in.
        inline constexpr int kTicketRadius = 3;

        // MC's two search radii (PortalForcer.java:25-26). Which one applies
        // depends on the DESTINATION dimension, not the origin — that
        // asymmetry is deliberate in vanilla and is why an overworld portal
        // can link to a nether portal up to 128 blocks away while the return
        // trip only looks 16.
        inline constexpr int kNetherSearchRadius    = 16;
        inline constexpr int kOverworldSearchRadius = 128;

        // Step 1. Null when nothing suitable is in range.
        std::optional<glm::ivec3> FindClosestPortalPosition(
            ServerLevel& level, const glm::ivec3& approximateExitPos, bool toNether);

        // Step 2 — MC PortalForcer.createPortal (:52). Builds a 4x5 obsidian
        // frame around a 2x3 opening and fills it with portal blocks.
        //
        // Returns the opening as a rectangle whose min corner is its bottom
        // cell and whose sizes are (2, 3) — the same shape
        // BlockUtil::GetLargestRectangleAround reports for an existing portal,
        // so the exit-alignment maths does not care which branch produced it.
        //
        // Empty only when the level has no legal Y band at all for a portal,
        // which in practice means a world border pushed everything out.
        std::optional<Game::FoundRectangle> CreatePortal(
            ServerLevel& level, const glm::ivec3& origin, Game::Axis portalAxis);

    } // namespace PortalForcer

} // namespace Server
