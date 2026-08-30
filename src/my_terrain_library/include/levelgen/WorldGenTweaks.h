#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>

// NON-VANILLA sandbox knobs for world generation ("World Properties" in the
// game UI). Every default value routes generation through the byte-identical
// vanilla code path — the parity harness never touches this, and the golden
// regression net proves the defaults are inert. Any non-default value makes
// the world deliberately non-vanilla.

namespace minecraft {
namespace levelgen {

struct WorldGenTweaks {
    // Carvers (caves + canyons) on/off.
    bool carversEnabled = true;

    // Per-GenerationStep feature toggles (11 decoration steps). Structures
    // are governed separately by the world's structures option.
    std::array<bool, 11> featureStepEnabled{true, true, true, true, true, true,
                                            true, true, true, true, true};

    // Density multipliers. featureDensity applies to every step; oreDensity
    // additionally scales UNDERGROUND_ORES (6); vegetationDensity scales
    // VEGETAL_DECORATION (9). 1.0 = vanilla.
    float featureDensity = 1.0f;
    float oreDensity = 1.0f;
    float vegetationDensity = 1.0f;

    // Structure grid frequency: spacing is divided by this (2.0 = twice as
    // close together = ~4x as many). 1.0 = vanilla.
    float structureFrequency = 1.0f;

    // Overworld biomes removed from the MultiNoise parameter list (their
    // climate space falls to the nearest remaining biome). Ignored if it
    // would remove everything.
    std::unordered_set<std::string> disabledBiomes;

    bool isDefault() const {
        if (!carversEnabled) return false;
        for (bool b : featureStepEnabled) if (!b) return false;
        return featureDensity == 1.0f && oreDensity == 1.0f &&
               vegetationDensity == 1.0f && structureFrequency == 1.0f &&
               disabledBiomes.empty();
    }

    float stepMultiplier(int step) const {
        float m = featureDensity;
        if (step == 6) m *= oreDensity;          // UNDERGROUND_ORES
        else if (step == 9) m *= vegetationDensity;  // VEGETAL_DECORATION
        return m;
    }

    // Global instance, read by the generation hooks. Set BEFORE any
    // generation starts (world init), never during.
    static WorldGenTweaks& get();
    static void reset() { get() = WorldGenTweaks(); }

    // Density multiplier for the feature step currently being decorated
    // (reads PlacedFeature's current-step bookkeeping; 1.0 outside the
    // decoration pass). Defined in WorldGenTweaks.cpp to avoid an include
    // cycle with PlacedFeature.h.
    static float currentStepMultiplier();
};

} // namespace levelgen
} // namespace minecraft
