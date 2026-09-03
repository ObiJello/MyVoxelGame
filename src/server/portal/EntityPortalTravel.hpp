// File: src/server/portal/EntityPortalTravel.hpp
//
// Non-player entities crossing immersive portals: mobs, dropped items and
// experience orbs. The Immersive Portals mod's ServerTeleportationManager
// for entities, on this engine's per-dimension ServerLevels.
//
// HOW A CROSSING IS DETECTED
//   The same rule as the player (ImmersivePortalTraveler, client side): the
//   entity's EYE moved along a segment this tick, and that segment pierced a
//   teleportable portal's surface. For mobs the segment is oldPosition →
//   position (both kept by the entity tick); for items and orbs, which keep
//   no old position, it is pos − vel → pos.
//
// HOW AN ENTITY MOVES BETWEEN LEVELS
//   There is one MobManager, ItemEntityManager and ExperienceOrbManager per
//   level, and an entity belongs to exactly one. A same-dimension portal is
//   a plain teleport inside the manager. A cross-dimension one EXTRACTS the
//   entity from the old manager (the mod discards and respawns a copy; here
//   the object survives and keeps its id, which is process-wide), re-points
//   it at the new level (Entity::SetLevel), and inserts it into the new
//   manager with AddExisting / AdoptWithId. Clients see a removal in the old
//   dimension's stream and a spawn in the new one — the entity trackers and
//   the item sync sets do that on their own once the managers are updated.
//
// MOBS CHASING THROUGH PORTALS
//   When a player crosses (IntegratedServer::OnClientPortalTeleport), every
//   mob in the old level that was targeting them is sent walking to the
//   portal (OnPlayerCrossed) and remembered as a Chase. When such a mob
//   crosses, its target becomes the same player's view in the new level as
//   soon as that level has one. The mod does this through its cross-portal
//   pathfinding; this engine's navigation knows one world, so the chase is
//   two ordinary walks stitched together at the portal.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "common/portal/ImmersivePortal.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Game { class Mob; }

namespace Server {

    class IntegratedServer;
    class ServerLevel;
    class ServerPlayer;
    class PlayerSessionManager;

    // Where a player collects items and orbs IN `dimension`: their own
    // position when they stand there, and their image through every portal
    // within reach that leads there — a player at a surface picks up from
    // the far side of it (the mod's cross-portal interaction). A player in
    // another dimension with no such surface is not a collector there at
    // all, which is also what stops a Nether player collecting the Overworld
    // item that happens to share their coordinates.
    struct PickupSource {
        ServerPlayer* player = nullptr;
        glm::dvec3    pos{0.0};
    };
    std::vector<PickupSource> PickupSourcesFor(PlayerSessionManager& sessions, Game::DimensionId dimension);

    class EntityPortalTravel {
    public:
        explicit EntityPortalTravel(IntegratedServer& server);

        // One level's crossings this tick. Server thread, after that level's
        // mobs and loose entities have moved (after TickPortals).
        void Tick(ServerLevel& level, int64_t serverTick);

        // A player of `connectionId` just left `from` through `portal`. Sends
        // the mobs that were hunting them after them.
        void OnPlayerCrossed(ServerLevel& from, const Game::Immersive::Portal& portal,
                             uint32_t connectionId, int64_t serverTick);
        // The player now stands at `newEye` in `dimension` (any teleport
        // through a portal, client-predicted or not): the server's own
        // crossing watch restarts there and holds off a few ticks.
        void OnPlayerTeleported(uint32_t connectionId, Game::DimensionId dimension,
                                const glm::dvec3& newEye, int64_t serverTick);

        size_t ChaseCount() const { return m_chases.size(); }

    private:
        struct Chase {
            int32_t           mobId = 0;
            Game::DimensionId mobDimension = Game::DimensionId::Overworld;
            Game::Immersive::PortalId portalId = Game::Immersive::kInvalidPortalId;
            uint32_t          connectionId = 0;
            int64_t           expiresTick = 0;
            bool              arrived = false;
            // A tempted animal following a held food, not a hunter: no
            // target is set on arrival, and the chase ends the moment the
            // player puts the food away.
            bool              tempt = false;
            // Times the walk to the surface was re-issued after the mob's
            // navigation gave up. A mob that keeps arriving at the frame
            // without going through (a path that ends on the obsidian) is
            // let go rather than pinned there.
            int               reissues = 0;
        };

        // PLAYERS, server-side: the same eye-segment test the client makes
        // (ImmersivePortalTraveler), against the positions the client
        // reports. A client that predicts the crossing reports it first
        // (OnClientPortalTeleport) and this never fires for it; one that
        // does not — an older client, a stalled one — is put through by
        // the server, the way the gun's pre-immersive tick did.
        void TickPlayers(ServerLevel& level, const std::vector<const Game::Immersive::Portal*>& portals,
                         int64_t serverTick);
        // Hostile mobs with no target look through nearby surfaces for
        // players on the far side and go after them (see the .cpp).
        void TickNotice(ServerLevel& level, const std::vector<const Game::Immersive::Portal*>& portals,
                        int64_t serverTick);
        void TickMobs(ServerLevel& level, const std::vector<const Game::Immersive::Portal*>& portals,
                      int64_t serverTick);
        void TickItems(ServerLevel& level, const std::vector<const Game::Immersive::Portal*>& portals,
                       int64_t serverTick);
        void TickOrbs(ServerLevel& level, const std::vector<const Game::Immersive::Portal*>& portals,
                      int64_t serverTick);
        void TickChases(ServerLevel& level, int64_t serverTick);

        // Move one mob through `portal`. False if the far side is not ready
        // (its chunk is not loaded) — the mob stays where it is.
        bool MoveMob(ServerLevel& from, Game::Mob& mob, const Game::Immersive::Portal& portal,
                     int64_t serverTick);
        void SendMobToPortal(Game::Mob& mob, const Game::Immersive::Portal& portal) const;

        // The nearest teleportable surface pierced by the segment, if any.
        static const Game::Immersive::Portal* FindCrossing(
            const std::vector<const Game::Immersive::Portal*>& portals,
            const glm::dvec3& from, const glm::dvec3& to);

        IntegratedServer& m_server;
        std::vector<Chase> m_chases;
        // A crossing mob's removal in the level it left, held back one
        // tick. The client hands a crossing entity between its stores when
        // the far level's ADD arrives; the removal has to land after that
        // add, and the far level's tracker only announces the mob on its own
        // tick. Flushed at the start of this level's next travel tick, by
        // which point the far level has ticked once whichever order the
        // levels run in.
        struct DeferredPacket {
            Game::DimensionId    dimension;
            uint32_t             connectionId;
            uint8_t              packetId;
            std::vector<uint8_t> payload;
        };
        std::vector<DeferredPacket> m_deferredRemovals;
        void FlushDeferredRemovals(Game::DimensionId dimension);
        // Items and orbs have no portal state of their own; a just-arrived
        // one must not be read as crossing back on the next tick.
        std::unordered_map<int32_t, int64_t> m_looseCooldownUntil;
        // Per-player eye tracking for TickPlayers, by connection id.
        struct PlayerTrack {
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            glm::dvec3        eye{0.0};
            int64_t           cooldownUntil = 0;
        };
        std::unordered_map<uint32_t, PlayerTrack> m_playerTracks;
    };

} // namespace Server

#endif // ENABLE_IMMERSIVE_PORTALS
