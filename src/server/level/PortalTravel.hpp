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

    } // namespace PortalTravel

} // namespace Server
