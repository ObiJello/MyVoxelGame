#include "levelgen/density/terrain/NoiseSpawnFinder.h"

#include "core/QuartPos.h"
#include "world/biome/Climate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Reference: levelgen.NoiseSpawnFinder and SpawnTargetPoint.sampleFitness (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

using world::biome::Climate;

struct Result {
    core::BlockPos location;
    int64_t fitness;
};

// SpawnTargetPoint.sampleFitness: the squared distance of each named
// function's quantized value to its Climate.Parameter span, summed.
int64_t sampleFitness(const SpawnTargetPoint& point, const DensitySamplerSet& samplers, int blockX, int blockY,
                      int blockZ) {
    int64_t fitness = 0;
    for (const SpawnTargetPoint::Entry& parameter : point.parameters) {
        const Climate::Parameter span = Climate::Parameter::span(parameter.min, parameter.max);
        const int64_t value = Climate::quantizeCoord(samplers.sampleValue(parameter.function, blockX, blockY, blockZ));
        const int64_t distance = span.distance(value);
        fitness += distance * distance;
    }
    return fitness;
}

Result getSpawnPositionAndFitness(const DensitySamplerSet& samplers, const std::vector<SpawnTargetPoint>& targetPoints,
                                  int blockX, int blockZ) {
    const int quartBlockX = core::QuartPos::toBlock(core::QuartPos::fromBlock(blockX));
    const int quartBlockZ = core::QuartPos::toBlock(core::QuartPos::fromBlock(blockZ));
    int64_t minFitness = std::numeric_limits<int64_t>::max();
    for (const SpawnTargetPoint& point : targetPoints) {
        minFitness = std::min(minFitness, sampleFitness(point, samplers, quartBlockX, 0, quartBlockZ));
    }
    const int64_t distanceBiasToWorldOrigin =
        static_cast<int64_t>(blockX) * blockX + static_cast<int64_t>(blockZ) * blockZ;
    // Java long arithmetic wraps; so does this (the library builds with -fwrapv).
    const int64_t fitnessWithDistance = minFitness * (2048LL * 2048LL) + distanceBiasToWorldOrigin;
    return Result{core::BlockPos(blockX, 0, blockZ), fitnessWithDistance};
}

void radialSearch(Result& result, const DensitySamplerSet& samplers, const std::vector<SpawnTargetPoint>& targetPoints,
                  float maxRadius, float radiusIncrement) {
    float angle = 0.0f;
    float radius = radiusIncrement;
    const core::BlockPos searchOrigin = result.location;
    while (radius <= maxRadius) {
        const int x = searchOrigin.getX() +
                      static_cast<int>(std::sin(static_cast<double>(angle)) * static_cast<double>(radius));
        const int z = searchOrigin.getZ() +
                      static_cast<int>(std::cos(static_cast<double>(angle)) * static_cast<double>(radius));
        const Result candidate = getSpawnPositionAndFitness(samplers, targetPoints, x, z);
        if (candidate.fitness < result.fitness) {
            result = candidate;
        }
        angle += radiusIncrement / radius;
        if (static_cast<double>(angle) > 6.283185307179586) {
            angle = 0.0f;
            radius += radiusIncrement;
        }
    }
}

} // namespace

core::BlockPos NoiseSpawnFinder::findSpawnPosition(const std::vector<SpawnTargetPoint>& targetPoints,
                                                   const DensitySamplerSet& samplers) {
    Result result = getSpawnPositionAndFitness(samplers, targetPoints, 0, 0);
    radialSearch(result, samplers, targetPoints, 2048.0f, 512.0f);
    radialSearch(result, samplers, targetPoints, 512.0f, 32.0f);
    return result.location;
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
