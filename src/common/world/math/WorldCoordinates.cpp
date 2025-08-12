// File: src/common/world/math/WorldCoordinates.cpp
#include "WorldCoordinates.hpp"
#include "../../core/Log.hpp"
#include <cmath>

namespace Game::Math {

    // No implementation needed for constexpr functions
    // This file is reserved for any future non-constexpr coordinate utilities

    // Static validation function to check coordinate system assumptions at startup
    bool ValidateCoordinateSystem() {
        bool isValid = true;

        // Test world Y bounds
        if (!WorldCoordinates::IsValidWorldY(Config::MinY)) {
            Log::Error("Coordinate system validation failed: MinY (%d) is not valid", Config::MinY);
            isValid = false;
        }

        if (!WorldCoordinates::IsValidWorldY(Config::MaxY)) {
            Log::Error("Coordinate system validation failed: MaxY (%d) is not valid", Config::MaxY);
            isValid = false;
        }

        // Test section count
        int expectedSections = (Config::MaxY - Config::MinY + 1) / SECTION_HEIGHT;
        if (expectedSections != SECTIONS_PER_CHUNK) {
            Log::Error("Coordinate system validation failed: Expected %d sections, got %d",
                      expectedSections, SECTIONS_PER_CHUNK);
            isValid = false;
        }

        // Test coordinate conversions
        int testWorldY = 64; // Middle of world
        int sectionIndex = WorldCoordinates::WorldYToSectionIndex(testWorldY);
        int sectionLocal = WorldCoordinates::WorldYToSectionLocal(testWorldY);
        int reconstructed = WorldCoordinates::SectionCoordsToWorldY(sectionIndex, sectionLocal);

        if (reconstructed != testWorldY) {
            Log::Error("Coordinate system validation failed: Y conversion round-trip failed (%d -> %d)",
                      testWorldY, reconstructed);
            isValid = false;
        }

        // Test edge cases
        int minSectionIndex = WorldCoordinates::WorldYToSectionIndex(Config::MinY);
        int maxSectionIndex = WorldCoordinates::WorldYToSectionIndex(Config::MaxY);

        if (minSectionIndex < 0 || maxSectionIndex >= SECTIONS_PER_CHUNK) {
            Log::Error("Coordinate system validation failed: Section indices out of range [%d, %d]",
                      minSectionIndex, maxSectionIndex);
            isValid = false;
        }

        if (isValid) {
            Log::Info("Coordinate system validation passed");
            Log::Debug("World Y range: [%d, %d]", Config::MinY, Config::MaxY);
            Log::Debug("Section count: %d", SECTIONS_PER_CHUNK);
            Log::Debug("Section height: %d", SECTION_HEIGHT);
        }

        return isValid;
    }

} // namespace Game::Math