#ifndef MTH_H
#define MTH_H

#include <cstdint>

namespace minecraft {

/**
 * Math utility functions from Minecraft.
 * Reference: /minecraft/util/Mth.java
 */
class Mth {
private:
    // Lookup table for sin/cos - Reference: Mth.java lines 30-39
    static constexpr int SIN_QUANTIZATION = 65536;
    static constexpr int SIN_MASK = 65535;
    static constexpr int COS_OFFSET = 16384;
    static constexpr double SIN_SCALE = 10430.378350470453;
    static float SIN[65536];
    static bool sinTableInitialized;

    static void initSinTable();

public:
    /**
     * Sine function using lookup table for Java parity
     * Reference: Mth.java lines 49-51
     *
     * CRITICAL: Uses lookup table, NOT std::sin, for bit-exact parity with Java!
     */
    static float sin(double radians);

    /**
     * Cosine function using lookup table for Java parity
     * Reference: Mth.java lines 53-55
     *
     * CRITICAL: Uses lookup table, NOT std::cos, for bit-exact parity with Java!
     */
    static float cos(double radians);

    /**
     * Generate a position-based seed from (x, y, z) coordinates.
     * This is used to create deterministic random sources for specific positions.
     *
     * Reference: Mth.java lines 322-326
     *
     * CRITICAL: This is deprecated in Java but still used throughout the codebase
     * for positional randomness in world generation.
     */
    static inline int64_t getSeed(int32_t x, int32_t y, int32_t z) {
        int64_t seed = static_cast<int64_t>(x * 3129871) ^
                       static_cast<int64_t>(z) * 116129781LL ^
                       static_cast<int64_t>(y);
        seed = seed * seed * 42317861LL + seed * 11LL;
        return seed >> 16;
    }

    /**
     * Floor function - returns largest integer <= x
     * Reference: Mth.java floor methods
     */
    static inline int32_t floor(double x) {
        int32_t i = static_cast<int32_t>(x);
        return x < i ? i - 1 : i;
    }

    /**
     * Long floor function - returns largest long <= x
     * Reference: Mth.java lines 71-74
     */
    static inline int64_t lfloor(double x) {
        int64_t i = static_cast<int64_t>(x);
        return x < static_cast<double>(i) ? i - 1L : i;
    }

    /**
     * Linear interpolation between two values
     * Reference: Mth.java line 557-558
     */
    static inline double lerp(double alpha, double p0, double p1) {
        return p0 + alpha * (p1 - p0);
    }

    /**
     * Clamped linear interpolation - clamps factor to [0, 1]
     * Reference: Mth.java lines 115-121
     */
    static inline double clampedLerp(double factor, double min, double max) {
        if (factor < 0.0) {
            return min;
        }
        return factor > 1.0 ? max : lerp(factor, min, max);
    }

    /**
     * Bilinear interpolation (2D)
     * Reference: Mth.java line 560-562
     */
    static inline double lerp2(double alpha1, double alpha2, double x00, double x10, double x01, double x11) {
        return lerp(alpha2, lerp(alpha1, x00, x10), lerp(alpha1, x01, x11));
    }

    /**
     * Trilinear interpolation (3D)
     * Reference: Mth.java line 564-566
     */
    static inline double lerp3(double alpha1, double alpha2, double alpha3,
                               double x000, double x100, double x010, double x110,
                               double x001, double x101, double x011, double x111) {
        return lerp(alpha3,
                    lerp2(alpha1, alpha2, x000, x100, x010, x110),
                    lerp2(alpha1, alpha2, x001, x101, x011, x111));
    }

    /**
     * Smoothstep function - smooth interpolation curve
     * Reference: Mth.java line 572-574
     * Formula: x^3 * (x * (6x - 15) + 10)
     */
    static inline double smoothstep(double x) {
        return x * x * x * (x * (x * static_cast<double>(6.0F) - static_cast<double>(15.0F)) + static_cast<double>(10.0F));
    }

