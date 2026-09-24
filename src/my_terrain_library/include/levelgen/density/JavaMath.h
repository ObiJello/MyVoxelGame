#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// Java arithmetic the 26.3 density engine relies on, bit for bit.
//
// The engine computes in float (26.3 moved worldgen density from double to
// float) and its results must match Java's exactly: Java evaluates float
// expressions in IEEE single precision, left to right, never fused (the
// library builds with -ffp-contract=off), and its casts to int SATURATE
// (NaN -> 0) where C++'s are undefined out of range.

namespace minecraft {
namespace levelgen {
namespace density {
namespace jmath {

// Java (int) of a double/float: NaN -> 0, clamped to the int range.
inline int32_t d2i(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 2147483647.0) return std::numeric_limits<int32_t>::max();
    if (v <= -2147483648.0) return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(v);
}
inline int32_t f2i(float v) { return d2i(static_cast<double>(v)); }

// Java (long) of a double: NaN -> 0, clamped to the long range.
inline int64_t d2l(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 9223372036854775807.0) return std::numeric_limits<int64_t>::max();
    if (v <= -9223372036854775808.0) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(v);
}

// Mth.floor(double) / Mth.floor(float): (int)Math.floor(v).
inline int32_t floor(double v) { return d2i(std::floor(v)); }
inline int32_t floor(float v) { return d2i(std::floor(static_cast<double>(v))); }
// Mth.lfloor(double): (long)Math.floor(v).
inline int64_t lfloor(double v) { return d2l(std::floor(v)); }

// Math.floorDiv / Math.floorMod (int).
inline int32_t floorDiv(int32_t a, int32_t b) {
    int32_t q = a / b;
    if ((a % b != 0) && ((a ^ b) < 0)) --q;
    return q;
}
inline int32_t floorMod(int32_t a, int32_t b) {
    int32_t m = a % b;
    if (m != 0 && ((m ^ b) < 0)) m += b;
    return m;
}

// Mth.roundToward(input, multiple) = positiveCeilDiv(input, multiple) * multiple,
// positiveCeilDiv(a, b) = -Math.floorDiv(-a, b).
inline int32_t roundToward(int32_t input, int32_t multiple) {
    return -floorDiv(-input, multiple) * multiple;
}

// Mth float helpers (net.minecraft.util.Mth), same expression order.
inline float lerp(float alpha, float p0, float p1) { return p0 + alpha * (p1 - p0); }
inline float lerp2(float a1, float a2, float x00, float x10, float x01, float x11) {
    return lerp(a2, lerp(a1, x00, x10), lerp(a1, x01, x11));
}
inline float lerp3(float a1, float a2, float a3,
                   float x000, float x100, float x010, float x110,
                   float x001, float x101, float x011, float x111) {
    return lerp(a3, lerp2(a1, a2, x000, x100, x010, x110), lerp2(a1, a2, x001, x101, x011, x111));
}
inline float smoothstep(float x) { return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); }
inline float smoothstepDerivative(float x) { return 30.0f * x * x * (x - 1.0f) * (x - 1.0f); }
inline float clampedLerp(float factor, float min, float max) {
    if (factor < 0.0f) return min;
    return factor > 1.0f ? max : lerp(factor, min, max);
}
inline float inverseLerp(float value, float min, float max) { return (value - min) / (max - min); }
inline float clampedMap(float value, float fromMin, float fromMax, float toMin, float toMax) {
    return clampedLerp(inverseLerp(value, fromMin, fromMax), toMin, toMax);
}
inline float map(float value, float fromMin, float fromMax, float toMin, float toMax) {
    return lerp(inverseLerp(value, fromMin, fromMax), toMin, toMax);
}
inline float square(float x) { return x * x; }

// Math.min/Math.max for float: NaN wins, and -0.0 < +0.0.
inline float fmin(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f) return std::signbit(a) ? a : b;
    return a <= b ? a : b;
}
inline float fmax(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f) return std::signbit(a) ? b : a;
    return a >= b ? a : b;
}

// Math.signum(float).
inline float signum(float v) {
    if (v != v || v == 0.0f) return v;
    return v > 0.0f ? 1.0f : -1.0f;
}

// Mth.cube(float).
inline float cube(float x) { return x * x * x; }
// Mth.sqrt(float): (float)Math.sqrt((double)x).
inline float sqrt(float x) { return static_cast<float>(std::sqrt(static_cast<double>(x))); }
// Mth.clamp(float/double/int).
inline float clamp(float value, float min, float max) { return value < min ? min : fmin(value, max); }
inline double clamp(double value, double min, double max) {
    // Math.min(double): NaN wins, -0.0 < +0.0.
    if (value < min) return min;
    if (value != value) return value;
    if (value == 0.0 && max == 0.0) return std::signbit(value) ? value : max;
    return value <= max ? value : max;
}
inline int32_t clamp(int32_t value, int32_t min, int32_t max) {
    const int32_t lower = value >= min ? value : min;   // Math.max(value, min)
    return lower <= max ? lower : max;                  // Math.min(.., max)
}
// Mth.lengthSquared / Mth.length (float overloads).
inline float lengthSquared(float x, float y) { return x * x + y * y; }
inline float lengthSquared(float x, float y, float z) { return x * x + y * y + z * z; }
inline float length(float x, float y) { return static_cast<float>(std::sqrt(static_cast<double>(lengthSquared(x, y)))); }
inline float length(float x, float y, float z) { return sqrt(lengthSquared(x, y, z)); }

// Math.round(float) (JDK 8+ bit-level implementation): nearest int, ties
// toward positive infinity, NaN -> 0, saturating.
inline int32_t round(float a) {
    int32_t intBits;
    std::memcpy(&intBits, &a, sizeof(intBits));
    const int32_t biasedExp = (intBits & 0x7F800000) >> 23;
    const int32_t shift = (24 - 2 + 127) - biasedExp;
    if ((shift & -32) == 0) {
        int32_t r = (intBits & 0x007FFFFF) | (0x007FFFFF + 1);
        if (intBits < 0) r = -r;
        return ((r >> shift) + 1) >> 1;
    }
    return f2i(a);
}

} // namespace jmath
} // namespace density
} // namespace levelgen
} // namespace minecraft
