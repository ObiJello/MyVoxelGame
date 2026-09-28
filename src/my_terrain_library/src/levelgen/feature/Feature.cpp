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

/**
 * WorldGenLevel::destroyBlock default - WorldGenRegion.destroyBlock: nothing
 * to destroy in air; otherwise the cell becomes air with flag 3 (a
 * generating region has no entities to drop resources into).
 */
bool WorldGenLevel::destroyBlock(const core::BlockPos& pos, bool dropResources) {
    (void)dropResources;
    BlockState* state = getBlockState(pos);
    if (!state || state->isAir()) {
        return false;
    }
    return setBlock(pos, world::level::block::Blocks::AIR->defaultBlockState(), 3);
}

} // namespace levelgen
} // namespace minecraft
