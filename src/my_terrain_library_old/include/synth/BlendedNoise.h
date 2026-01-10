#ifndef BLENDEDNOISE_H
#define BLENDEDNOISE_H

#include "synth/PerlinNoise.h"
#include "levelgen/DensityFunction.h"
#include "random/XoroshiroRandomSource.h"

namespace minecraft {

// Use density namespace for DensityFunction types
using namespace density;

/**
 * BlendedNoise - The base 3D noise for Minecraft terrain generation.
 *
 * This is a complex noise function that combines three PerlinNoise instances:
 * - minLimitNoise: 16 octaves (range -15 to 0)
 * - maxLimitNoise: 16 octaves (range -15 to 0)
 * - mainNoise: 8 octaves (range -7 to 0)
 *
 * The main noise is sampled first to calculate a blending factor,
 * then min and max noises are blended together based on that factor.
 *
 * Reference: /minecraft/world/level/levelgen/synth/BlendedNoise.java
 */
class BlendedNoise : public DensityFunction {
public:
    /**
     * Create unseeded BlendedNoise (uses seed 0L).
     * Reference: BlendedNoise.java lines 31-33
     *
     * This factory method creates a BlendedNoise with a fixed seed of 0,
     * used for codec deserialization and consistent noise generation.
     *
     * @param xzScale Horizontal scale multiplier
     * @param yScale Vertical scale multiplier
     * @param xzFactor Horizontal factor for main noise division
     * @param yFactor Vertical factor for main noise division
     * @param smearScaleMultiplier Smear scale multiplier for Y-axis
     */
    static BlendedNoise createUnseeded(double xzScale,
                                       double yScale,
                                       double xzFactor,
                                       double yFactor,
                                       double smearScaleMultiplier);

    /**
     * Create BlendedNoise with specified parameters.
     * Reference: BlendedNoise.java lines 50-52 (constructor)
     *
     * @param random Random source for PerlinNoise initialization
     * @param xzScale Horizontal scale multiplier
     * @param yScale Vertical scale multiplier
     * @param xzFactor Horizontal factor for main noise division
     * @param yFactor Vertical factor for main noise division
     * @param smearScaleMultiplier Smear scale multiplier for Y-axis
     */
    BlendedNoise(XoroshiroRandomSource& random,
                 double xzScale,
                 double yScale,
                 double xzFactor,
                 double yFactor,
                 double smearScaleMultiplier);

    /**
     * Copy constructor - creates new BlendedNoise with same parameters but new random.
     * Reference: BlendedNoise.java lines 54-56
     */
    BlendedNoise withNewRandom(XoroshiroRandomSource& terrainRandom) const;

    /**
     * Compute noise value at given context.
     * Reference: BlendedNoise.java lines 58-110
     */
    double compute(const FunctionContext& context) const override;

    /**
     * Get minimum value this noise can produce.
     * Reference: BlendedNoise.java lines 112-114
     */
    double minValue() const override;

    /**
     * Get maximum value this noise can produce.
     * Reference: BlendedNoise.java lines 116-118
     */
    double maxValue() const override;

    /**
     * Fill array implementation (required by DensityFunction base class)
     */
    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override;

    /**
     * Map all visitor implementation (required by DensityFunction base class)
     */
    DensityFunction* mapAll(Visitor& visitor) override;

    // Accessors for testing
    double xzScale() const { return m_xzScale; }
    double yScale() const { return m_yScale; }
    double xzFactor() const { return m_xzFactor; }
    double yFactor() const { return m_yFactor; }
    double smearScaleMultiplier() const { return m_smearScaleMultiplier; }

private:
    /**
     * Internal constructor with pre-created PerlinNoise instances.
     * Reference: BlendedNoise.java lines 35-47
     */
    BlendedNoise(PerlinNoise* minLimitNoise,
                 PerlinNoise* maxLimitNoise,
                 PerlinNoise* mainNoise,
                 double xzScale,
                 double yScale,
                 double xzFactor,
                 double yFactor,
                 double smearScaleMultiplier);

    // Member variables (Reference: BlendedNoise.java lines 19-29)
    PerlinNoise* m_minLimitNoise;  // 16 octaves
    PerlinNoise* m_maxLimitNoise;  // 16 octaves
    PerlinNoise* m_mainNoise;      // 8 octaves
    double m_xzScale;
    double m_yScale;
    double m_xzFactor;
    double m_yFactor;
    double m_smearScaleMultiplier;
    double m_xzMultiplier;  // = 684.412 * xzScale
    double m_yMultiplier;   // = 684.412 * yScale
    double m_maxValue;      // From minLimitNoise.maxBrokenValue(yMultiplier)
};

} // namespace minecraft

#endif // BLENDEDNOISE_H
