// File:  src/platform/Time.cpp
#include "Time.hpp"
#include <chrono>

namespace Time {
    // We use steady_clock so time never jumps backwards.
    static std::chrono::steady_clock::time_point lastTime = std::chrono::steady_clock::now();
    static double deltaSec = 0.0;

    double Now() {
        // Return wall-clock time in seconds since the clock’s epoch:
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now.time_since_epoch()).count();
    }

    void Tick() {
        // Compute how much time has passed since the previous Tick() call.
        auto now = std::chrono::steady_clock::now();
        deltaSec = std::chrono::duration<double>(now - lastTime).count();
        lastTime = now;
    }

    double Delta() {
        return deltaSec;
    }
}
