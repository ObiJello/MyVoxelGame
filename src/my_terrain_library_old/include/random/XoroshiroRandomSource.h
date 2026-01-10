#ifndef XOROSHIRORANDOMSOURCE_H
#define XOROSHIRORANDOMSOURCE_H

#include "random/Xoroshiro128PlusPlus.h"
#include "random/RandomSupport.h"
#include "random/MarsagliaPolarGaussian.h"
#include <cstdint>
#include <string>

namespace minecraft {

// Forward declaration
class XoroshiroPositionalRandomFactory;

/**
 * Main random number generator for Minecraft Overworld terrain generation.
 * Wraps Xoroshiro128PlusPlus and provides various random number generation methods.
 *
 * Reference: /minecraft/world/level/levelgen/XoroshiroRandomSource.java
 */
class XoroshiroRandomSource {
public:
    // Constants from XoroshiroRandomSource.java lines 9-10
    static constexpr float FLOAT_UNIT = 5.9604645E-8F;
    // CRITICAL: Java uses (double)1.110223E-16F - float literal cast to double!
    static constexpr double DOUBLE_UNIT = static_cast<double>(1.110223E-16F);

    /**
     * Construct from a 64-bit seed.
     * The seed is automatically upgraded to 128-bit using Stafford13 mixing.
     * Reference: XoroshiroRandomSource.java lines 15-16
     */
    explicit XoroshiroRandomSource(int64_t seed);

    /**
     * Construct from a 128-bit seed.
     * Reference: XoroshiroRandomSource.java lines 19-21
     */
    XoroshiroRandomSource(const Seed128bit& seed);

    /**
     * Construct from explicit 128-bit seed components.
     * Reference: XoroshiroRandomSource.java lines 23-25
     */
    XoroshiroRandomSource(uint64_t seedLo, uint64_t seedHi);

    /**
     * Fork this random source to create a new independent one.
     * Reference: XoroshiroRandomSource.java lines 31-33
     */
    XoroshiroRandomSource fork();

    /**
     * Create a positional random factory for generating position/string-based randoms.
     * Reference: XoroshiroRandomSource.java lines 35-37
     */
    XoroshiroPositionalRandomFactory forkPositional();

    /**
     * Reset the generator with a new seed.
     * Reference: XoroshiroRandomSource.java lines 39-42
     */
    void setSeed(int64_t seed);

    /**
     * Generate a random 32-bit integer.
     * Reference: XoroshiroRandomSource.java lines 44-46
     */
    int32_t nextInt();

    /**
     * Generate a random integer in range [0, bound).
     * Uses modern bounded random algorithm with rejection sampling.
     * Reference: XoroshiroRandomSource.java lines 48-65
     */
    int32_t nextInt(int32_t bound);

    /**
     * Generate a random integer in range [min, max] inclusive.
     * Reference: RandomSource.nextIntBetweenInclusive()
     */
    int32_t nextIntBetweenInclusive(int32_t min, int32_t max) {
        return min + nextInt(max - min + 1);
    }

    /**
     * Generate a random 64-bit integer.
     * Reference: XoroshiroRandomSource.java lines 67-69
     */
    int64_t nextLong();

    /**
     * Generate a random boolean.
     * Reference: XoroshiroRandomSource.java lines 71-73
     */
    bool nextBoolean();

    /**
     * Generate a random float in range [0, 1).
     * Reference: XoroshiroRandomSource.java lines 75-77
     */
    float nextFloat();

    /**
     * Generate a random double in range [0, 1).
     * Reference: XoroshiroRandomSource.java lines 79-81
     */
    double nextDouble();

    /**
     * Generate a gaussian-distributed random number.
     * Reference: XoroshiroRandomSource.java lines 83-85
     */
    double nextGaussian();

    /**
     * Consume a number of random values.
     * CRITICAL: This calls randomNumberGenerator.nextLong(), NOT nextInt()!
     * Reference: XoroshiroRandomSource.java lines 87-92
     */
    void consumeCount(int32_t rounds);

    // For testing/debugging
    const Xoroshiro128PlusPlus& getGenerator() const { return generator; }

private:
    Xoroshiro128PlusPlus generator;

    /**
     * Gaussian source for nextGaussian().
     * Reference: XoroshiroRandomSource.java line 12
     * Note: Initialized with 'this' pointer in constructor.
     */
    MarsagliaPolarGaussian gaussianSource;

    /**
     * Constructor from existing generator (used by fork()).
     */
    explicit XoroshiroRandomSource(const Xoroshiro128PlusPlus& gen);

    /**
     * Get the next N bits from the generator.
     * Reference: XoroshiroRandomSource.java lines 94-96
     */
    uint64_t nextBits(int bits);
};

/**
 * Factory for creating random sources based on position or string identifiers.
 * This is a nested class in Java (XoroshiroRandomSource.XoroshiroPositionalRandomFactory).
 *
 * Reference: XoroshiroRandomSource.java lines 102-130
 */
class XoroshiroPositionalRandomFactory {
public:
    /**
     * Construct factory with a 128-bit base seed.
     * Reference: XoroshiroRandomSource.java lines 106-109
     */
    XoroshiroPositionalRandomFactory(uint64_t seedLo, uint64_t seedHi);

    /**
     * Create a random source for a specific (x, y, z) position.
     * Reference: XoroshiroRandomSource.java lines 111-115
     */
    XoroshiroRandomSource at(int32_t x, int32_t y, int32_t z);

    /**
     * Create a random source from a string identifier using MD5 hash.
     * CRITICAL: This is how Minecraft generates octave randoms ("octave_0", etc.)
     * Reference: XoroshiroRandomSource.java lines 117-120
     */
    XoroshiroRandomSource fromHashOf(const std::string& name);

    /**
     * Create a random source from a 64-bit seed.
     * Reference: XoroshiroRandomSource.java lines 122-124
     */
    XoroshiroRandomSource fromSeed(int64_t seed);

private:
    uint64_t seedLo;
    uint64_t seedHi;
};

} // namespace minecraft

#endif // XOROSHIRORANDOMSOURCE_H