    /**
     * Derivative of smoothstep function
     * Reference: Mth.java line 576-578
     * Formula: 30 * x^2 * (x - 1)^2
     */
    static inline double smoothstepDerivative(double x) {
        return static_cast<double>(30.0F) * x * x * (x - static_cast<double>(1.0F)) * (x - static_cast<double>(1.0F));
    }

    /**
     * Clamp a value between min and max
     * Reference: Mth.java line 18
     */
    static inline double clamp(double value, double min, double max) {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }

    /**
     * Clamp an integer value between min and max
     * Reference: Mth.java line 22
     */
    static inline int32_t clamp(int32_t value, int32_t min, int32_t max) {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }

    /**
     * Clamp a float value between min and max
     * Reference: Mth.java line 20
     */
    static inline float clamp(float value, float min, float max) {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }

    /**
     * Map a value from [srcMin, srcMax] to [dstMin, dstMax] (without clamping)
     * Reference: Mth.java line 127
     */
    static inline double map(double value, double srcMin, double srcMax, double dstMin, double dstMax) {
        return (value - srcMin) / (srcMax - srcMin) * (dstMax - dstMin) + dstMin;
    }

    /**
     * Clamp and map a value from [srcMin, srcMax] to [dstMin, dstMax]
     * Reference: Mth.java line 123
     */
    static inline double clampedMap(double value, double srcMin, double srcMax, double dstMin, double dstMax) {
        return clamp((value - srcMin) / (srcMax - srcMin), 0.0, 1.0) * (dstMax - dstMin) + dstMin;
    }

    // Float overloads matching Java's Mth float methods.
    // CRITICAL: Java has separate float implementations that compute entirely
    // in float precision. Using double would produce different results at boundaries.
    static float inverseLerp(float value, float min, float max) {
        return (value - min) / (max - min);
    }
    static float clampedLerp(float factor, float min, float max) {
        if (factor < 0.0f) return min;
        return factor > 1.0f ? max : min + factor * (max - min);
    }
    static float clampedMap(float value, float fromMin, float fromMax, float toMin, float toMax) {
        return clampedLerp(inverseLerp(value, fromMin, fromMax), toMin, toMax);
    }

    /**
     * Quantize a value to the nearest multiple of divisor
     * Reference: Mth.java line 147
     *
     * Example: quantize(7.8, 3) = 9 (nearest multiple of 3)
     */
    static int32_t quantize(double value, int32_t divisor);

    /**
     * Floor division (Java's Math.floorDiv)
     * Reference: Java's Math.floorDiv
     * Returns the largest integer less than or equal to the algebraic quotient
     */
    static inline int32_t floorDiv(int32_t x, int32_t y) {
        int32_t quotient = x / y;
        if ((x ^ y) < 0 && (quotient * y != x)) {
            quotient--;
        }
        return quotient;
    }

    /**
     * Floor modulus (Java's Math.floorMod)
     * Reference: Java's Math.floorMod
     * Returns x - floorDiv(x, y) * y
     * Always returns a value with the same sign as y (unlike C++ %)
     */
    static int32_t floorMod(int32_t x, int32_t y) {
        return x - floorDiv(x, y) * y;
    }

    /**
     * Return maximum of absolute values
     * Reference: Mth.java lines 131-141
     */
    static int32_t absMax(int32_t a, int32_t b);
    static float absMax(float a, float b);
    static double absMax(double a, double b);

    /**
     * Chessboard distance (Chebyshev distance)
     * Reference: Mth.java lines 143-145
     *
     * Returns max(|x1-x0|, |z1-z0|)
     */
    static int32_t chessboardDistance(int32_t x0, int32_t z0, int32_t x1, int32_t z1);

    /**
     * Ceiling of log base 2
     * Reference: Mth.java line 100-102
     *
     * Returns smallest integer n such that 2^n >= value
     */
    static int32_t ceillog2(int32_t value);

    /**
     * Square a value
     * Reference: Mth.java line 34-36
     */
    static inline double square(double x) {
        return x * x;
    }

    static inline float square(float x) {
        return x * x;
    }

    static inline int32_t square(int32_t x) {
        return x * x;
    }

