#pragma once

#include "random/XoroshiroRandomSource.h"
#include "random/LegacyRandomSource.h"
#include <cstdint>
#include <functional>
#include <memory>

// Forward declare for return type
namespace minecraft {
    class LegacyRandomSource;
}

// Reference: net/minecraft/world/level/levelgen/WorldgenRandom.java

namespace minecraft {
namespace levelgen {

/**
 * WorldgenRandom - Random source with world generation seed derivation methods
 * Reference: WorldgenRandom.java
 */
class WorldgenRandom {
public:
    /**
     * Algorithm - Random source algorithm types
     * Reference: WorldgenRandom.java lines 74-91
     */
    enum class Algorithm {
        LEGACY,
        XOROSHIRO
    };

private:
    XoroshiroRandomSource m_randomSource;
    int32_t m_count;

public:
    /**
     * Constructor
     * Reference: WorldgenRandom.java lines 10-13
     */
    explicit WorldgenRandom(XoroshiroRandomSource randomSource)
        : m_randomSource(std::move(randomSource))
        , m_count(0)
    {}

    /**
     * Create from seed with specified algorithm
     * Reference: WorldgenRandom.java Algorithm.newInstance()
     */
    explicit WorldgenRandom(int64_t seed)
        : m_randomSource(seed)
        , m_count(0)
    {}

    /**
     * Get the count of random values generated
     * Reference: WorldgenRandom.java lines 15-17
     */
    int32_t getCount() const {
        return m_count;
    }

    /**
     * Fork this random source
     * Reference: WorldgenRandom.java lines 19-21
     */
    XoroshiroRandomSource fork() {
        return m_randomSource.fork();
    }

    /**
     * Fork positional random factory
     * Reference: WorldgenRandom.java lines 23-25
     */
    XoroshiroPositionalRandomFactory forkPositional() {
        return m_randomSource.forkPositional();
    }

    /**
     * Set the seed
     * Reference: WorldgenRandom.java lines 37-41
     */
    void setSeed(int64_t seed) {
        m_randomSource.setSeed(seed);
    }

    /**
     * Set decoration seed for chunk-based feature generation
     * Reference: WorldgenRandom.java lines 43-50
     */
    int64_t setDecorationSeed(int64_t seed, int32_t chunkX, int32_t chunkZ) {
        setSeed(seed);
        int64_t xScale = nextLong() | 1L;
        int64_t zScale = nextLong() | 1L;
        int64_t result = static_cast<int64_t>(chunkX) * xScale + static_cast<int64_t>(chunkZ) * zScale ^ seed;
        setSeed(result);
        return result;
    }

    /**
     * Set feature seed for indexed feature generation
     * Reference: WorldgenRandom.java lines 52-55
     */
    void setFeatureSeed(int64_t seed, int32_t index, int32_t step) {
        int64_t result = seed + static_cast<int64_t>(index) + static_cast<int64_t>(10000 * step);
        setSeed(result);
    }

    /**
     * Set large feature seed for structures
     * Reference: WorldgenRandom.java lines 57-63
     */
    void setLargeFeatureSeed(int64_t seed, int32_t chunkX, int32_t chunkZ) {
        setSeed(seed);
        int64_t xScale = nextLong();
        int64_t zScale = nextLong();
        int64_t result = static_cast<int64_t>(chunkX) * xScale ^ static_cast<int64_t>(chunkZ) * zScale ^ seed;
        setSeed(result);
    }

    /**
     * Set large feature seed with salt
     * Reference: WorldgenRandom.java lines 65-68
     */
    void setLargeFeatureWithSalt(int64_t seed, int32_t x, int32_t z, int32_t blend) {
        int64_t result = static_cast<int64_t>(x) * 341873128712L +
                         static_cast<int64_t>(z) * 132897987541L +
                         seed +
                         static_cast<int64_t>(blend);
        setSeed(result);
    }

