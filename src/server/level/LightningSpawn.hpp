// File: src/server/level/LightningSpawn.hpp
//
// Server-side helper for putting a lightning bolt into a level — MC's
//     LightningBolt bolt = EntityType.LIGHTNING_BOLT.create(level, ...);
//     bolt.snapTo(pos); bolt.setVisualOnly(visualOnly);
//     level.addFreshEntity(bolt);
// the shape every vanilla (and Twilight Forest) strike site uses.
//
// The bolt goes through ServerLevelBridge::AddFreshEntity, so it is safe from
// anywhere on the server thread — including inside the mob or item tick —
// and joins the mob manager (tracking, AddEntityS2C) at the next absorb.
#pragma once

#include <glm/glm.hpp>

namespace Game { class LightningBolt; }

namespace Server {

    class ServerLevel;

    // Spawns a bolt with its feet at `pos` (MC places it exactly there — the
    // callers add the 0.5 block-centre offset themselves). Returns the bolt,
    // owned by the level from here on (valid until its next tick at least),
    // or null when the level has no entity bridge.
    Game::LightningBolt* SpawnLightningBolt(ServerLevel& level, const glm::dvec3& pos,
                                            bool visualOnly);

} // namespace Server