    static inline int64_t square(int64_t x) {
        return x * x;
    }

    /**
     * Square root of a float value
     * Reference: Mth.java line 38
     */
    static float sqrt(float x);

    /**
     * Fast inverse square root approximation
     * Reference: Mth.java fastInvSqrt
     * Returns 1.0 / sqrt(x) approximately
     */
    static double fastInvSqrt(double x);

    /**
     * Calculate length (magnitude) of a 3D vector
     * Reference: Mth.java length method
     */
    static double length(double x, double y, double z);

    /**
     * Calculate squared length (magnitude squared) of a 3D vector
     * Reference: Mth.java lengthSquared method
     */
    static inline double lengthSquared(double x, double y, double z) {
        return x * x + y * y + z * z;
    }

    /**
     * Generate a random integer between min and maxInclusive (inclusive)
     * Reference: Mth.java lines 660-662
     *
     * @param random Random source
     * @param min Minimum value (inclusive)
     * @param maxInclusive Maximum value (inclusive)
     * @return Random integer in range [min, maxInclusive]
     */
    template<typename RandomSource>
    static int32_t randomBetweenInclusive(RandomSource& random, int32_t min, int32_t maxInclusive) {
        return random.nextInt(maxInclusive - min + 1) + min;
    }

    /**
     * Generate a random integer between min and max (inclusive), with edge case handling
     * Reference: Mth.java nextInt() method
     *
     * CRITICAL: Returns min if min >= max (no random consumption in that case)
     * This is different from randomBetweenInclusive which doesn't handle this edge case.
     *
     * @param random Random source
     * @param min Minimum value (inclusive)
     * @param max Maximum value (inclusive)
     * @return Random integer in range [min, max], or min if min >= max
     */
    template<typename RandomSource>
    static int32_t nextInt(RandomSource& random, int32_t min, int32_t max) {
        // Reference: Mth.java nextInt()
        // return min >= max ? min : random.nextInt(max - min + 1) + min;
        return min >= max ? min : random.nextInt(max - min + 1) + min;
    }

    /**
     * Generate a random float between min and maxExclusive
     * Reference: Mth.java lines 664-666
     */
    template<typename RandomSource>
    static float randomBetween(RandomSource& random, float min, float maxExclusive) {
        return random.nextFloat() * (maxExclusive - min) + min;
    }

    //==========================================================================
    // POSITION COORDINATE UTILITIES
    //==========================================================================

    /**
     * Convert block coordinate to quart position (divide by 4)
     * Reference: QuartPos.java line 12-14
     */
    static inline int32_t quartFromBlock(int32_t blockCoord) {
        return blockCoord >> 2;
    }

    /**
     * Convert quart position to block coordinate (multiply by 4)
     * Reference: QuartPos.java line 20-22
     */
    static inline int32_t quartToBlock(int32_t quart) {
        return quart << 2;
    }

    /**
     * Convert block coordinate to section coordinate (divide by 16)
     * Reference: SectionPos.java line 80-82
     */
    static inline int32_t blockToSectionCoord(int32_t blockCoord) {
        return blockCoord >> 4;
    }

    /**
     * Pack X and Z coordinates into a single long for cache keys
     * Reference: ColumnPos.java line 18-20
     */
    static inline int64_t columnPosAsLong(int32_t x, int32_t z) {
        return (static_cast<int64_t>(x) & 0xFFFFFFFFL) |
               ((static_cast<int64_t>(z) & 0xFFFFFFFFL) << 32);
    }

    /**
     * Extract X coordinate from packed column position
     * Reference: ColumnPos.java line 22-24
     */
    static inline int32_t columnPosGetX(int64_t pos) {
        return static_cast<int32_t>(pos & 0xFFFFFFFFL);
    }

    /**
     * Extract Z coordinate from packed column position
     * Reference: ColumnPos.java line 26-28
     */
    static inline int32_t columnPosGetZ(int64_t pos) {
        return static_cast<int32_t>((pos >> 32) & 0xFFFFFFFFL);
    }
};

} // namespace minecraft

#endif // MTH_H
