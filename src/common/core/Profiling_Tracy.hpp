// File: src/common/core/Profiling_Tracy.hpp
// Tracy profiler integration wrapper.
// When TRACY_ENABLE is defined (via -DENABLE_TRACY=ON in CMake), these macros
// expand to Tracy profiling calls. Otherwise they compile to nothing (zero overhead).
//
// Usage:
//   #include "common/core/Profiling_Tracy.hpp"
//   void MyFunction() {
//       PROFILE_ZONE;                    // Auto-named from function signature
//       // ... or ...
//       PROFILE_ZONE_N("CustomName");    // Explicit name for sub-sections
//   }
//
// In the main loop, call PROFILE_FRAME_MARK at the end of each frame.
// In thread entry points, call PROFILE_THREAD("ThreadName") once.
#pragma once

#ifdef TRACY_ENABLE
    #include <tracy/Tracy.hpp>
    #define PROFILE_ZONE            ZoneScoped
    #define PROFILE_ZONE_N(name)    ZoneScopedN(name)
    #define PROFILE_FRAME_MARK              FrameMark
    #define PROFILE_FRAME_MARK_NAMED(name)  FrameMarkNamed(name)
    #define PROFILE_THREAD(name)            tracy::SetThreadName(name)
    // Numeric time-series plot (name must be a string literal). Shows up in
    // Tracy's Plots pane — use for per-frame counts (visible sections, uploads).
    #define PROFILE_PLOT(name, value)       TracyPlot(name, value)

    // A zone on a call site hot enough that the instrumentation itself
    // distorts the measurement. Each ZoneScopedN pair is a TracyQueuePrepare +
    // a timer read + a TracyQueueCommit, ~30-50 ns; on a function called ten
    // million times in a capture that is hundreds of ms attributed to the
    // thing being measured.
    //
    // OFF by default even in a Tracy build. Turn it on with
    // -DEXPLOSION_DETAIL_ZONES=ON when you specifically want the per-call
    // breakdown, and NEVER compare a capture taken with it against one taken
    // without — the same trap as sampled-vs-unsampled captures.
    #ifdef EXPLOSION_DETAIL_ZONES
        #define PROFILE_ZONE_DETAIL(name)   ZoneScopedN(name)
    #else
        #define PROFILE_ZONE_DETAIL(name)   (void)0
    #endif
#else
    #define PROFILE_ZONE            (void)0
    #define PROFILE_ZONE_N(name)    (void)0
    #define PROFILE_FRAME_MARK              (void)0
    #define PROFILE_FRAME_MARK_NAMED(name)  (void)0
    #define PROFILE_THREAD(name)    (void)0
    #define PROFILE_PLOT(name, value)       (void)0
    #define PROFILE_ZONE_DETAIL(name)       (void)0
#endif
