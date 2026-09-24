// File: src/server/world/ticketing/ChunkLevel.hpp
//
// MC net.minecraft.server.level.ChunkLevel — the one place that says what a
// chunk LEVEL means.
//
// A level is a distance-like integer: LOWER is stronger. A ticket names a
// level at one chunk, and every other chunk's level is that plus its Chebyshev
// distance from the ticket (see ChunkTicketManager). The thresholds below then
// turn a level into a capability.
//
// These numbers are MC's from ChunkLevel.java, shifted up by SIMULATION_HEADROOM:
//
//     level <= 31   ENTITY_TICKING   mobs tick, TNT fuses burn
//     level <= 32   BLOCK_TICKING    random ticks, scheduled ticks, fluids
//     level <= 33   FULL             loaded and readable, nothing simulates
//     level >  33   INACCESSIBLE     may be unloaded
//
// The shift exists because a player's ticket sits at ENTITY_TICKING minus the
// simulation distance and propagation adds one per chunk, so the scale itself
// caps how far simulation can reach: MC's 31 caps it at 32 chunks, which is
// why vanilla's option stops there. This engine lets the option run to
// kMaxSimulationDistance (128, for redstone machines that span a kilometre),
// so the rungs sit 97 higher. Every consumer names the constants and every
// other ticket is relative to them, so nothing else notices.
//
// The engine previously used 33/34/35 for the same three rungs, which was off
// by two and — much worse — was reached by a completely different route: each
// chunk in a square was assigned a level directly by whoever moved the player.
// That let the square be centred on a chunk the player was not standing in,
// which froze every entity near them. Levels are now DERIVED from ticket
// sources and nothing else; see the header note on ChunkTicketManager.
#pragma once

#include <algorithm>

namespace Server {

    namespace ChunkLevel {

        // The largest simulation distance a client may ask for (the server
        // clamps to it: IntegratedServerConfig::maxSimulationDistance).
        inline constexpr int kMaxSimulationDistance = 128;
        // How far MC's rungs are shifted up, so PlayerTicketLevel(distance) is
        // still >= 0 at kMaxSimulationDistance: 31 + 97 - 128 = 0.
        inline constexpr int SIMULATION_HEADROOM = kMaxSimulationDistance - 31;

        // MC's three named rungs (31, 32, 33 before the shift).
        inline constexpr int ENTITY_TICKING = 31 + SIMULATION_HEADROOM;
        inline constexpr int BLOCK_TICKING  = ENTITY_TICKING + 1;
        inline constexpr int FULL           = ENTITY_TICKING + 2;

        // MC: MAX_LEVEL = 33 + RADIUS_AROUND_FULL_CHUNK, where the radius comes
        // from the generation pyramid — how many chunks of context a FULL chunk
        // needs generated around it. Vanilla's pyramid gives 8.
        //
        // This engine has no generation-status pyramid to derive it from, so
        // the 8 is written out. It is the one number here that is a transcribed
        // constant rather than a computed one, and it only bounds how far a
        // ticket's influence spreads before a chunk is considered unloadable.
        inline constexpr int RADIUS_AROUND_FULL_CHUNK = 8;
        inline constexpr int MAX_LEVEL = FULL + RADIUS_AROUND_FULL_CHUNK;   // MC 41, here 138

        // The level a chunk has when nothing reaches it at all. One past
        // MAX_LEVEL so IsLoaded is false and any real ticket wins a min().
        inline constexpr int UNLOADED = MAX_LEVEL + 1;

        constexpr bool IsEntityTicking(int level) { return level <= ENTITY_TICKING; }
        constexpr bool IsBlockTicking(int level)  { return level <= BLOCK_TICKING; }
        constexpr bool IsLoaded(int level)        { return level <= MAX_LEVEL; }

        // MC DistanceManager.getPlayerTicketLevel():
        //     max(0, ChunkLevel.byStatus(ENTITY_TICKING) - simulationDistance)
        //
        // So a player's single ticket sits at level 31 - simulationDistance on
        // the chunk they are standing in, and propagation adds one per chunk
        // outward. Entity ticking therefore reaches exactly simulationDistance
        // chunks and block ticking one further — which is what makes the
        // simulation distance mean what a player expects it to mean, without
        // anyone enumerating a square.
        constexpr int PlayerTicketLevel(int simulationDistance) {
            return std::max(0, ENTITY_TICKING - simulationDistance);
        }

    } // namespace ChunkLevel

} // namespace Server
