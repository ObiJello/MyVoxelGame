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
// These numbers are MC's, verbatim from ChunkLevel.java:
//
//     level <= 31   ENTITY_TICKING   mobs tick, TNT fuses burn
//     level <= 32   BLOCK_TICKING    random ticks, scheduled ticks, fluids
//     level <= 33   FULL             loaded and readable, nothing simulates
//     level >  33   INACCESSIBLE     may be unloaded
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

        // MC's three named rungs.
        inline constexpr int ENTITY_TICKING = 31;
        inline constexpr int BLOCK_TICKING  = 32;
        inline constexpr int FULL           = 33;

        // MC: MAX_LEVEL = 33 + RADIUS_AROUND_FULL_CHUNK, where the radius comes
        // from the generation pyramid — how many chunks of context a FULL chunk
        // needs generated around it. Vanilla's pyramid gives 8.
        //
        // This engine has no generation-status pyramid to derive it from, so
        // the 8 is written out. It is the one number here that is a transcribed
        // constant rather than a computed one, and it only bounds how far a
        // ticket's influence spreads before a chunk is considered unloadable.
        inline constexpr int RADIUS_AROUND_FULL_CHUNK = 8;
        inline constexpr int MAX_LEVEL = FULL + RADIUS_AROUND_FULL_CHUNK;   // 41

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
