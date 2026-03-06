#include "levelgen/feature/Feature.h"
#include "levelgen/placement/PlacedFeature.h"

// Reference: net/minecraft/world/level/levelgen/feature/WeightedPlacedFeature.java

namespace minecraft {
namespace levelgen {

/**
 * WeightedPlacedFeature::place implementation
 * Reference: WeightedPlacedFeature.java place() method
 *
 * Simply delegates to the contained PlacedFeature's place method.
 */
bool WeightedPlacedFeature::place(
    WorldGenLevel* level,
    ChunkGenerator* generator,
    WorldgenRandom& random,
    const core::BlockPos& origin
) {
    if (feature) {
        return feature->place(level, generator, random, origin);
    }
    return false;
}

// Static instance of NoneFeatureConfiguration
NoneFeatureConfiguration NoneFeatureConfiguration::INSTANCE;

} // namespace levelgen
} // namespace minecraft
