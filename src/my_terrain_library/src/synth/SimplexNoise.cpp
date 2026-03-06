#include "synth/SimplexNoise.h"
#include "math/Mth.h"
#include <cmath>

namespace minecraft {
namespace synth {

// Define the static gradient table
constexpr int SimplexNoise::GRADIENT[16][3];

SimplexNoise::SimplexNoise(XoroshiroRandomSource& random) {
    // Reference: SimplexNoise.java lines 15-32
    // Same initialization as ImprovedNoise

    // Generate random offsets in range [0, 256)
    // CRITICAL: Java uses nextDouble() * 256.0
    this->xo = random.nextDouble() * static_cast<double>(256.0F);
    this->yo = random.nextDouble() * static_cast<double>(256.0F);
    this->zo = random.nextDouble() * static_cast<double>(256.0F);

    // Initialize permutation table with values 0-255
    for (int i = 0; i < 256; ++i) {
        this->p[i] = static_cast<uint8_t>(i);
    }

    // Fisher-Yates shuffle
    // CRITICAL: Must match Java's shuffle exactly!
    for (int i = 0; i < 256; ++i) {
        int offset = random.nextInt(256 - i);
        uint8_t tmp = this->p[i];
        this->p[i] = this->p[i + offset];
        this->p[i + offset] = tmp;
    }
}

SimplexNoise::SimplexNoise(LegacyRandomSource& random) {
    // Reference: SimplexNoise.java lines 15-32
    // Same initialization as ImprovedNoise, but using LegacyRandomSource (Java's java.util.Random)

    // Generate random offsets in range [0, 256)
    // CRITICAL: Java uses nextDouble() * 256.0
    this->xo = random.nextDouble() * static_cast<double>(256.0F);
    this->yo = random.nextDouble() * static_cast<double>(256.0F);
    this->zo = random.nextDouble() * static_cast<double>(256.0F);

    // Initialize permutation table with values 0-255
    for (int i = 0; i < 256; ++i) {
        this->p[i] = static_cast<uint8_t>(i);
    }

    // Fisher-Yates shuffle
    // CRITICAL: Must match Java's shuffle exactly!
    for (int i = 0; i < 256; ++i) {
        int offset = random.nextInt(256 - i);
        uint8_t tmp = this->p[i];
        this->p[i] = this->p[i + offset];
        this->p[i + offset] = tmp;
    }
}

int SimplexNoise::pLookup(int x) const {
    // Reference: SimplexNoise.java lines 34-36
    return static_cast<int>(this->p[x & 255]);
}

double SimplexNoise::dot(const int g[3], double x, double y, double z) {
    // Reference: SimplexNoise.java lines 37-39
    return static_cast<double>(g[0]) * x + static_cast<double>(g[1]) * y + static_cast<double>(g[2]) * z;
}

double SimplexNoise::getCornerNoise3D(int gradientIndex, double x, double y, double z, double falloff) const {
    // Reference: SimplexNoise.java lines 41-52
    double contribution = falloff - x * x - y * y - z * z;
    double result;
    if (contribution < static_cast<double>(0.0F)) {
        result = static_cast<double>(0.0F);
    } else {
        contribution = contribution * contribution;
        // gradientIndex is already % 12, no masking needed
        result = contribution * contribution * dot(GRADIENT[gradientIndex], x, y, z);
    }
    return result;
}

double SimplexNoise::getValue(double x, double y) const {
    // Reference: SimplexNoise.java lines 49-95
    // 2D Simplex noise
    // NOTE: SimplexNoise does NOT add xo/yo offsets in 2D (getValue(0,0) returns 0)

    // Skew factor for 2D: F2 = 0.5*(sqrt(3.0)-1.0)
    // Hairy factor for 2D: G2 = (3.0-sqrt(3.0))/6.0

    // Skew the input space to determine which simplex cell we're in
    double skew = (x + y) * F2;
    int i = Mth::floor(x + skew);
    int j = Mth::floor(y + skew);

    // Unskew back to (x,y) space
    double unskew = static_cast<double>(i + j) * G2;
    double x0 = x - (static_cast<double>(i) - unskew);
    double y0 = y - (static_cast<double>(j) - unskew);

    // Determine which simplex we're in
    // Reference: SimplexNoise.java line 65 uses > not >=
    int i1, j1;
    if (x0 > y0) {
        // Lower triangle, XY order: (0,0)->(1,0)->(1,1)
        i1 = 1;
        j1 = 0;
    } else {
        // Upper triangle, YX order: (0,0)->(0,1)->(1,1)
        i1 = 0;
        j1 = 1;
    }

    // Offsets for middle corner in (x,y) unskewed coords
    double x1 = x0 - static_cast<double>(i1) + G2;
    double y1 = y0 - static_cast<double>(j1) + G2;
    // Offsets for last corner in (x,y) unskewed coords
    double x2 = x0 - static_cast<double>(1.0F) + static_cast<double>(2.0F) * G2;
    double y2 = y0 - static_cast<double>(1.0F) + static_cast<double>(2.0F) * G2;

    // Wrap indices to [0, 255]
    int ii = i & 255;
    int jj = j & 255;

    // Get gradient indices
    // CRITICAL: Java uses % 12 (not & 15) - SimplexNoise.java lines 79-81
    int gi0 = this->pLookup(ii + this->pLookup(jj)) % 12;
    int gi1 = this->pLookup(ii + i1 + this->pLookup(jj + j1)) % 12;
    int gi2 = this->pLookup(ii + 1 + this->pLookup(jj + 1)) % 12;

    // Calculate contributions from three corners using getCornerNoise3D
    // Reference: SimplexNoise.java lines 82-84 - Java uses getCornerNoise3D with z=0, base=0.5
    double n0 = this->getCornerNoise3D(gi0, x0, y0, static_cast<double>(0.0F), static_cast<double>(0.5F));
    double n1 = this->getCornerNoise3D(gi1, x1, y1, static_cast<double>(0.0F), static_cast<double>(0.5F));
    double n2 = this->getCornerNoise3D(gi2, x2, y2, static_cast<double>(0.0F), static_cast<double>(0.5F));

    // Scale to return value in [-1, 1]
    // CRITICAL: Java uses 70.0F cast to double
    return static_cast<double>(70.0F) * (n0 + n1 + n2);
}

double SimplexNoise::getValue(double x, double y, double z) const {
    // Reference: SimplexNoise.java lines 97-177
    // 3D Simplex noise
    // NOTE: SimplexNoise does NOT add xo/yo/zo offsets in 3D

    // Skew the input space to determine which simplex cell we're in
    // Reference: SimplexNoise.java lines 89-93
    double skew = (x + y + z) * 0.3333333333333333;
    int i = Mth::floor(x + skew);
    int j = Mth::floor(y + skew);
    int k = Mth::floor(z + skew);

    // Unskew back to (x,y,z) space
    // Reference: SimplexNoise.java lines 94-95
    double unskew = static_cast<double>(i + j + k) * 0.16666666666666666;
    double x0 = x - (static_cast<double>(i) - unskew);
    double y0 = y - (static_cast<double>(j) - unskew);
    double z0 = z - (static_cast<double>(k) - unskew);

    // Determine which simplex we're in
    // For 3D, we need to determine the traversal order through the simplex
    int i1, j1, k1; // Offsets for second corner of simplex
    int i2, j2, k2; // Offsets for third corner of simplex

    if (x0 >= y0) {
        if (y0 >= z0) {
            // X Y Z order
            i1 = 1; j1 = 0; k1 = 0;
            i2 = 1; j2 = 1; k2 = 0;
        } else if (x0 >= z0) {
            // X Z Y order
            i1 = 1; j1 = 0; k1 = 0;
            i2 = 1; j2 = 0; k2 = 1;
        } else {
            // Z X Y order
            i1 = 0; j1 = 0; k1 = 1;
            i2 = 1; j2 = 0; k2 = 1;
        }
    } else {
        if (y0 < z0) {
            // Z Y X order
            i1 = 0; j1 = 0; k1 = 1;
            i2 = 0; j2 = 1; k2 = 1;
        } else if (x0 < z0) {
            // Y Z X order
            i1 = 0; j1 = 1; k1 = 0;
            i2 = 0; j2 = 1; k2 = 1;
        } else {
            // Y X Z order
            i1 = 0; j1 = 1; k1 = 0;
            i2 = 1; j2 = 1; k2 = 0;
        }
    }

    // Offsets for second corner in (x,y,z) coords
    // Reference: SimplexNoise.java lines 154-156
    double x1 = x0 - static_cast<double>(i1) + 0.16666666666666666;
    double y1 = y0 - static_cast<double>(j1) + 0.16666666666666666;
    double z1 = z0 - static_cast<double>(k1) + 0.16666666666666666;

    // Offsets for third corner
    // Reference: SimplexNoise.java lines 157-159
    double x2 = x0 - static_cast<double>(i2) + 0.3333333333333333;
    double y2 = y0 - static_cast<double>(j2) + 0.3333333333333333;
    double z2 = z0 - static_cast<double>(k2) + 0.3333333333333333;

    // Offsets for fourth corner
    // Reference: SimplexNoise.java lines 160-162 - Java uses literal 0.5F
    double x3 = x0 - static_cast<double>(1.0F) + static_cast<double>(0.5F);
    double y3 = y0 - static_cast<double>(1.0F) + static_cast<double>(0.5F);
    double z3 = z0 - static_cast<double>(1.0F) + static_cast<double>(0.5F);

    // Wrap indices
    int ii = i & 255;
    int jj = j & 255;
    int kk = k & 255;

    // Get gradient indices for all 4 corners
    // CRITICAL: Java uses % 12 - SimplexNoise.java lines 166-169
    int gi0 = this->pLookup(ii + this->pLookup(jj + this->pLookup(kk))) % 12;
    int gi1 = this->pLookup(ii + i1 + this->pLookup(jj + j1 + this->pLookup(kk + k1))) % 12;
    int gi2 = this->pLookup(ii + i2 + this->pLookup(jj + j2 + this->pLookup(kk + k2))) % 12;
    int gi3 = this->pLookup(ii + 1 + this->pLookup(jj + 1 + this->pLookup(kk + 1))) % 12;

    // Calculate contributions from four corners using getCornerNoise3D
    // CRITICAL: Java uses 0.6F as the falloff value
    double n0 = this->getCornerNoise3D(gi0, x0, y0, z0, static_cast<double>(0.6F));
    double n1 = this->getCornerNoise3D(gi1, x1, y1, z1, static_cast<double>(0.6F));
    double n2 = this->getCornerNoise3D(gi2, x2, y2, z2, static_cast<double>(0.6F));
    double n3 = this->getCornerNoise3D(gi3, x3, y3, z3, static_cast<double>(0.6F));

    // Scale to return value in [-1, 1]
    // CRITICAL: Java uses 32.0F cast to double
    return static_cast<double>(32.0F) * (n0 + n1 + n2 + n3);
}

} // namespace synth
} // namespace minecraft
