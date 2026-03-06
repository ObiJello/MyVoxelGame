#include "synth/NormalNoise.h"
#include <limits>
#include <algorithm>

namespace minecraft {

// Factory methods - Reference: NormalNoise.java lines 31-37

NormalNoise NormalNoise::create(XoroshiroRandomSource& random, const NoiseParameters& parameters) {
    // Reference: NormalNoise.java line 36
    return NormalNoise(random, parameters, true);
}

NormalNoise NormalNoise::create(XoroshiroRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes) {
    // Reference: NormalNoise.java lines 31-33
    return create(random, NoiseParameters(firstOctave, amplitudes));
}

NormalNoise NormalNoise::createLegacyNetherBiome(XoroshiroRandomSource& random, const NoiseParameters& parameters) {
    // Reference: NormalNoise.java lines 27-29
    return NormalNoise(random, parameters, false);
}

// Helper to create first PerlinNoise
static PerlinNoise createFirstNoise(XoroshiroRandomSource& random, int32_t firstOctave,
                                     const std::vector<double>& amplitudes, bool useNewInitialization) {
    if (useNewInitialization) {
        return PerlinNoise::create(random, firstOctave, amplitudes);
    } else {
        return PerlinNoise::createLegacyForLegacyNetherBiome(random, firstOctave, amplitudes);
    }
}

// Helper to create second PerlinNoise
static PerlinNoise createSecondNoise(XoroshiroRandomSource& random, int32_t firstOctave,
                                      const std::vector<double>& amplitudes, bool useNewInitialization) {
    if (useNewInitialization) {
        return PerlinNoise::create(random, firstOctave, amplitudes);
    } else {
        return PerlinNoise::createLegacyForLegacyNetherBiome(random, firstOctave, amplitudes);
    }
}

// Private constructor - Reference: NormalNoise.java lines 39-66
NormalNoise::NormalNoise(XoroshiroRandomSource& random, const NoiseParameters& parameters, bool useNewInitialization)
    : m_first(createFirstNoise(random, parameters.firstOctave, parameters.amplitudes, useNewInitialization))
    , m_second(createSecondNoise(random, parameters.firstOctave, parameters.amplitudes, useNewInitialization))
    , m_parameters(parameters)
{
    const std::vector<double>& amplitudes = parameters.amplitudes;

    // Find min and max octave indices with non-zero amplitudes
    // Reference: NormalNoise.java lines 51-62
    int32_t minOctave = std::numeric_limits<int32_t>::max();
    int32_t maxOctave = std::numeric_limits<int32_t>::min();

    for (size_t i = 0; i < amplitudes.size(); ++i) {
        double amplitude = amplitudes[i];
        // CRITICAL: Java compares with (double)0.0F, not 0.0
        if (amplitude != static_cast<double>(0.0F)) {
            minOctave = std::min(minOctave, static_cast<int32_t>(i));
            maxOctave = std::max(maxOctave, static_cast<int32_t>(i));
        }
    }

    // Calculate value factor - Reference: NormalNoise.java line 64
    // CRITICAL: 0.16666666666666666 is exactly 1/6
    int32_t octaveSpan = maxOctave - minOctave;
    m_valueFactor = 0.16666666666666666 / expectedDeviation(octaveSpan);

    // Calculate max value - Reference: NormalNoise.java line 65
    m_maxValue = (m_first.maxValue() + m_second.maxValue()) * m_valueFactor;
}

// Expected deviation calculation - Reference: NormalNoise.java lines 72-74
double NormalNoise::expectedDeviation(int32_t octaveSpan) {
    // Formula: 0.1 * (1.0 + 1.0 / (octaveSpan + 1))
    // CRITICAL: Java casts 1.0F to double explicitly
    return 0.1 * (static_cast<double>(1.0F) + static_cast<double>(1.0F) / static_cast<double>(octaveSpan + 1));
}

// Get value at position - Reference: NormalNoise.java lines 76-81
double NormalNoise::getValue(double x, double y, double z) const {
    // CRITICAL: Java uses hardcoded literal 1.0181268882175227, not INPUT_FACTOR constant
    // This might be due to constant folding or to ensure exact bit pattern
    double x2 = x * 1.0181268882175227;
    double y2 = y * 1.0181268882175227;
    double z2 = z * 1.0181268882175227;

    // Sample both PerlinNoise instances and combine
    double firstValue = m_first.getValue(x, y, z);
    double secondValue = m_second.getValue(x2, y2, z2);

    return (firstValue + secondValue) * m_valueFactor;
}

} // namespace minecraft
