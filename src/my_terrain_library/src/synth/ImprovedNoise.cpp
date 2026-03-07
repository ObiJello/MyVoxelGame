#include "synth/ImprovedNoise.h"
#include "math/Mth.h"

namespace minecraft {

// Define the static gradient table
constexpr int ImprovedNoise::GRADIENT[16][3];

ImprovedNoise::ImprovedNoise(XoroshiroRandomSource& random) {
    // Reference: ImprovedNoise.java lines 14-31

    // Generate random offsets in range [0, 256)
    // CRITICAL: Java uses (double)256.0F - cast float to double!
    this->xo = random.nextDouble() * static_cast<double>(256.0F);
    this->yo = random.nextDouble() * static_cast<double>(256.0F);
    this->zo = random.nextDouble() * static_cast<double>(256.0F);

    // Initialize permutation table with values 0-255
    for (int i = 0; i < 256; ++i) {
        this->p[i] = static_cast<uint8_t>(i);
    }

    // Fisher-Yates shuffle
    // CRITICAL: This must match Java's shuffle exactly!
    for (int i = 0; i < 256; ++i) {
        int offset = random.nextInt(256 - i);
        uint8_t tmp = this->p[i];
        this->p[i] = this->p[i + offset];
        this->p[i + offset] = tmp;
    }
}

ImprovedNoise::ImprovedNoise(LegacyRandomSource& random) {
    this->xo = random.nextDouble() * static_cast<double>(256.0F);
    this->yo = random.nextDouble() * static_cast<double>(256.0F);
    this->zo = random.nextDouble() * static_cast<double>(256.0F);

    for (int i = 0; i < 256; ++i) {
        this->p[i] = static_cast<uint8_t>(i);
    }

    for (int i = 0; i < 256; ++i) {
        int offset = random.nextInt(256 - i);
        uint8_t tmp = this->p[i];
        this->p[i] = this->p[i + offset];
        this->p[i + offset] = tmp;
    }
}

double ImprovedNoise::noise(double x, double y, double z) {
    // Reference: ImprovedNoise.java lines 33-34
    return this->noise(x, y, z, static_cast<double>(0.0F), static_cast<double>(0.0F));
}

double ImprovedNoise::noise(double x, double y, double z, double yScale, double yFudge) {
    // Reference: ImprovedNoise.java lines 39-64

    // Add random offsets
    double xAdj = x + this->xo;
    double yAdj = y + this->yo;
    double zAdj = z + this->zo;

    // Floor coordinates to get integer cell
    int32_t xf = Mth::floor(xAdj);
    int32_t yf = Mth::floor(yAdj);
    int32_t zf = Mth::floor(zAdj);

    // Get fractional part (position within cell)
    double xr = xAdj - static_cast<double>(xf);
    double yr = yAdj - static_cast<double>(yf);
    double zr = zAdj - static_cast<double>(zf);

    // Y-axis fudging (legacy feature, rarely used)
    double yrFudge;
    if (yScale != static_cast<double>(0.0F)) {
        double fudgeLimit;
        if (yFudge >= static_cast<double>(0.0F) && yFudge < yr) {
            fudgeLimit = yFudge;
        } else {
            fudgeLimit = yr;
        }
        yrFudge = static_cast<double>(Mth::floor(fudgeLimit / yScale + static_cast<double>(SHIFT_UP_EPSILON)) * yScale);
    } else {
        yrFudge = static_cast<double>(0.0F);
    }

    return this->sampleAndLerp(xf, yf, zf, xr, yr - yrFudge, zr, yr);
}

double ImprovedNoise::noiseWithDerivative(double x, double y, double z, double derivativeOut[3]) {
    // Reference: ImprovedNoise.java lines 66-77

    double xAdj = x + this->xo;
    double yAdj = y + this->yo;
    double zAdj = z + this->zo;

    int32_t xf = Mth::floor(xAdj);
    int32_t yf = Mth::floor(yAdj);
    int32_t zf = Mth::floor(zAdj);

    double xr = xAdj - static_cast<double>(xf);
    double yr = yAdj - static_cast<double>(yf);
    double zr = zAdj - static_cast<double>(zf);

    return this->sampleWithDerivative(xf, yf, zf, xr, yr, zr, derivativeOut);
}

double ImprovedNoise::gradDot(int hash, double x, double y, double z) {
    // Reference: ImprovedNoise.java lines 79-81
    // Uses SimplexNoise.dot(SimplexNoise.GRADIENT[hash & 15], x, y, z)
    return dot(GRADIENT[hash & 15], x, y, z);
}

int ImprovedNoise::pLookup(int x) const {
    // Reference: ImprovedNoise.java lines 83-85
    // Returns: this.p[x & 255] & 255
    // In Java, byte is signed, so & 255 converts to unsigned
    // In C++, uint8_t is already unsigned, but we still cast to int
    return static_cast<int>(this->p[x & 255]);
}

double ImprovedNoise::sampleAndLerp(int x, int y, int z, double xr, double yr, double zr, double yrOriginal) {
    // Reference: ImprovedNoise.java lines 87-106

    // Hash coordinates to get permutation values
    int x0 = this->pLookup(x);
    int x1 = this->pLookup(x + 1);
    int xy00 = this->pLookup(x0 + y);
    int xy01 = this->pLookup(x0 + y + 1);
    int xy10 = this->pLookup(x1 + y);
    int xy11 = this->pLookup(x1 + y + 1);

    // Get gradient dot products for all 8 cube corners
    double d000 = gradDot(this->pLookup(xy00 + z), xr, yr, zr);
    double d100 = gradDot(this->pLookup(xy10 + z), xr - static_cast<double>(1.0F), yr, zr);
    double d010 = gradDot(this->pLookup(xy01 + z), xr, yr - static_cast<double>(1.0F), zr);
    double d110 = gradDot(this->pLookup(xy11 + z), xr - static_cast<double>(1.0F), yr - static_cast<double>(1.0F), zr);
    double d001 = gradDot(this->pLookup(xy00 + z + 1), xr, yr, zr - static_cast<double>(1.0F));
    double d101 = gradDot(this->pLookup(xy10 + z + 1), xr - static_cast<double>(1.0F), yr, zr - static_cast<double>(1.0F));
    double d011 = gradDot(this->pLookup(xy01 + z + 1), xr, yr - static_cast<double>(1.0F), zr - static_cast<double>(1.0F));
    double d111 = gradDot(this->pLookup(xy11 + z + 1), xr - static_cast<double>(1.0F), yr - static_cast<double>(1.0F), zr - static_cast<double>(1.0F));

    // Compute smoothstep curves for interpolation weights
    double xAlpha = Mth::smoothstep(xr);
    double yAlpha = Mth::smoothstep(yrOriginal);
    double zAlpha = Mth::smoothstep(zr);

    // Trilinear interpolation
    return Mth::lerp3(xAlpha, yAlpha, zAlpha, d000, d100, d010, d110, d001, d101, d011, d111);
}

double ImprovedNoise::sampleWithDerivative(int x, int y, int z, double xr, double yr, double zr, double derivativeOut[3]) {
    // Reference: ImprovedNoise.java lines 108-158

    // Hash coordinates
    int x0 = this->pLookup(x);
    int x1 = this->pLookup(x + 1);
    int xy00 = this->pLookup(x0 + y);
    int xy01 = this->pLookup(x0 + y + 1);
    int xy10 = this->pLookup(x1 + y);
    int xy11 = this->pLookup(x1 + y + 1);

    // Get permutation indices for all 8 corners
    int p000 = this->pLookup(xy00 + z);
    int p100 = this->pLookup(xy10 + z);
    int p010 = this->pLookup(xy01 + z);
    int p110 = this->pLookup(xy11 + z);
    int p001 = this->pLookup(xy00 + z + 1);
    int p101 = this->pLookup(xy10 + z + 1);
    int p011 = this->pLookup(xy01 + z + 1);
    int p111 = this->pLookup(xy11 + z + 1);

    // Get gradient vectors
    const int* g000 = GRADIENT[p000 & 15];
    const int* g100 = GRADIENT[p100 & 15];
    const int* g010 = GRADIENT[p010 & 15];
    const int* g110 = GRADIENT[p110 & 15];
    const int* g001 = GRADIENT[p001 & 15];
    const int* g101 = GRADIENT[p101 & 15];
    const int* g011 = GRADIENT[p011 & 15];
    const int* g111 = GRADIENT[p111 & 15];

    // Compute gradient dot products
    double d000 = dot(g000, xr, yr, zr);
    double d100 = dot(g100, xr - static_cast<double>(1.0F), yr, zr);
    double d010 = dot(g010, xr, yr - static_cast<double>(1.0F), zr);
    double d110 = dot(g110, xr - static_cast<double>(1.0F), yr - static_cast<double>(1.0F), zr);
    double d001 = dot(g001, xr, yr, zr - static_cast<double>(1.0F));
    double d101 = dot(g101, xr - static_cast<double>(1.0F), yr, zr - static_cast<double>(1.0F));
    double d011 = dot(g011, xr, yr - static_cast<double>(1.0F), zr - static_cast<double>(1.0F));
    double d111 = dot(g111, xr - static_cast<double>(1.0F), yr - static_cast<double>(1.0F), zr - static_cast<double>(1.0F));

    // Smoothstep interpolation weights
    double xAlpha = Mth::smoothstep(xr);
    double yAlpha = Mth::smoothstep(yr);
    double zAlpha = Mth::smoothstep(zr);

    // Interpolate gradient components
    double d1x = Mth::lerp3(xAlpha, yAlpha, zAlpha,
                            static_cast<double>(g000[0]), static_cast<double>(g100[0]),
                            static_cast<double>(g010[0]), static_cast<double>(g110[0]),
                            static_cast<double>(g001[0]), static_cast<double>(g101[0]),
                            static_cast<double>(g011[0]), static_cast<double>(g111[0]));
    double d1y = Mth::lerp3(xAlpha, yAlpha, zAlpha,
                            static_cast<double>(g000[1]), static_cast<double>(g100[1]),
                            static_cast<double>(g010[1]), static_cast<double>(g110[1]),
                            static_cast<double>(g001[1]), static_cast<double>(g101[1]),
                            static_cast<double>(g011[1]), static_cast<double>(g111[1]));
    double d1z = Mth::lerp3(xAlpha, yAlpha, zAlpha,
                            static_cast<double>(g000[2]), static_cast<double>(g100[2]),
                            static_cast<double>(g010[2]), static_cast<double>(g110[2]),
                            static_cast<double>(g001[2]), static_cast<double>(g101[2]),
                            static_cast<double>(g011[2]), static_cast<double>(g111[2]));

    // Compute derivatives
    double d2x = Mth::lerp2(yAlpha, zAlpha, d100 - d000, d110 - d010, d101 - d001, d111 - d011);
    double d2y = Mth::lerp2(zAlpha, xAlpha, d010 - d000, d011 - d001, d110 - d100, d111 - d101);
    double d2z = Mth::lerp2(xAlpha, yAlpha, d001 - d000, d101 - d100, d011 - d010, d111 - d110);

    // Smoothstep derivatives
    double xSD = Mth::smoothstepDerivative(xr);
    double ySD = Mth::smoothstepDerivative(yr);
    double zSD = Mth::smoothstepDerivative(zr);

    // Final derivative calculation
    double dX = d1x + xSD * d2x;
    double dY = d1y + ySD * d2y;
    double dZ = d1z + zSD * d2z;

    // Accumulate into output array (Java uses +=)
    derivativeOut[0] += dX;
    derivativeOut[1] += dY;
    derivativeOut[2] += dZ;

    // Return noise value
    return Mth::lerp3(xAlpha, yAlpha, zAlpha, d000, d100, d010, d110, d001, d101, d011, d111);
}

double ImprovedNoise::dot(const int g[3], double x, double y, double z) {
    // Reference: SimplexNoise.java lines 37-39
    return static_cast<double>(g[0]) * x + static_cast<double>(g[1]) * y + static_cast<double>(g[2]) * z;
}

} // namespace minecraft
