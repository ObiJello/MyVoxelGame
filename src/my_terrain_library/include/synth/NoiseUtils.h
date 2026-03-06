#ifndef NOISEUTILS_H
#define NOISEUTILS_H

#include <cmath>

namespace minecraft {

/**
 * NoiseUtils - Utility functions for noise operations.
 *
 * Reference: net/minecraft/world/level/levelgen/synth/NoiseUtils.java
 */
class NoiseUtils {
public:
    /**
     * Bias a noise value towards extremes (-1 or 1).
     * Reference: NoiseUtils.java lines 6-8
     *
     * This function takes a noise value and biases it towards the extremes.
     * When factor > 0, values near 0 are pushed towards -1 or 1.
     *
     * Formula: noise + sin(PI * noise) * factor / PI
     *
     * @param noise The input noise value (typically in range [-1, 1])
     * @param factor The bias factor (higher = more extreme)
     * @return The biased noise value
     */
    static double biasTowardsExtreme(double noise, double factor);

private:
    // Static constant for PI
    static constexpr double PI = 3.14159265358979323846;
};

} // namespace minecraft

#endif // NOISEUTILS_H
