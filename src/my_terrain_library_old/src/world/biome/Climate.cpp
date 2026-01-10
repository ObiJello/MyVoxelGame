#include "world/biome/Climate.h"
#include "core/QuartPos.h"
#include "core/BlockPos.h"
#include "levelgen/DensityFunction.h"
#include "math/Mth.h"
#include <cmath>

namespace minecraft {
namespace world {
namespace biome {

// Reference: Climate.java lines 32-34
Climate::TargetPoint Climate::target(float temperature, float humidity, float continentalness,
                                     float erosion, float depth, float weirdness) {
    return TargetPoint(
        quantizeCoord(temperature),
        quantizeCoord(humidity),
        quantizeCoord(continentalness),
        quantizeCoord(erosion),
        quantizeCoord(depth),
        quantizeCoord(weirdness)
    );
}

// Reference: Climate.java lines 36-38
Climate::ParameterPoint Climate::parameters(float temperature, float humidity, float continentalness,
                                            float erosion, float depth, float weirdness, float offset) {
    return ParameterPoint(
        Parameter::point(temperature),
        Parameter::point(humidity),
        Parameter::point(continentalness),
        Parameter::point(erosion),
        Parameter::point(depth),
        Parameter::point(weirdness),
        quantizeCoord(offset)
    );
}

// Reference: Climate.java lines 40-42
Climate::ParameterPoint Climate::parameters(const Parameter& temperature, const Parameter& humidity,
                                            const Parameter& continentalness, const Parameter& erosion,
                                            const Parameter& depth, const Parameter& weirdness, float offset) {
    return ParameterPoint(
        temperature,
        humidity,
        continentalness,
        erosion,
        depth,
        weirdness,
        quantizeCoord(offset)
    );
}

// Reference: Climate.java lines 44-46
int64_t Climate::quantizeCoord(float coord) {
    // CRITICAL: Java uses exact formula: (long)(coord * 10000.0F)
    return static_cast<int64_t>(coord * 10000.0F);
}

// Reference: Climate.java lines 48-50
float Climate::unquantizeCoord(int64_t coord) {
    // CRITICAL: Java uses exact formula: (float)coord / 10000.0F
    return static_cast<float>(coord) / 10000.0F;
}

// Reference: Climate.java lines 387-393
Climate::TargetPoint Climate::Sampler::sample(int32_t quartX, int32_t quartY, int32_t quartZ) const {
    // Reference: QuartPos.toBlock() - convert quart coords to block coords
    // QuartPos.java line 21: return quart << 2;
    int32_t blockX = core::QuartPos::toBlock(quartX);
    int32_t blockY = core::QuartPos::toBlock(quartY);
    int32_t blockZ = core::QuartPos::toBlock(quartZ);

    // Reference: Climate.java line 391
    // DensityFunction.SinglePointContext context = new DensityFunction.SinglePointContext(blockX, blockY, blockZ);
    density::DensityFunction::SinglePointContext context(blockX, blockY, blockZ);

    // Reference: Climate.java line 392
    // return Climate.target((float)this.temperature.compute(context), ...)
    return Climate::target(
        static_cast<float>(m_temperature->compute(context)),
        static_cast<float>(m_humidity->compute(context)),
        static_cast<float>(m_continentalness->compute(context)),
        static_cast<float>(m_erosion->compute(context)),
        static_cast<float>(m_depth->compute(context)),
        static_cast<float>(m_weirdness->compute(context))
    );
}

// Reference: Climate.java lines 395-397
core::BlockPos Climate::Sampler::findSpawnPosition() const {
    if (m_spawnTarget.empty()) {
        return core::BlockPos(0, 0, 0);  // BlockPos.ZERO
    }
    return Climate::findSpawnPosition(m_spawnTarget, *this);
}

// Reference: Climate.java lines 52-55
Climate::Sampler Climate::empty() {
    // Return a sampler with null density functions
    // In Java this uses DensityFunctions.zero(), but we'll use nullptr for simplicity
    // The caller should check for nullptr before using
    return Sampler(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {});
}

// =========================================================================
// SpawnFinder - Helper class for finding spawn positions
// Reference: Climate.java lines 400-448
// =========================================================================

namespace {

struct SpawnResult {
    core::BlockPos location;
    int64_t fitness;

    SpawnResult(const core::BlockPos& loc, int64_t fit) : location(loc), fitness(fit) {}
};

SpawnResult getSpawnPositionAndFitness(const std::vector<Climate::ParameterPoint>& targetClimates,
                                       const Climate::Sampler& sampler,
                                       int32_t blockX, int32_t blockZ) {
    // Reference: Climate.java lines 432-444
    Climate::TargetPoint targetPoint = sampler.sample(
        core::QuartPos::fromBlock(blockX), 0, core::QuartPos::fromBlock(blockZ));

    // Create a target with zero depth
    Climate::TargetPoint zeroDepthTargetPoint(
        targetPoint.temperature,
        targetPoint.humidity,
        targetPoint.continentalness,
        targetPoint.erosion,
        0,  // zero depth
        targetPoint.weirdness
    );

    int64_t minFitness = INT64_MAX;
    for (const auto& point : targetClimates) {
        minFitness = std::min(minFitness, point.fitness(zeroDepthTargetPoint));
    }

    // Add distance bias towards world origin
    int64_t distanceBiasToWorldOrigin = Mth::square(static_cast<int64_t>(blockX)) +
                                        Mth::square(static_cast<int64_t>(blockZ));
    int64_t fitnessWithDistance = minFitness * Mth::square(static_cast<int64_t>(2048)) + distanceBiasToWorldOrigin;

    return SpawnResult(core::BlockPos(blockX, 0, blockZ), fitnessWithDistance);
}

class SpawnFinder {
public:
    SpawnResult result;

    SpawnFinder(const std::vector<Climate::ParameterPoint>& targetClimates,
                const Climate::Sampler& sampler)
        : result(getSpawnPositionAndFitness(targetClimates, sampler, 0, 0))
    {
        // Reference: Climate.java lines 406-408
        radialSearch(targetClimates, sampler, 2048.0f, 512.0f);
        radialSearch(targetClimates, sampler, 512.0f, 32.0f);
    }

private:
    void radialSearch(const std::vector<Climate::ParameterPoint>& targetClimates,
                     const Climate::Sampler& sampler,
                     float maxRadius, float radiusIncrement) {
        // Reference: Climate.java lines 410-430
        float angle = 0.0f;
        float radius = radiusIncrement;
        core::BlockPos searchOrigin = result.location;

        while (radius <= maxRadius) {
            int32_t x = searchOrigin.getX() + static_cast<int32_t>(std::sin(static_cast<double>(angle)) * static_cast<double>(radius));
            int32_t z = searchOrigin.getZ() + static_cast<int32_t>(std::cos(static_cast<double>(angle)) * static_cast<double>(radius));

            SpawnResult candidate = getSpawnPositionAndFitness(targetClimates, sampler, x, z);
            if (candidate.fitness < result.fitness) {
                result = candidate;
            }

            angle += radiusIncrement / radius;
            if (static_cast<double>(angle) > (3.14159265358979323846 * 2.0)) {
                angle = 0.0f;
                radius += radiusIncrement;
            }
        }
    }
};

} // anonymous namespace

// Reference: Climate.java lines 57-59
core::BlockPos Climate::findSpawnPosition(const std::vector<ParameterPoint>& targetClimates,
                                          const Sampler& sampler) {
    SpawnFinder finder(targetClimates, sampler);
    return finder.result.location;
}

} // namespace biome
} // namespace world
} // namespace minecraft
