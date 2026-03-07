#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/BiomeFeatureRegistry.h"
#include "levelgen/placement/PlacedFeature.h"
#include "world/biome/Biome.h"
#include <iostream>

// Reference: net/minecraft/world/level/levelgen/placement/BiomeFilter.java

// Debug flag for BiomeFilter logging
static bool s_debugBiomeFilter = false;
static int s_biomeFilterCalls = 0;
static int s_biomeFilterFiltered = 0;

namespace minecraft {
namespace levelgen {
namespace placement {

void BiomeFilter::setDebugEnabled(bool enabled) {
    s_debugBiomeFilter = enabled;
    if (enabled) {
        s_biomeFilterCalls = 0;
        s_biomeFilterFiltered = 0;
    }
}

void BiomeFilter::printDebugStats() {
}

int BiomeFilter::getFilteredCount() {
    return s_biomeFilterFiltered;
}

int BiomeFilter::getCallCount() {
    return s_biomeFilterCalls;
}

/**
 * BiomeFilter::shouldPlace - Check if biome has this feature
 * Reference: BiomeFilter.java lines 20-24
 */
bool BiomeFilter::shouldPlace(
    PlacementContext& context,
    WorldgenRandom& random,
    const core::BlockPos& origin
) {
    s_biomeFilterCalls++;

    // Reference: BiomeFilter.java line 21
    // PlacedFeature feature = (PlacedFeature)context.topFeature().orElseThrow(...)
    auto topFeature = context.topFeature();
    if (!topFeature.has_value()) {
        // In Java, this throws IllegalStateException
        // "Tried to biome check an unregistered feature, or a feature that should not restrict the biome"
        // For safety in C++, allow placement
        if (s_debugBiomeFilter) {
        }
        return true;
    }

    // Reference: BiomeFilter.java line 22
    // Holder<Biome> biome = context.getLevel().getBiome(origin);
    const world::biome::Biome* biome = context.getBiome(origin);
    if (!biome) {
        // No biome available - DENY placement (matches Java which throws IllegalStateException)
        if (s_debugBiomeFilter) {
        }
        s_biomeFilterFiltered++;
        return false;
    }
    std::string biomeName = biome->getName();

    // Reference: BiomeFilter.java line 23
    // return context.generator().getBiomeGenerationSettings(biome).hasFeature(feature);
    // We use BiomeFeatureRegistry instead of BiomeGenerationSettings for feature lookup
    bool result = data::worldgen::BiomeFeatureRegistry::hasFeature(biomeName, *topFeature);

    if (!result) {
        s_biomeFilterFiltered++;
    }

    return result;
}

} // namespace placement
} // namespace levelgen
} // namespace minecraft
