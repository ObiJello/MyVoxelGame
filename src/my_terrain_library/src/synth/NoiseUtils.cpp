#include "synth/NoiseUtils.h"
#include <cmath>

namespace minecraft {

double NoiseUtils::biasTowardsExtreme(double noise, double factor) {
    // Reference: NoiseUtils.java lines 6-8
    // Formula: noise + sin(PI * noise) * factor / PI
    return noise + std::sin(PI * noise) * factor / PI;
}

} // namespace minecraft
