#include "world/biome/Climate.h"
#include "core/QuartPos.h"
#include "core/BlockPos.h"
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

// Reference: 26.3 Climate.Sampler.sample
Climate::TargetPoint Climate::Sampler::sample(int32_t quartX, int32_t quartY, int32_t quartZ) const {
    const int32_t blockX = core::QuartPos::toBlock(quartX);
    const int32_t blockY = core::QuartPos::toBlock(quartY);
    const int32_t blockZ = core::QuartPos::toBlock(quartZ);
    // Java evaluates the six arguments left to right.
    const float temperature = m_temperature.sampleValue(blockX, blockY, blockZ);
    const float humidity = m_humidity.sampleValue(blockX, blockY, blockZ);
    const float continentalness = m_continentalness.sampleValue(blockX, blockY, blockZ);
    const float erosion = m_erosion.sampleValue(blockX, blockY, blockZ);
    const float depth = m_depth.sampleValue(blockX, blockY, blockZ);
    const float weirdness = m_weirdness.sampleValue(blockX, blockY, blockZ);
    return Climate::target(temperature, humidity, continentalness, erosion, depth, weirdness);
}

} // namespace biome
} // namespace world
} // namespace minecraft
