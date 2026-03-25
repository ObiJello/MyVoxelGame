#pragma once

#include "random/XoroshiroRandomSource.h"
#include "random/LegacyRandomSource.h"
#include <cstdint>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

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
    Algorithm m_algorithm;
    XoroshiroRandomSource m_xoroshiroRandomSource;
    LegacyRandomSource m_legacyRandomSource;
    int32_t m_count;

    // Gaussian cache matching Java's LegacyRandomSource.gaussianSource
    // CRITICAL: In Java, WorldgenRandom extends LegacyRandomSource which has
    // its own MarsagliaPolarGaussian(this). WorldgenRandom.setSeed() does NOT
    // call super.setSeed(), so the LegacyRandomSource gaussian cache is NEVER
    // reset by setSeed/setFeatureSeed. It persists across features.
    double m_nextNextGaussian = 0.0;
    bool m_haveNextNextGaussian = false;

public:
    struct DebugStateSnapshot {
        Algorithm algorithm;
        uint64_t seedLo;
        uint64_t seedHi;
        int64_t legacySeed;
        int32_t count;
        double nextNextGaussian;
        bool haveNextNextGaussian;
    };

    /**
     * Constructor
     * Reference: WorldgenRandom.java lines 10-13
     */
    explicit WorldgenRandom(XoroshiroRandomSource randomSource)
        : m_algorithm(Algorithm::XOROSHIRO)
        , m_xoroshiroRandomSource(std::move(randomSource))
        , m_legacyRandomSource(0L)
        , m_count(0)
        , m_nextNextGaussian(0.0)
        , m_haveNextNextGaussian(false)
    {}

    explicit WorldgenRandom(LegacyRandomSource randomSource)
        : m_algorithm(Algorithm::LEGACY)
        , m_xoroshiroRandomSource(0L)
        , m_legacyRandomSource(std::move(randomSource))
        , m_count(0)
        , m_nextNextGaussian(0.0)
        , m_haveNextNextGaussian(false)
    {}

    /**
     * Create from seed with specified algorithm
     * Reference: WorldgenRandom.java Algorithm.newInstance()
     */
    explicit WorldgenRandom(int64_t seed)
        : m_algorithm(Algorithm::XOROSHIRO)
        , m_xoroshiroRandomSource(seed)
        , m_legacyRandomSource(0L)
        , m_count(0)
        , m_nextNextGaussian(0.0)
        , m_haveNextNextGaussian(false)
    {}

    /**
     * Get the count of random values generated
     * Reference: WorldgenRandom.java lines 15-17
     */
    int32_t getCount() const {
        return m_count;
    }

    /**
     * Check if there's a cached gaussian value (for debugging)
     */
    bool hasNextGaussian() const {
        return m_haveNextNextGaussian;
    }

    /**
     * Fork this random source
     * Reference: WorldgenRandom.java lines 19-21
     */
    WorldgenRandom fork() {
        return usesLegacyRandomSource()
            ? WorldgenRandom(m_legacyRandomSource.fork())
            : WorldgenRandom(m_xoroshiroRandomSource.fork());
    }

    /**
     * Fork positional random factory
     * Reference: WorldgenRandom.java lines 23-25
     */
    XoroshiroPositionalRandomFactory forkPositional() {
        if (usesLegacyRandomSource()) {
            throw std::logic_error("Legacy-backed WorldgenRandom does not expose Xoroshiro positional random factory");
        }
        return m_xoroshiroRandomSource.forkPositional();
    }

    LegacyPositionalRandomFactory forkLegacyPositional() {
        if (!usesLegacyRandomSource()) {
            throw std::logic_error("Xoroshiro-backed WorldgenRandom does not expose legacy positional random factory");
        }
        return m_legacyRandomSource.forkPositional();
    }

    bool usesLegacyRandomSource() const {
        return m_algorithm == Algorithm::LEGACY;
    }

    /**
     * Set the seed
     * Reference: WorldgenRandom.java lines 37-41
     */
    void setSeed(int64_t seed) {
        if (usesLegacyRandomSource()) {
            m_legacyRandomSource.setSeed(seed);
        } else {
            m_xoroshiroRandomSource.setSeed(seed);
        }
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
    // CRITICAL: WorldgenRandom extends LegacyRandomSource which implements BitRandomSource!
    // All random methods (nextInt, nextFloat, nextDouble, etc.) come from BitRandomSource defaults
    // which use next(bits). WorldgenRandom.next() delegates to the underlying source.
    // Reference: WorldgenRandom.java, BitRandomSource.java

    /**
     * Generate next bits (internal method matching Java)
     * Reference: WorldgenRandom.java next() - delegates to underlying source
     * For XoroshiroRandomSource: (int)(randomSource.nextLong() >>> (64 - bits))
     */
    int32_t next(int32_t bits) {
        ++m_count;
        if (usesLegacyRandomSource()) {
            return m_legacyRandomSource.next(bits);
        }

        int64_t raw = m_xoroshiroRandomSource.nextLong();
        return static_cast<int32_t>(static_cast<uint64_t>(raw) >> (64 - bits));
    }

    /**
     * Generate next random int (uses next(32))
     * Reference: BitRandomSource.java nextInt()
     */
    int32_t nextInt() {
        return next(32);
    }

    /**
     * Generate next random int in range [0, bound)
     * Reference: BitRandomSource.java nextInt(bound)
     */
    int32_t nextInt(int32_t bound) {
        if (bound <= 0) {
            throw std::invalid_argument("Bound must be positive");
        }
        if ((bound & (bound - 1)) == 0) {
            // Power of 2 - fast path
            return static_cast<int32_t>((static_cast<int64_t>(bound) * static_cast<int64_t>(next(31))) >> 31);
        }

        // Java's BitRandomSource.nextInt(int) relies on 32-bit signed wraparound
        // in the rejection check: sample - modulo + (bound - 1) < 0.
        int32_t sample;
        int32_t modulo;
        do {
            sample = next(31);
            modulo = sample % bound;
        } while (static_cast<int32_t>(
            static_cast<uint32_t>(sample)
            - static_cast<uint32_t>(modulo)
            + static_cast<uint32_t>(bound - 1)
        ) < 0);

        return modulo;
    }

    /**
     * Generate random int in range [min, max] inclusive
     * Reference: RandomSource.nextIntBetweenInclusive()
     */
    int32_t nextIntBetweenInclusive(int32_t min, int32_t max) {
        return min + nextInt(max - min + 1);
    }

    /**
     * Generate next random long
     * Reference: BitRandomSource.java nextLong()
     * CRITICAL: Uses 2 calls to next(32), NOT direct delegation!
     */
    int64_t nextLong() {
        int32_t high = next(32);
        int32_t low = next(32);
        return (static_cast<int64_t>(high) << 32) + static_cast<int64_t>(low);
    }

    /**
     * Generate next random boolean
     * Reference: BitRandomSource.java nextBoolean()
     */
    bool nextBoolean() {
        return next(1) != 0;
    }

    /**
     * Generate next random float in [0, 1)
     * Reference: BitRandomSource.java nextFloat()
     * Uses: (float)next(24) * FLOAT_MULTIPLIER (5.9604645E-8F)
     */
    float nextFloat() {
        return static_cast<float>(next(24)) * 5.9604645E-8f;
    }

    /**
     * Generate next random double in [0, 1)
     * Reference: BitRandomSource.java nextDouble()
     * CRITICAL: Uses 2 calls - next(26) and next(27)!
     * Java: ((long)next(26) << 27) + next(27)) * DOUBLE_MULTIPLIER
     */
    double nextDouble() {
        int32_t high = next(26);
        int32_t low = next(27);
        int64_t combined = (static_cast<int64_t>(high) << 27) + static_cast<int64_t>(low);
        return static_cast<double>(combined) * 0x1.0p-53;
    }

    /**
     * Generate next gaussian-distributed value using Marsaglia polar method.
     *
     * CRITICAL: In Java, WorldgenRandom extends LegacyRandomSource which has
     * its own MarsagliaPolarGaussian instance. This gaussian source:
     * 1. Uses LegacyRandomSource (=WorldgenRandom) as its RandomSource,
     *    so nextDouble() goes through BitRandomSource.nextDouble() which calls
     *    next(26) + next(27) = TWO Xoroshiro128PlusPlus.nextLong() calls.
     * 2. Is NEVER reset by WorldgenRandom.setSeed() / setFeatureSeed() because
     *    WorldgenRandom.setSeed() overrides LegacyRandomSource.setSeed() without
     *    calling super.setSeed(). So the gaussian cache persists across features.
     *
     * Reference: LegacyRandomSource.java line 15: gaussianSource = new MarsagliaPolarGaussian(this)
     * Reference: WorldgenRandom.java setSeed() - does NOT call super.setSeed()
     * Reference: MarsagliaPolarGaussian.java nextGaussian() lines 19-38
     */
    double nextGaussian() {
        if (m_haveNextNextGaussian) {
            m_haveNextNextGaussian = false;
            return m_nextNextGaussian;
        }

        double x, y, radiusSquared;
        do {
            // Use BitRandomSource.nextDouble() pattern: next(26) + next(27) = 2 nextLong calls
            // Reference: BitRandomSource.java nextDouble()
            x = static_cast<double>(2.0F) * nextDouble() - static_cast<double>(1.0F);
            y = static_cast<double>(2.0F) * nextDouble() - static_cast<double>(1.0F);
            radiusSquared = x * x + y * y;
        } while (radiusSquared >= static_cast<double>(1.0F) || radiusSquared == static_cast<double>(0.0F));

        double multiplier = std::sqrt(static_cast<double>(-2.0F) * std::log(radiusSquared) / radiusSquared);
        m_nextNextGaussian = y * multiplier;
        m_haveNextNextGaussian = true;
        return x * multiplier;
    }

    /**
     * Consume count of random values
     * Note: Each "count" in WorldgenRandom consumes one underlying nextLong()
     * through the next(32) method, which increments m_count
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
        if (usesLegacyRandomSource()) {
            throw std::logic_error("Legacy-backed WorldgenRandom does not expose Xoroshiro random source");
        }
        return m_xoroshiroRandomSource;
    }

    const XoroshiroRandomSource& getRandomSource() const {
        if (usesLegacyRandomSource()) {
            throw std::logic_error("Legacy-backed WorldgenRandom does not expose Xoroshiro random source");
        }
        return m_xoroshiroRandomSource;
    }

    LegacyRandomSource& getLegacyRandomSource() {
        if (!usesLegacyRandomSource()) {
            throw std::logic_error("Xoroshiro-backed WorldgenRandom does not expose legacy random source");
        }
        return m_legacyRandomSource;
    }

    const LegacyRandomSource& getLegacyRandomSource() const {
        if (!usesLegacyRandomSource()) {
            throw std::logic_error("Xoroshiro-backed WorldgenRandom does not expose legacy random source");
        }
        return m_legacyRandomSource;
    }

    /**
     * Get current seed state for debugging/comparison with Java
     * Returns seedLo and seedHi from the underlying Xoroshiro128PlusPlus
     */
    void getSeedState(uint64_t& seedLo, uint64_t& seedHi) const {
        if (usesLegacyRandomSource()) {
            seedLo = static_cast<uint64_t>(m_legacyRandomSource.getInternalSeed());
            seedHi = 0;
            return;
        }

        const auto& gen = m_xoroshiroRandomSource.getGenerator();
        seedLo = gen.getSeedLo();
        seedHi = gen.getSeedHi();
    }

    DebugStateSnapshot captureDebugState() const {
        uint64_t seedLo = 0;
        uint64_t seedHi = 0;
        getSeedState(seedLo, seedHi);
        return DebugStateSnapshot{
            m_algorithm,
            seedLo,
            seedHi,
            m_legacyRandomSource.getInternalSeed(),
            m_count,
            m_nextNextGaussian,
            m_haveNextNextGaussian
        };
    }

    void restoreDebugState(const DebugStateSnapshot& snapshot) {
        m_algorithm = snapshot.algorithm;
        if (snapshot.algorithm == Algorithm::LEGACY) {
            m_legacyRandomSource.setInternalSeedDirectly(snapshot.legacySeed);
        } else {
            m_xoroshiroRandomSource.setStateDirectly(snapshot.seedLo, snapshot.seedHi);
        }
        m_count = snapshot.count;
        m_nextNextGaussian = snapshot.nextNextGaussian;
        m_haveNextNextGaussian = snapshot.haveNextNextGaussian;
    }

    /**
     * Set only the xoroshiro state directly.
     * This exists for low-level debugging; use restoreDebugState() when exact replay matters.
     */
    void setSeedState(uint64_t seedLo, uint64_t seedHi) {
        if (usesLegacyRandomSource()) {
            (void)seedHi;
            m_legacyRandomSource.setInternalSeedDirectly(static_cast<int64_t>(seedLo));
        } else {
            m_xoroshiroRandomSource.setStateDirectly(seedLo, seedHi);
        }
    }
};

} // namespace levelgen
} // namespace minecraft
