// File: src/server/level/PortalTravel.hpp
//
// The second half of a portal crossing: an entity's portal timer has run out,
// so work out where it comes out and put it there.
//
// Ports MC's `NetherPortalBlock.getPortalDestination` / `getExitPortal` /
// `createDimensionTransition` (NetherPortalBlock.java:107-176) and
// `EndPortalBlock.getPortalDestination` (EndPortalBlock.java:71-107),
// collapsed into one entry point because this engine has no per-block class
// to hang the two overrides off.
//
// It lives in its own file rather than in IntegratedServer for a reason worth
// keeping: it is the only code that legitimately touches TWO levels at once,
// and having that in one place makes the "which world am I reading" question
// answerable by reading a single function.
#pragma once

#include "common/world/block/Blocks.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

namespace Game { class Entity; }

namespace Server {

    class IntegratedServer;
    class ServerLevel;

    namespace PortalTravel {

        // `entryPos` is the portal cell the entity was standing in when the
        // timer completed — MC's PortalProcessor.entryPosition. It is NOT the
        // entity's position, and the difference matters: the exit alignment is
        // measured against the RECTANGLE OF PORTAL BLOCKS around this cell, so
        // passing the entity's own block position would measure against
        // whatever it happens to be overlapping.
        //
        // The caller has already armed the entity's portal cooldown (MC does
        // it before resolving a destination, so a failed lookup does not retry
        // every tick). This function is free to fail.
        void Traverse(IntegratedServer& server, ServerLevel& from, Game::Entity& entity,
                      Game::BlockID portal, const glm::ivec3& entryPos);

        // The same trip without a portal block (the /dimension command): the
        // entity goes where a portal at its own position would have sent it.
        // To or from the End that is the End-portal rule (the obsidian
        // platform at END_SPAWN_POINT, or the world spawn coming home); to
        // or from the Hush the hush-portal rule (1:1, the exit aimed at the
        // Hush's surface); the Nether and the Overworld use the nether-portal
        // rule — coordinates scaled by 8, the nearest portal within range
        // reused, else the spot a new one would take (PortalForcer's
        // placement search) with NO portal built: you arrive where the
        // crossing would have put you, and the world stays as it was. Arms
        // the entity's portal cooldown.
        void TravelToDimension(IntegratedServer& server, ServerLevel& from, Game::Entity& entity,
                               Game::DimensionId toDim);

        // MC PoiManager.ensureLoadedAndValid as PortalForcer uses it: hold
        // and synchronously generate the chunks (radius 2) around `around`
        // in `level` before a portal search or build reads them. Shared with
        // TwilightTeleporter (TFTeleporter.loadSurroundingArea).
        void EnsureExitAreaLoaded(ServerLevel& level, const glm::ivec3& around);

        // MC Entity.teleport(TeleportTransition) for a landing another
        // module resolved (TwilightTeleporter, the Aether fall-out): moves a
        // player from `from` to `to` at `position` keeping its own facing,
        // carrying `velocityPerTick` (blocks per tick; zero = arrive still)
        // and the entity's portal state. Non-players are logged and stay,
        // as for every portal route.
        void ArriveAt(IntegratedServer& server, ServerLevel& from, ServerLevel& to,
                      Game::Entity& entity, const glm::dvec3& position,
                      const glm::dvec3& velocityPerTick = glm::dvec3(0.0));

        // Aether EntityMixin.entityFell: `entity` fell to or below the
        // Aether's floor — it comes out at the top of the Overworld at the
        // same x/z (maxBuildHeight − bbHeight), keeping its velocity and
        // rotation, with the portal cooldown armed. No-op outside the Aether.
        void FallOutOfAether(IntegratedServer& server, ServerLevel& from, Game::Entity& entity);

    } // namespace PortalTravel

} // namespace Server
