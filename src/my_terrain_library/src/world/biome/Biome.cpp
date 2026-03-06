#include "world/biome/Biome.h"
#include "synth/PerlinSimplexNoise.h"
#include "random/LegacyRandomSource.h"
#include <iostream>
#include <iomanip>

namespace minecraft {
namespace world {
namespace biome {

// Static noise instances for temperature calculations
// Reference: Biome.java lines 234-235
static synth::PerlinSimplexNoise* getTemperatureNoise() {
    static synth::PerlinSimplexNoise* noise = nullptr;
    if (!noise) {
        // Reference: Biome.java line 234
        // TEMPERATURE_NOISE = new PerlinSimplexNoise(new WorldgenRandom(new LegacyRandomSource(1234L)), ImmutableList.of(0));
        LegacyRandomSource random(1234L);
        noise = new synth::PerlinSimplexNoise(random, {0});
    }
    return noise;
}

static synth::PerlinSimplexNoise* getFrozenTemperatureNoise() {
    static synth::PerlinSimplexNoise* noise = nullptr;
    if (!noise) {
        // Reference: Biome.java line 235
        LegacyRandomSource random(3456L);
        noise = new synth::PerlinSimplexNoise(random, {-2, -1, 0});
    }
    return noise;
}

static synth::PerlinSimplexNoise* getBiomeInfoNoise() {
    static synth::PerlinSimplexNoise* noise = nullptr;
    if (!noise) {
        LegacyRandomSource random(2345L);
        noise = new synth::PerlinSimplexNoise(random, {0});
    }
    return noise;
}

// Reference: Biome.java TemperatureModifier.FROZEN.modifyTemperature() lines 268-280
float modifyTemperatureFrozen(const core::BlockPos& pos, float baseTemperature) {
    auto* frozenNoise = getFrozenTemperatureNoise();
    auto* biomeNoise = getBiomeInfoNoise();

    // Reference: line 269
    double groundValueLargeVariation = frozenNoise->getValue(
        static_cast<double>(pos.getX()) * 0.05,
        static_cast<double>(pos.getZ()) * 0.05,
        false) * 7.0;

    // Reference: line 270
    double groundValueEdgeVariation = biomeNoise->getValue(
        static_cast<double>(pos.getX()) * 0.2,
        static_cast<double>(pos.getZ()) * 0.2,
        false);

    // Reference: line 271
    double icePatches = groundValueLargeVariation + groundValueEdgeVariation;

    // Reference: lines 272-277
    if (icePatches < 0.3) {
        double groundValueSmallVariation = biomeNoise->getValue(
            static_cast<double>(pos.getX()) * 0.09,
            static_cast<double>(pos.getZ()) * 0.09,
            false);
        if (groundValueSmallVariation < 0.8) {
            return 0.2f;  // Warmer patch - ice melts here
        }
    }

    return baseTemperature;  // Keep base temperature
}

// Reference: Biome.java getHeightAdjustedTemperature() lines 91-100
float Biome::getTemperature(const core::BlockPos& pos, int32_t seaLevel) const {
    float baseTemp = m_climateSettings.temperature;
    float adjustedTemp;

    // Apply temperature modifier
    if (m_climateSettings.temperatureModifier == TemperatureModifier::FROZEN) {
        adjustedTemp = modifyTemperatureFrozen(pos, baseTemp);
    } else {
        adjustedTemp = baseTemp;  // NONE modifier
    }

    // Reference: lines 93-99 - height adjustment with TEMPERATURE_NOISE
    int32_t snowLevel = seaLevel + 17;
    if (pos.getY() > snowLevel) {
        // Reference: Biome.java lines 95-96
        // float v = (float)(TEMPERATURE_NOISE.getValue((double)((float)pos.getX() / 8.0F), (double)((float)pos.getZ() / 8.0F), false) * (double)8.0F);
        // return adjustedTemperature - (v + (float)pos.getY() - (float)snowLevel) * 0.05F / 40.0F;
        auto* tempNoise = getTemperatureNoise();
        // CRITICAL: Java casts to float first, then divides by 8.0F, then casts to double
        // This intermediate float step affects precision and must be matched exactly
        double noiseInputX = static_cast<double>(static_cast<float>(pos.getX()) / 8.0f);
        double noiseInputZ = static_cast<double>(static_cast<float>(pos.getZ()) / 8.0f);
        double noiseRaw = tempNoise->getValue(noiseInputX, noiseInputZ, false);
        float noiseValue = static_cast<float>(noiseRaw * static_cast<double>(8.0f));
        float heightFactor = (noiseValue + static_cast<float>(pos.getY()) - static_cast<float>(snowLevel)) * 0.05f / 40.0f;

        adjustedTemp -= heightFactor;
    }

    return adjustedTemp;
}

} // namespace biome
} // namespace world
} // namespace minecraft
