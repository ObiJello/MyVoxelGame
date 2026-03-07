#ifndef IMPROVEDNOISE_H
#define IMPROVEDNOISE_H

#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"
#include <cstdint>
#include <array>

namespace minecraft {

/**
 * Improved Perlin Noise implementation (Ken Perlin 2002).
 * This is the foundation for all terrain noise generation in Minecraft.
 *
 * Reference: /minecraft/world/level/levelgen/synth/ImprovedNoise.java
 */
class ImprovedNoise {
public:
    /**
     * Gradient vectors for Perlin noise (from SimplexNoise.GRADIENT).
     * Reference: SimplexNoise.java line 7
     *
     * These 16 gradient vectors are carefully chosen to point to the
     * midpoints of edges of a unit cube. This ensures uniform distribution
     * and prevents directional bias in the noise.
     */
    static constexpr int GRADIENT[16][3] = {
        {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
        {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
        {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
        {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1}
    };

    static constexpr float SHIFT_UP_EPSILON = 1.0E-7F;

    /**
     * Construct ImprovedNoise with a random permutation table.
     *
     * Reference: ImprovedNoise.java lines 14-31
     *
     * The constructor:
     * 1. Generates random offsets (xo, yo, zo) in range [0, 256)
     * 2. Creates a permutation table by shuffling 0-255
     *
     * CRITICAL: The permutation table generation must match Java exactly
     * for terrain to be identical!
     */
    explicit ImprovedNoise(XoroshiroRandomSource& random);
    explicit ImprovedNoise(LegacyRandomSource& random);

    /**
     * Generate 3D Perlin noise at position (x, y, z).
     * Reference: ImprovedNoise.java lines 33-34
     */
    double noise(double x, double y, double z);

    /**
     * Generate noise with Y-axis scaling and fudging (legacy/deprecated).
     * Reference: ImprovedNoise.java lines 39-64
     */
    double noise(double x, double y, double z, double yScale, double yFudge);

    /**
     * Generate noise and compute partial derivatives.
     * Reference: ImprovedNoise.java lines 66-77
     *
     * derivativeOut must be a 3-element array that will be incremented
     * with the partial derivatives (dNoise/dx, dNoise/dy, dNoise/dz).
     */
    double noiseWithDerivative(double x, double y, double z, double derivativeOut[3]);

    // Public for verification/testing
    const uint8_t* getPermutationTable() const { return p; }
    double getXo() const { return xo; }
    double getYo() const { return yo; }
    double getZo() const { return zo; }

private:
    /**
     * Permutation table - 256 bytes shuffled from 0-255.
     * Used to generate pseudo-random gradient indices.
     */
    uint8_t p[256];

    /**
     * Random offsets added to input coordinates.
     * These ensure different ImprovedNoise instances produce different patterns.
     */
    double xo, yo, zo;

    /**
     * Gradient dot product.
     * Reference: ImprovedNoise.java lines 79-81
     *
     * Computes the dot product of a gradient vector (from GRADIENT table)
     * with the distance vector (x, y, z).
     */
    static double gradDot(int hash, double x, double y, double z);

    /**
     * Permutation table lookup with wrapping.
     * Reference: ImprovedNoise.java lines 83-85
     * NOTE: In Java this is named p(), but renamed to pLookup() in C++ to avoid name collision
     */
    int pLookup(int x) const;

    /**
     * Sample noise and perform trilinear interpolation.
     * Reference: ImprovedNoise.java lines 87-106
     *
     * This is the core noise sampling function:
     * 1. Get 8 corner gradient values
     * 2. Compute smoothstep curves for interpolation
     * 3. Perform trilinear interpolation
     */
    double sampleAndLerp(int x, int y, int z, double xr, double yr, double zr, double yrOriginal);

    /**
     * Sample noise with derivative calculation.
     * Reference: ImprovedNoise.java lines 108-158
     */
    double sampleWithDerivative(int x, int y, int z, double xr, double yr, double zr, double derivativeOut[3]);

    /**
     * Dot product helper (from SimplexNoise).
     * Reference: SimplexNoise.java lines 37-39
     */
    static double dot(const int g[3], double x, double y, double z);
};

} // namespace minecraft

#endif // IMPROVEDNOISE_H
