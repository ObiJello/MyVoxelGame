#pragma once

#include "levelgen/density/synth/GradientNoise.h"

// Reference: synth.SimplexNoise (26.3) - 2D/3D simplex noise, computed in
// double and narrowed to float at the end.

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class SimplexNoise : public GradientNoise {
public:
    static const Interval RANGE;
    static constexpr double STANDARD_DEVIATION = 0.42544;

    explicit SimplexNoise(random::AnyRandomSource& random);
    SimplexNoise(random::AnyRandomSource& random, bool discardNoiseOffset);

    Interval range() const override;
    float get(double xin, double yin) const override;
    float get(double xin, double yin, double zin) const override;

private:
    double getCornerNoise3D(int index, double x, double y, double z, double base) const;
};

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
