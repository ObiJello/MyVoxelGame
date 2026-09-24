#pragma once

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/synth/NoiseStack.h"
#include "random/AnyPositionalRandomFactory.h"

#include <string>

// Reference: synth.BlendedNoise (26.3) - "minecraft:old_blended_noise", the
// legacy 3D terrain noise: two 16-octave smeared limit stacks blended by an
// 8-octave main stack (clamp(main + 0.5, 0, 1) lerps min -> max). Compiles
// to the op/generator samplers Java builds directly (NoiseFunction.Sampler,
// BinaryFunction.ConstAddSampler, ClampFunction.Sampler, LerpFunction.Sampler).

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class BlendedNoise final : public DensityFunction {
public:
    // BlendedNoise.NOISE_SEED (Identifier "minecraft:terrain"): the id the
    // compile context seeds the octaves from (RandomState swaps in a legacy
    // random for it when the settings use the legacy random source).
    static constexpr const char* NOISE_SEED = "minecraft:terrain";

    // BlendedNoise.FbmSet (record).
    struct FbmSet {
        NoiseStackPtr minLimitNoise;
        NoiseStackPtr maxLimitNoise;
        NoiseStackPtr mainNoise;
    };

    BlendedNoise(double xzScale, double yScale, double xzFactor, double yFactor, double smearScaleMultiplier)
        : m_xzScale(xzScale), m_yScale(yScale), m_xzFactor(xzFactor), m_yFactor(yFactor),
          m_smearScaleMultiplier(smearScaleMultiplier) {}

    FbmSet createFbmSet(random::AnyRandomSource& random) const;
    // Throws std::invalid_argument for firstOctave > 0.
    static NoiseStackPtr createFbm(random::AnyRandomSource& random, int firstOctave, double smearScaleY,
                                   double valueFactor);

    DensitySamplerPtr compileSampler(CompileContext& context) const override;
    // @VisibleForTesting
    DensitySamplerPtr compileSampler(random::AnyRandomSource& random) const;

    DensityFunctionPtr rewriteChildren(const DfRewriteRule& rule) const override;
    Interval range() const override;
    int domainAxes() const override { return ALL_AXES; }
    std::string typeId() const override { return "minecraft:old_blended_noise"; }
    bool equals(const DensityFunction& other) const override;
    size_t hash() const override;

    double xzScale() const { return m_xzScale; }
    double yScale() const { return m_yScale; }
    double xzFactor() const { return m_xzFactor; }
    double yFactor() const { return m_yFactor; }
    double smearScaleMultiplier() const { return m_smearScaleMultiplier; }

private:
    double xzMultiplier() const { return 684.412 * m_xzScale; }
    double yMultiplier() const { return 684.412 * m_yScale; }
    static Interval computeFbmRange(int firstOctave, double smearScaleY, double valueFactor);

    double m_xzScale;
    double m_yScale;
    double m_xzFactor;
    double m_yFactor;
    double m_smearScaleMultiplier;
};

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
