// File: src/server/portal/TwilightTeleporter.hpp
//
// The server half of the Twilight Forest pool portal (docs/mod-ports.md):
//
//   * the trigger — ProgressionEvents.checkForPortalCreation +
//     TFPortalBlock.tryToCreatePortal: a diamond a player threw, lying in a
//     valid pool (TwilightPortalShape, ModPortalBehaviors.hpp), is spent and
//     the pool becomes twilight_portal blocks. Driven from the server's
//     item-entity tick (ItemEntityManager);
//   * the crossing — TFPortalBlock.getPortalDestination + TFTeleporter: into
//     the Twilight Forest from anywhere, back to the Overworld from it,
//     coordinates scaled by TeleportationScale; the nearest twilight_portal
//     within 200 blocks is reused, else a new pool is built the way
//     TFTeleporter.makePortalAt does; the traveller arrives on the ring
//     beside the pool.
//
// The per-dimension twilight_portal index (IntegratedServer::
// TwilightPortalIndex, a NetherPortalIndex tracking twilight_portal) stands
// in for TF's brute-force getPortalPosition scan and its TeleporterCache.
#pragma once

#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

namespace Game {
    class Entity;
    class World;
    struct ItemEntity;
}

namespace Server {

    class IntegratedServer;
    class ServerLevel;
    class PlayerSessionManager;

    namespace TwilightTeleporter {

        // TFTeleporter.getPortalPosition: `int i = 200` — how far from the
        // scaled destination an existing twilight_portal is reused.
        inline constexpr int kPortalSearchRadius = 200;

        // ProgressionEvents.checkForPortalCreation: the catalyst must lie
        // within the thrower's box inflated by 32 (the non-admin range).
        inline constexpr double kCatalystRange = 32.0;

        // How often a thrown diamond is tested against the pool it lies in.
        // TF polls per player every 100 ticks (20 with checkPortalPlacement);
        // per item every 20 ticks is the same latency bound without walking
        // every player's surroundings.
        inline constexpr int kCatalystCheckInterval = 20;

        // TFPortalBlock.getPortalDestination: the Twilight Forest from
        // anywhere else, the origin dimension (the Overworld) from it.
        Game::DimensionId DestinationOf(Game::DimensionId from);

        // TFPortalBlock.getPortalDestination + TFTeleporter.createTransition.
        // `entryPos` is the entity's block position when the portal fired
        // (TFPortalBlock.entityInside records entity.blockPosition()).
        // With `buildPortal` false (the /dimension command) an existing pool
        // is still reused, but none is built: the traveller lands where TF
        // would have looked for a spot (moveToSafeCoords). Players only; the
        // caller has armed the portal cooldown.
        void Travel(IntegratedServer& server, ServerLevel& from, Game::Entity& entity,
                    const glm::ivec3& entryPos, Game::DimensionId toDim, bool buildPortal);

        // ProgressionEvents.checkForPortalCreation + TFPortalBlock
        // .tryToCreatePortal for one item entity. `thrownByPlayer` is the
        // item manager's record that a player threw it (MC ItemEntity
        // .getOwner()); the thrower is taken to be any player within
        // kCatalystRange in the same dimension. Returns true when the pool
        // was lit and one diamond spent (the caller resyncs the entity).
        bool TryCreatePortalFromCatalyst(Game::World& world, PlayerSessionManager* sessions,
                                         Game::ItemEntity& catalyst, bool thrownByPlayer);

    } // namespace TwilightTeleporter

} // namespace Server
