#ifndef PERLINNOISE_H
#define PERLINNOISE_H

#include "synth/ImprovedNoise.h"
#include "random/XoroshiroRandomSource.h"
#include <vector>
#include <cstdint>

namespace minecraft {

/**
 * Multi-octave Perlin noise combining multiple ImprovedNoise instances.
 * Each octave contributes at a different frequency and amplitude.
 *
 * Reference: /minecraft/world/level/levelgen/synth/PerlinNoise.java
 */
class PerlinNoise {
public:
    static constexpr int32_t ROUND_OFF = 33554432; // 2^25

    /**
     * Create PerlinNoise with modern initialization (post-1.18).
     * Reference: PerlinNoise.java lines 43-49, 57-59
     *
     * @param random Random source for initialization
     * @param firstOctave The octave number of the first noise level
     * @param amplitudes Amplitude for each octave
     */
    static PerlinNoise create(XoroshiroRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes);

    /**
     * Create PerlinNoise from firstOctave and amplitudes.
     * Reference: PerlinNoise.java lines 51-55
     *
     * @param random Random source
     * @param firstOctave Starting octave number
     * @param firstAmplitude Amplitude of first octave
     * @param amplitudes Additional amplitudes (varargs in Java)
     */
    static PerlinNoise create(XoroshiroRandomSource& random, int32_t firstOctave, double firstAmplitude, const std::vector<double>& additionalAmplitudes = {});

    /**
     * Create PerlinNoise with legacy initialization for Nether biomes (pre-1.18).
     * Reference: PerlinNoise.java lines 39-41
     * @deprecated Only for legacy Nether generation
     *
     * @param random Random source for initialization
     * @param firstOctave The octave number of the first noise level
     * @param amplitudes Amplitude for each octave
     */
    static PerlinNoise createLegacyForLegacyNetherBiome(XoroshiroRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes);

    /**
     * Create PerlinNoise with legacy initialization for BlendedNoise.
     * Reference: PerlinNoise.java lines 33-35
     * @deprecated Only for BlendedNoise generation
     *
     * @param random Random source for initialization
     * @param octaveStart First octave (e.g., -15)
     * @param octaveEnd Last octave (e.g., 0)
     */
    static PerlinNoise createLegacyForBlendedNoise(XoroshiroRandomSource& random, int32_t octaveStart, int32_t octaveEnd);

    /**
     * Get noise value at position (x, y, z).
     * Reference: PerlinNoise.java lines 143-145
     */
    double getValue(double x, double y, double z) const;

    /**
     * Get noise value with Y-scale and fudge (legacy/deprecated).
     * Reference: PerlinNoise.java lines 149-166
     */
    double getValue(double x, double y, double z, double yScale, double yFudge, bool yFlatHack) const;

    /**
     * Get maximum possible value this noise can produce.
     * Reference: PerlinNoise.java lines 135-137
     */
    double maxValue() const { return m_maxValue; }

    /**
     * Get maximum broken value (legacy).
     * Reference: PerlinNoise.java lines 168-170
     */
    double maxBrokenValue(double yScale) const;

    /**
     * Get specific octave noise (for testing/debugging).
     * Reference: PerlinNoise.java lines 188-190
     */
    ImprovedNoise* getOctaveNoise(int32_t i) const;

    /**
     * Wrap coordinate to prevent floating-point precision issues.
     * Reference: PerlinNoise.java lines 192-194
     *
     * This wraps large coordinates to a smaller range to avoid
     * floating-point precision loss in noise calculations.
     */
    static double wrap(double x);

    // Accessors for testing
    int32_t firstOctave() const { return m_firstOctave; }
    const std::vector<double>& amplitudes() const { return m_amplitudes; }
    size_t octaveCount() const { return m_noiseLevels.size(); }

private:
    /**
     * Internal constructor.
     * Reference: PerlinNoise.java lines 84-133
     *
     * @param random Random source
     * @param firstOctave First octave number
     * @param amplitudes Amplitude for each octave
     * @param useNewInitialization Use modern (post-1.18) initialization
     */
    PerlinNoise(XoroshiroRandomSource& random, int32_t firstOctave, const std::vector<double>& amplitudes, bool useNewInitialization);

    /**
     * Calculate edge value for max value computation.
     * Reference: PerlinNoise.java lines 172-186
     */
    double edgeValue(double noiseValue) const;

    /**
     * Skip an octave during legacy initialization.
     * Reference: PerlinNoise.java lines 139-141
     *
     * Consumes 262 random values to match Java's behavior.
     */
    static void skipOctave(XoroshiroRandomSource& random);

    // Member variables (Reference: PerlinNoise.java lines 23-29)
    std::vector<ImprovedNoise*> m_noiseLevels;  // Can contain nullptrs
    int32_t m_firstOctave;
    std::vector<double> m_amplitudes;
    double m_lowestFreqValueFactor;
    double m_lowestFreqInputFactor;
    double m_maxValue;
};

} // namespace minecraft

#endif // PERLINNOISE_H
