#pragma once

#include "core/BlockPos.h"
#include "levelgen/density/DensityFunctionCompiler.h"
#include "levelgen/density/terrain/TerrainSettings.h"

#include <vector>

// Reference: levelgen.NoiseSpawnFinder (26.3) - the world-spawn search: the
// best climate fit to the settings' spawn target near the origin, from two
// widening spirals (2048 in 512 steps, then 512 in 32 steps).

namespace minecraft {
namespace levelgen {
namespace density {

class NoiseSpawnFinder {
public:
    static core::BlockPos findSpawnPosition(const std::vector<SpawnTargetPoint>& targetPoints,
                                            const DensitySamplerSet& samplers);
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
