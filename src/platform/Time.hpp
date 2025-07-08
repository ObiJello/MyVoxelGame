// File:  src/platform/Time.hpp
#pragma once

namespace Time {
    // Returns the current wall‐clock time in seconds (monotonic, from epoch).
    double Now();

    // Call once per frame to update internal state (computes delta between frames).
    void Tick();

    // After Tick() has run, Delta() returns the seconds elapsed since the last Tick().
    double Delta();
}
