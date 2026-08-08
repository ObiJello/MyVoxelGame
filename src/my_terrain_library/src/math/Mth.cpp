#include "math/Mth.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace minecraft {

// Static member initialization
float Mth::SIN[65536];
bool Mth::sinTableInitialized = false;

void Mth::initSinTable() {
    // Reference: Mth.java lines 34-39
    // Java: for(int i = 0; i < sin.length; ++i) { sin[i] = (float)Math.sin((double)i / 10430.378350470453); }
    if (sinTableInitialized) return;
    for (int i = 0; i < 65536; ++i) {
        SIN[i] = static_cast<float>(std::sin(static_cast<double>(i) / SIN_SCALE));
    }
    sinTableInitialized = true;
}

float Mth::sin(double radians) {
    // Reference: Mth.java lines 49-51
    // Java: return SIN[(int)((long)(i * 10430.378350470453) & 65535L)];
    // CRITICAL: Java uses DOUBLE arithmetic and casts to LONG before masking!
    if (!sinTableInitialized) initSinTable();
    return SIN[static_cast<int>(static_cast<int64_t>(radians * 10430.378350470453) & SIN_MASK)];
}

float Mth::cos(double radians) {
    // Reference: Mth.java lines 53-55
    // Java: return SIN[(int)((long)(i * 10430.378350470453 + (double)16384.0F) & 65535L)];
    // CRITICAL: Java uses DOUBLE arithmetic and casts to LONG before masking!
    if (!sinTableInitialized) initSinTable();
    return SIN[static_cast<int>(static_cast<int64_t>(radians * 10430.378350470453 + static_cast<double>(16384.0f)) & SIN_MASK)];
}

int32_t Mth::quantize(double value, int32_t divisor) {
    // Reference: Mth.java lines 696-697
    // return floor(value / (double)quantizeResolution) * quantizeResolution;
    return static_cast<int32_t>(std::floor(value / static_cast<double>(divisor))) * divisor;
}

int32_t Mth::absMax(int32_t a, int32_t b) {
    // Reference: Mth.java lines 131-133
    return std::max(std::abs(a), std::abs(b));
}

float Mth::absMax(float a, float b) {
    // Reference: Mth.java lines 135-137
    return std::max(std::abs(a), std::abs(b));
}

double Mth::absMax(double a, double b) {
    // Reference: Mth.java lines 139-141
    return std::max(std::abs(a), std::abs(b));
}

int32_t Mth::chessboardDistance(int32_t x0, int32_t z0, int32_t x1, int32_t z1) {
    // Reference: Mth.java lines 143-145
    return absMax(x1 - x0, z1 - z0);
}

int32_t Mth::ceillog2(int32_t value) {
    // Reference: Mth.java lines 100-105
    // Uses De Bruijn multiplication for fast log2 calculation
    // MULTIPLY_DE_BRUIJN_BIT_POSITION from Mth.java line 14
    static const int32_t MULTIPLY_DE_BRUIJN_BIT_POSITION[32] = {
        0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
        31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
    };

    // isPowerOfTwo check (Mth.java lines 96-98)
    bool isPowerOfTwo = (value != 0) && ((value & (value - 1)) == 0);

    // If not power of two, round up to next power of two
    // smallestEncompassingPowerOfTwo (Mth.java lines 85-93)
    if (!isPowerOfTwo) {
        int32_t result = value - 1;
        result |= result >> 1;
        result |= result >> 2;
        result |= result >> 4;
        result |= result >> 8;
        result |= result >> 16;
        value = result + 1;
    }

    // De Bruijn multiplication (Mth.java lines 101-102)
    return MULTIPLY_DE_BRUIJN_BIT_POSITION[(int32_t)(((int64_t)value * 125613361LL) >> 27) & 31];
}

float Mth::sqrt(float x) {
    // Reference: Mth.java line 38
    // Java: return (float)Math.sqrt((double)x);
    // Must cast to double, compute sqrt, then cast back to float for bit parity
    return static_cast<float>(std::sqrt(static_cast<double>(x)));
}

double Mth::fastInvSqrt(double x) {
    // Reference: Mth.java lines 444-449
    // Fast inverse square root using bit manipulation (Quake III algorithm adapted for double)
    double xhalf = 0.5 * x;
    int64_t i;
    std::memcpy(&i, &x, sizeof(double));
    i = 6910469410427058090LL - (i >> 1);
    std::memcpy(&x, &i, sizeof(double));
    x *= 1.5 - xhalf * x * x;
    return x;
}

double Mth::length(double x, double y, double z) {
    // Reference: Mth.java lines 688-690
    return std::sqrt(lengthSquared(x, y, z));
}

} // namespace minecraft
