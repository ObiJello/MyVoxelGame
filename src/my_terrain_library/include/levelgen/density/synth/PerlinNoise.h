#pragma once

#include "levelgen/density/synth/GradientNoise.h"

// Reference: synth.PerlinNoise (26.3) - one float Perlin octave. get() and
// addToVolume() round differently (the volume walk splits each corner dot
// product into an xz part plus y * gradient.y); both are ported as Java has
// them and callers pick the path Java picks.

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class PerlinNoise : public GradientNoise {
public:
    static const Interval RANGE;
    static constexpr double STANDARD_DEVIATION = 0.2702247831245211;

    explicit PerlinNoise(random::AnyRandomSource& random);

    Interval range() const override;
    float get(double x, double y) const override;
    float get(double x, double y, double z) const override;

    // derivativeOut: float[3], accumulated into (+=).
    float noiseWithDerivative(double x, double y, double z, float* derivativeOut) const;

    void addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
                     double xzScale, double yScale, float amplitude) const override;

protected:
    float sampleAndLerp(int x, int y, int z, float relativeX, float relativeY, float relativeZ,
                        float originalRelativeY) const;

private:
    float sampleWithDerivative(int x, int y, int z, float xr, float yr, float zr, float* derivativeOut) const;
};

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