    /**
     * Create a random source for slime chunk detection
     * Reference: WorldgenRandom.java lines 70-72
     * IMPORTANT: Returns LegacyRandomSource (java.util.Random), NOT XoroshiroRandomSource
     * Java: RandomSource.create() returns LegacyRandomSource
     */
    static LegacyRandomSource seedSlimeChunk(int32_t x, int32_t z, int64_t seed, int64_t salt) {
        return LegacyRandomSource(
            (seed +
            static_cast<int64_t>(x * x * 4987142) +
            static_cast<int64_t>(x * 5947611) +
            static_cast<int64_t>(z * z) * 4392871L +
            static_cast<int64_t>(z * 389711)) ^ salt
        );
    }

    // Random number generation methods
    // CRITICAL: WorldgenRandom extends LegacyRandomSource in Java!
    // LegacyRandomSource.nextLong() uses 2 calls to next(32)
    // WorldgenRandom.next(bits) uses randomSource.nextLong() >>> (64 - bits)
    // Reference: WorldgenRandom.java lines 27-35, LegacyRandomSource.java

    /**
     * Generate next bits (internal method matching Java)
     * Reference: WorldgenRandom.java lines 27-35
     * For XoroshiroRandomSource: (int)(randomSource.nextLong() >>> (64 - bits))
     */
    int32_t next(int32_t bits) {
        ++m_count;
        int64_t raw = m_randomSource.nextLong();
        return static_cast<int32_t>(static_cast<uint64_t>(raw) >> (64 - bits));
    }

    /**
     * Generate next random int (uses next(32))
     * Reference: LegacyRandomSource.java nextInt()
     */
    int32_t nextInt() {
        return next(32);
    }

    /**
     * Generate next random int in range [0, bound)
     * Reference: LegacyRandomSource.java nextInt(bound)
     */
    int32_t nextInt(int32_t bound) {
        if (bound <= 0) return 0;
        if ((bound & (bound - 1)) == 0) {
            // Power of 2 - fast path
            return static_cast<int32_t>((static_cast<int64_t>(bound) * static_cast<int64_t>(next(31))) >> 31);
        }
        // General case
        int32_t bits, val;
        do {
            bits = next(31);
            val = bits % bound;
        } while (bits - val + (bound - 1) < 0);
        return val;
    }

    /**
     * Generate next random long
     * Reference: LegacyRandomSource.java nextLong()
     * CRITICAL: Uses 2 calls to next(32), NOT direct delegation!
     */
    int64_t nextLong() {
        int32_t high = next(32);
        int32_t low = next(32);
        return (static_cast<int64_t>(high) << 32) + static_cast<int64_t>(low);
    }

    /**
     * Generate next random boolean
     * Reference: LegacyRandomSource.java nextBoolean()
     */
    bool nextBoolean() {
        return next(1) != 0;
    }

    /**
     * Generate next random float in [0, 1)
     * Reference: LegacyRandomSource.java nextFloat()
     */
    float nextFloat() {
        return static_cast<float>(next(24)) / static_cast<float>(1 << 24);
    }

    /**
     * Generate next random double in [0, 1)
     * Reference: LegacyRandomSource.java nextDouble()
     */
    double nextDouble() {
        int64_t high = static_cast<int64_t>(next(26));
        int64_t low = static_cast<int64_t>(next(27));
        return static_cast<double>((high << 27) + low) / static_cast<double>(1LL << 53);
    }

    /**
     * Generate next gaussian-distributed value
     * This would need full BoxMuller implementation
     */
    double nextGaussian() {
        // For now, delegate to underlying source
        // This is not used in seed derivation methods
        return m_randomSource.nextGaussian();
    }

    /**
     * Consume count of random values
     * Note: Each "count" in WorldgenRandom consumes one underlying nextLong()
     */
    void consumeCount(int32_t rounds) {
        for (int32_t i = 0; i < rounds; ++i) {
            next(32);  // This will properly increment m_count and consume
        }
    }

    /**
     * Get the underlying random source
     */
    XoroshiroRandomSource& getRandomSource() {
        return m_randomSource;
    }

    const XoroshiroRandomSource& getRandomSource() const {
        return m_randomSource;
    }
};

} // namespace levelgen
} // namespace minecraft
