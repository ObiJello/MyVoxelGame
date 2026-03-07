#pragma once

#include "random/LegacyRandomSource.h"
#include "synth/PerlinNoise.h"
#include "random/XoroshiroRandomSource.h"
#include <vector>
#include <cstdint>

namespace minecraft {

/**
 * NormalNoise - Combines two PerlinNoise instances for normalized noise output
 *
 * Reference: NormalNoise.java
 *
 * This class creates two separate PerlinNoise instances and combines them
 * with a normalization factor to produce noise values with a target deviation.
 * The second noise is sampled at slightly offset coordinates (INPUT_FACTOR).
 */
class NormalNoise {
public:
    /**
     * NoiseParameters - Configuration for NormalNoise
     * Reference: NormalNoise.java lines 97-112
     */
    struct NoiseParameters {
        int32_t firstOctave;
        std::vector<double> amplitudes;

        NoiseParameters(int32_t firstOctave, const std::vector<double>& amplitudes)
            : firstOctave(firstOctave), amplitudes(amplitudes) {}

        NoiseParameters(int32_t firstOctave, double firstAmplitude, const std::vector<double>& additionalAmplitudes)
            : firstOctave(firstOctave) {
            amplitudes.push_back(firstAmplitude);
            amplitudes.insert(amplitudes.end(), additionalAmplitudes.begin(), additionalAmplitudes.end());
        }
    };

    // Constants from NormalNoise.java lines 17-18
    static constexpr double INPUT_FACTOR = 1.0181268882175227;
    static constexpr double TARGET_DEVIATION = 0.3333333333333333;

    /**
     * Factory method for modern noise (post-1.18)
     * Reference: NormalNoise.java lines 35-37
     */
    static NormalNoise create(XoroshiroRandomSource& random, const NoiseParameters& parameters);
    static NormalNoise create(LegacyRandomSource& random, const NoiseParameters& parameters);

    /**
     * Factory method with amplitude array
     * Reference: NormalNoise.java lines 31-33
     */
    static NormalNoise create(XoroshiroRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes);
    static NormalNoise create(LegacyRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes);

    /**
     * Factory method for legacy Nether biome noise (pre-1.18)
     * Reference: NormalNoise.java lines 27-29
     * @deprecated Only for legacy Nether generation
     */
    static NormalNoise createLegacyNetherBiome(XoroshiroRandomSource& random, const NoiseParameters& parameters);
    static NormalNoise createLegacyNetherBiome(LegacyRandomSource& random, const NoiseParameters& parameters);

    /**
     * Get noise value at position
     * Reference: NormalNoise.java lines 76-81
     *
     * Samples two PerlinNoise instances:
     * - first at (x, y, z)
     * - second at (x*INPUT_FACTOR, y*INPUT_FACTOR, z*INPUT_FACTOR)
     * Returns the average scaled by valueFactor
     */
    double getValue(double x, double y, double z) const;

    /**
     * Get maximum possible noise value
     * Reference: NormalNoise.java lines 68-70
     */
    double maxValue() const { return m_maxValue; }

    /**
     * Get the noise parameters
     * Reference: NormalNoise.java lines 83-85
     */
    const NoiseParameters& parameters() const { return m_parameters; }

    // For testing/debugging
    const PerlinNoise& firstNoise() const { return m_first; }
    const PerlinNoise& secondNoise() const { return m_second; }
    double valueFactor() const { return m_valueFactor; }

private:
    /**
     * Private constructor
     * Reference: NormalNoise.java lines 39-66
     *
     * @param random Random source for noise generation
     * @param parameters Noise configuration
     * @param useNewInitialization true=modern (post-1.18), false=legacy Nether
     */
    NormalNoise(XoroshiroRandomSource& random, const NoiseParameters& parameters, bool useNewInitialization);
    NormalNoise(LegacyRandomSource& random, const NoiseParameters& parameters, bool useNewInitialization);

    /**
     * Calculate expected deviation based on octave span
     * Reference: NormalNoise.java lines 72-74
     *
     * Formula: 0.1 * (1.0 + 1.0 / (octaveSpan + 1))
     */
    static double expectedDeviation(int32_t octaveSpan);

    // Member variables - Reference: NormalNoise.java lines 19-23
    double m_valueFactor;
    PerlinNoise m_first;
    PerlinNoise m_second;
    double m_maxValue;
    NoiseParameters m_parameters;
};

} // namespace minecraft
