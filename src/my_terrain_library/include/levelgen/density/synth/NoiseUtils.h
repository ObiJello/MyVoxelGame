#pragma once

#include <cmath>

// Reference: synth.NoiseUtils (26.3). The parity config-string helpers are
// debug-only and not ported.

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class NoiseUtils {
public:
    static constexpr double PI = 3.141592653589793; // Math.PI

    // Java: noise + Math.sin(Math.PI * noise) * factor / Math.PI.
    static double biasTowardsExtreme(double noise, double factor) {
        return noise + std::sin(PI * noise) * factor / PI;
    }
};

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
