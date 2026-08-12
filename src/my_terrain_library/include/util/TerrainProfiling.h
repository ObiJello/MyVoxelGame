#pragma once

// Tracy zones for the terrain library.
//
// Deliberately NOT common/core/Profiling_Tracy.hpp: pulling that in would need
// ${PROJECT_SOURCE_DIR}/src on this target's include path, and both trees have a
// top-level server/ (and now server/level/), so terrain lib's own headers could
// silently resolve to the game's. This header depends only on TracyClient, which
// exports its own include dir, so there is nothing to collide with.
//
// Compiles to nothing without TRACY_ENABLE, exactly like the game-side macros.
//
// Why this exists: chunk generation is the single most expensive thing in the
// program, and it was a black box. `TerrainLibGetChunk` in MyTerrainGenerator
// measures only the CALLER's wait — for a non-main thread that is a dispatch
// plus a 100us sleep-poll, not work. The real cost runs on the BackgroundExecutor
// pool, which had no zones and no thread names, so it never appeared in a trace
// at all. These macros put it on the timeline.

#if defined(TRACY_ENABLE)
    #include <tracy/Tracy.hpp>
    #define TERRAIN_ZONE_N(name)        ZoneScopedN(name)
    #define TERRAIN_THREAD(name)        tracy::SetThreadName(name)
    #define TERRAIN_PLOT(name, value)   TracyPlot(name, value)
#else
    #define TERRAIN_ZONE_N(name)        (void)0
    #define TERRAIN_THREAD(name)        (void)0
    #define TERRAIN_PLOT(name, value)   (void)0
#endif
