#ifndef SIMPLEXNOISE_H
#define SIMPLEXNOISE_H

#include "levelgen/WorldgenRandom.h"
#include "random/XoroshiroRandomSource.h"
#include "random/LegacyRandomSource.h"
#include <cstdint>

namespace minecraft {
namespace synth {

/**
 * Simplex Noise implementation for Minecraft.
 * Used primarily for End dimension terrain generation.
 *
 * Reference: /minecraft/world/level/levelgen/synth/SimplexNoise.java
 */
class SimplexNoise {
public:
    /**
     * Gradient vectors for noise generation.
     * Reference: SimplexNoise.java line 7
     */
    static constexpr int GRADIENT[16][3] = {
        {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
        {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
        {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
        {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1}
    };

    /**
     * Skewing factor for 2D simplex noise.
     * F2 = 0.5 * (sqrt(3) - 1)
     * Reference: SimplexNoise.java line 8
     */
    static constexpr double F2 = 0.5 * (1.7320508075688772 - 1.0);

    /**
     * Unskewing factor for 2D simplex noise.
     * G2 = (3 - sqrt(3)) / 6
     * Reference: SimplexNoise.java line 9
     */
    static constexpr double G2 = (3.0 - 1.7320508075688772) / 6.0;

    /**
     * Construct SimplexNoise with XoroshiroRandomSource.
     * Reference: SimplexNoise.java lines 15-32
     */
    explicit SimplexNoise(XoroshiroRandomSource& random);

    /**
     * Construct SimplexNoise with LegacyRandomSource.
     * Used for End island generation to match Java exactly.
     * Reference: SimplexNoise.java lines 15-32
     */
    explicit SimplexNoise(LegacyRandomSource& random);
    explicit SimplexNoise(levelgen::WorldgenRandom& random);

    /**
     * Get 2D simplex noise value.
     * Reference: SimplexNoise.java lines 49-95
     */
    double getValue(double x, double y) const;

    /**
     * Get 3D simplex noise value.
     * Reference: SimplexNoise.java lines 97-177
     */
    double getValue(double x, double y, double z) const;

    // Public for verification/testing
    const uint8_t* getPermutationTable() const { return p; }
    double getXo() const { return xo; }
    double getYo() const { return yo; }
    double getZo() const { return zo; }

private:
    /**
     * Permutation table - 256 bytes shuffled from 0-255.
     */
    uint8_t p[256];

    /**
     * Random offsets added to input coordinates.
     */
    double xo, yo, zo;

    /**
     * Permutation table lookup with wrapping.
     * Reference: SimplexNoise.java lines 34-36
     */
    int pLookup(int x) const;

    /**
     * Dot product of gradient vector with (x, y, z).
     * Reference: SimplexNoise.java lines 37-39
     */
    static double dot(const int g[3], double x, double y, double z);

    /**
     * Calculate corner noise contribution for 3D simplex noise.
     * Reference: SimplexNoise.java lines 41-47
     */
    double getCornerNoise3D(int gradientIndex, double x, double y, double z, double falloff) const;
};

} // namespace synth
} // namespace minecraft

#endif // SIMPLEXNOISE_H
