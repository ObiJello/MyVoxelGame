// File: src/server/items/HushItems.hpp
//
// Server half of the Hush's "tools of the deep" (docs/the-hush.md). The item
// behaviours call in through the common bridge (common/world/level/
// HushItems.hpp — the tuning fork, the recall chime, the bows, the burst);
// this header is what the SERVER itself calls:
//
//   TickPlayer         — once per player per server tick (PlayerSession::Tick):
//                        advances the module's clock (the cooldowns are server
//                        ticks) and runs the echo compass: every
//                        kCompassCheckTicks, a player in the Hush who carries
//                        one gets the nearest Echo Vault as the needle's
//                        target (HushSignalS2C CompassTarget) — searched once
//                        and cached until they have walked kCompassResearch
//                        blocks from where it was searched (the structure
//                        lookup is /locate's, far too dear to run per tick).
//                        Anywhere else the needle is told to spin. It also
//                        rings a recall chime whose hold ended in this tick's
//                        ServerPlayer::tick (the teleport waits for the
//                        player's own tick to finish).
//   RecordGateCrossing — a player arrived through a hush gate: remember the
//                        landing for the recall chime (ServerPlayer::
//                        setLastHushGate, saved with the player).
//   ForgetPlayer       — the player left; drop their cooldowns and cache.
#pragma once

#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Server {

    class PlayerSession;
    class ServerPlayer;

    namespace HushItems {

        inline constexpr int kCompassCheckTicks = 40;    // 2 s
        inline constexpr int kCompassResearch   = 48;    // blocks walked before a re-search
        inline constexpr int kCompassSearchRings = 8;    // LocateFinder rings (spacing-24 cells)

        void TickPlayer(PlayerSession& session, int64_t serverTick);
        void RecordGateCrossing(ServerPlayer& player, Game::DimensionId dimension,
                                const glm::dvec3& landing, float yaw);
        void ForgetPlayer(uint32_t playerId);

    } // namespace HushItems

} // namespace Server
