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
#else
    #define PROFILE_ZONE            (void)0
    #define PROFILE_ZONE_N(name)    (void)0
    #define PROFILE_FRAME_MARK              (void)0
    #define PROFILE_FRAME_MARK_NAMED(name)  (void)0
    #define PROFILE_THREAD(name)    (void)0
#endif
