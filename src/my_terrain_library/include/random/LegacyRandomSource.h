#pragma once

#include <cstdint>
#include <cmath>

namespace minecraft {

/**
 * LegacyRandomSource - Java LCG Random implementation
 * Java: net/minecraft/world/level/levelgen/LegacyRandomSource.java
 *
 * This implements Java's java.util.Random algorithm (Linear Congruential Generator)
 * which is used for legacy Minecraft random operations including The End island generation.
 *
 * The algorithm uses:
 * - MULTIPLIER = 25214903917L
 * - INCREMENT = 11L
 * - MODULUS = 2^48 (48-bit state)
 *
 * Formula: seed = (seed * MULTIPLIER + INCREMENT) & MODULUS_MASK
 */
class LegacyRandomSource {
public:
    static constexpr int MODULUS_BITS = 48;
    static constexpr int64_t MODULUS_MASK = (1LL << MODULUS_BITS) - 1;  // 281474976710655L
    static constexpr int64_t MULTIPLIER = 25214903917LL;
    static constexpr int64_t INCREMENT = 11LL;

    /**
     * Construct with initial seed
     * Java line 17-19
     */
    explicit LegacyRandomSource(int64_t seed) {
        setSeed(seed);
    }

    /**
     * Set the seed (with XOR scramble)
     * Java line 29-35
     */
    void setSeed(int64_t seed) {
        m_seed = (seed ^ MULTIPLIER) & MODULUS_MASK;
    }

    /**
     * Generate next bits
     * Java line 37-40
     */
    int32_t next(int bits) {
        m_seed = (m_seed * MULTIPLIER + INCREMENT) & MODULUS_MASK;
        return static_cast<int32_t>(m_seed >> (MODULUS_BITS - bits));
    }

    /**
     * Generate next int
     */
    int32_t nextInt() {
        return next(32);
    }

    /**
     * Generate bounded int [0, bound)
     */
    int32_t nextInt(int32_t bound) {
        if (bound <= 0) return 0;

        // Handle power of 2 bounds more efficiently
        if ((bound & (bound - 1)) == 0) {
            return static_cast<int32_t>((static_cast<int64_t>(bound) * static_cast<int64_t>(next(31))) >> 31);
        }

        int32_t bits, val;
        do {
            bits = next(31);
            val = bits % bound;
        } while (bits - val + (bound - 1) < 0);

        return val;
    }

    /**
     * Generate next long
     */
    int64_t nextLong() {
        return (static_cast<int64_t>(next(32)) << 32) + next(32);
    }

    /**
     * Generate next float [0.0, 1.0)
     */
    float nextFloat() {
        return next(24) / static_cast<float>(1 << 24);
    }

    /**
     * Generate next double [0.0, 1.0)
     */
    double nextDouble() {
        return ((static_cast<int64_t>(next(26)) << 27) + next(27)) / static_cast<double>(1LL << 53);
    }

    /**
     * Consume count random values (call next() count times)
     * Java: consumeCount() line ~45
     */
    void consumeCount(int count) {
        for (int i = 0; i < count; ++i) {
            next(1);
        }
    }

    /**
     * Fork to create a new random source
     */
    LegacyRandomSource fork() {
        return LegacyRandomSource(nextLong());
    }

    /**
     * Generate next gaussian-distributed double
     * Uses Marsaglia polar method
     * Reference: java.util.Random.nextGaussian()
     */
    double nextGaussian() {
        if (m_haveNextNextGaussian) {
            m_haveNextNextGaussian = false;
            return m_nextNextGaussian;
        }

        double v1, v2, s;
        do {
            v1 = 2.0 * nextDouble() - 1.0;
            v2 = 2.0 * nextDouble() - 1.0;
            s = v1 * v1 + v2 * v2;
        } while (s >= 1.0 || s == 0.0);

        double multiplier = std::sqrt(-2.0 * std::log(s) / s);
        m_nextNextGaussian = v2 * multiplier;
        m_haveNextNextGaussian = true;
        return v1 * multiplier;
    }

    /**
     * Set large feature seed for deterministic chunk features
     * Reference: WorldgenRandom.java setLargeFeatureSeed()
     *
     * This creates a deterministic seed based on the world seed and chunk position.
     * Used by carvers, decorators, and other large features.
     */
    void setLargeFeatureSeed(int64_t worldSeed, int32_t chunkX, int32_t chunkZ) {
        setSeed(worldSeed);
        int64_t xScale = nextLong();
        int64_t zScale = nextLong();
        int64_t result = static_cast<int64_t>(chunkX) * xScale ^ static_cast<int64_t>(chunkZ) * zScale ^ worldSeed;
        setSeed(result);
    }

private:
    int64_t m_seed;
    double m_nextNextGaussian = 0.0;
    bool m_haveNextNextGaussian = false;
};

} // namespace minecraft
