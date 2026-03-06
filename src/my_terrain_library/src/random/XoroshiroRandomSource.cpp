#include "random/XoroshiroRandomSource.h"
#include "math/Mth.h"
#include <stdexcept>

namespace minecraft {

// ============================================================================
// XoroshiroRandomSource Implementation
// ============================================================================

XoroshiroRandomSource::XoroshiroRandomSource(int64_t seed)
    : XoroshiroRandomSource(RandomSupport::upgradeSeedTo128bit(seed)) {
    // Reference: XoroshiroRandomSource.java lines 15-16
    // CRITICAL: Must call upgradeSeedTo128bit only ONCE
    // Delegates to Seed128bit constructor
}

XoroshiroRandomSource::XoroshiroRandomSource(const Seed128bit& seed)
    : generator(seed.seedLo, seed.seedHi)
    , gaussianSource(this) {
    // Reference: XoroshiroRandomSource.java lines 19-21
}

XoroshiroRandomSource::XoroshiroRandomSource(uint64_t seedLo, uint64_t seedHi)
    : generator(seedLo, seedHi)
    , gaussianSource(this) {
    // Reference: XoroshiroRandomSource.java lines 23-25
}

XoroshiroRandomSource::XoroshiroRandomSource(const Xoroshiro128PlusPlus& gen)
    : generator(gen)
    , gaussianSource(this) {
    // Private constructor for fork()
}

XoroshiroRandomSource XoroshiroRandomSource::fork() {
    // Reference: XoroshiroRandomSource.java lines 31-33
    uint64_t newLo = generator.nextLong();
    uint64_t newHi = generator.nextLong();
    return XoroshiroRandomSource(newLo, newHi);
}

XoroshiroPositionalRandomFactory XoroshiroRandomSource::forkPositional() {
    // Reference: XoroshiroRandomSource.java lines 35-37
    uint64_t lo = generator.nextLong();
    uint64_t hi = generator.nextLong();
    return XoroshiroPositionalRandomFactory(lo, hi);
}

void XoroshiroRandomSource::setSeed(int64_t seed) {
    // Reference: XoroshiroRandomSource.java lines 39-42
    Seed128bit seed128 = RandomSupport::upgradeSeedTo128bit(seed);
    generator = Xoroshiro128PlusPlus(seed128.seedLo, seed128.seedHi);
    gaussianSource.reset();
}

void XoroshiroRandomSource::setStateDirectly(uint64_t seedLo, uint64_t seedHi) {
    // Similar to setSeed() but with explicit seedLo/seedHi
    // Used for restoring state after tracing
    generator = Xoroshiro128PlusPlus(seedLo, seedHi);
    gaussianSource.reset();
}

int32_t XoroshiroRandomSource::nextInt() {
    // Reference: XoroshiroRandomSource.java lines 44-46
    return static_cast<int32_t>(generator.nextLong());
}

int32_t XoroshiroRandomSource::nextInt(int32_t bound) {
    // Reference: XoroshiroRandomSource.java lines 48-65
    // This is the modern bounded random algorithm using rejection sampling
    // to avoid modulo bias

    if (bound <= 0) {
        throw std::invalid_argument("Bound must be positive");
    }

    // Cast to unsigned to match Java's Integer.toUnsignedLong behavior
    uint64_t randomBits = static_cast<uint32_t>(nextInt());
    uint64_t multiplied = randomBits * static_cast<uint64_t>(bound);
    uint64_t fractional = multiplied & 0xFFFFFFFFULL;

    if (fractional < static_cast<uint64_t>(bound)) {
        // Rejection sampling to avoid bias
        // unbiasedBucketsStartIndex = Integer.remainderUnsigned(~bound + 1, bound)
        uint32_t threshold = static_cast<uint32_t>((~static_cast<uint32_t>(bound) + 1) % static_cast<uint32_t>(bound));

        while (fractional < static_cast<uint64_t>(threshold)) {
            randomBits = static_cast<uint32_t>(nextInt());
            multiplied = randomBits * static_cast<uint64_t>(bound);
            fractional = multiplied & 0xFFFFFFFFULL;
        }
    }

    // Return the integer part (upper 32 bits)
    return static_cast<int32_t>(multiplied >> 32);
}

int64_t XoroshiroRandomSource::nextLong() {
    // Reference: XoroshiroRandomSource.java lines 67-69
    return static_cast<int64_t>(generator.nextLong());
}

bool XoroshiroRandomSource::nextBoolean() {
    // Reference: XoroshiroRandomSource.java lines 71-73
    return (generator.nextLong() & 1) != 0;
}

float XoroshiroRandomSource::nextFloat() {
    // Reference: XoroshiroRandomSource.java lines 75-77
    return static_cast<float>(nextBits(24)) * FLOAT_UNIT;
}

double XoroshiroRandomSource::nextDouble() {
    // Reference: XoroshiroRandomSource.java lines 79-81
    return static_cast<double>(nextBits(53)) * DOUBLE_UNIT;
}

double XoroshiroRandomSource::nextGaussian() {
    // Reference: XoroshiroRandomSource.java lines 83-85
    return gaussianSource.nextGaussian();
}

void XoroshiroRandomSource::consumeCount(int32_t rounds) {
    // Reference: XoroshiroRandomSource.java lines 87-92
    // CRITICAL: This calls randomNumberGenerator.nextLong(), NOT nextInt()!
    for (int32_t i = 0; i < rounds; ++i) {
        generator.nextLong();
    }
}

uint64_t XoroshiroRandomSource::nextBits(int bits) {
    // Reference: XoroshiroRandomSource.java lines 94-96
    return generator.nextLong() >> (64 - bits);
}

// ============================================================================
// XoroshiroPositionalRandomFactory Implementation
// ============================================================================

XoroshiroPositionalRandomFactory::XoroshiroPositionalRandomFactory(uint64_t seedLo, uint64_t seedHi)
    : seedLo(seedLo), seedHi(seedHi) {
    // Reference: XoroshiroRandomSource.java lines 106-109
}

XoroshiroRandomSource XoroshiroPositionalRandomFactory::at(int32_t x, int32_t y, int32_t z) {
    // Reference: XoroshiroRandomSource.java lines 111-115
    int64_t positionalSeed = Mth::getSeed(x, y, z);
    int64_t randomSeed = positionalSeed ^ static_cast<int64_t>(seedLo);
    return XoroshiroRandomSource(static_cast<uint64_t>(randomSeed), seedHi);
}

XoroshiroRandomSource XoroshiroPositionalRandomFactory::fromHashOf(const std::string& name) {
    // Reference: XoroshiroRandomSource.java lines 117-120
    // CRITICAL: Uses MD5 hash, NOT simple string hash!
    Seed128bit hashSeed = RandomSupport::seedFromHashOf(name);
    Seed128bit xored = hashSeed.xor_(seedLo, seedHi);
    return XoroshiroRandomSource(xored.seedLo, xored.seedHi);
}

XoroshiroRandomSource XoroshiroPositionalRandomFactory::fromSeed(int64_t seed) {
    // Reference: XoroshiroRandomSource.java lines 122-124
    return XoroshiroRandomSource(
        static_cast<uint64_t>(seed) ^ seedLo,
        static_cast<uint64_t>(seed) ^ seedHi
    );
}

} // namespace minecraft
