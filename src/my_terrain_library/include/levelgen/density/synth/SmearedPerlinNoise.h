#pragma once

#include "levelgen/density/synth/PerlinNoise.h"

// Reference: synth.SmearedPerlinNoise (26.3, deprecated) - the Perlin octave
// of old_blended_noise: y is "fudged" towards the lattice below it by
// fudgeYScale, which is what smears the legacy terrain vertically.

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class SmearedPerlinNoise : public PerlinNoise {
public:
    SmearedPerlinNoise(random::AnyRandomSource& random, double fudgeYScale);

    static Interval range(double fudgeYScale);

    using PerlinNoise::get;
    Interval range() const override;
    float get(double x, double y, double z) const override;

    void addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
                     double xzScale, double yScale, float amplitude) const override;

private:
    double computeFudgeY(double originalY, double relativeY) const;

    double m_fudgeYScale;
};

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
