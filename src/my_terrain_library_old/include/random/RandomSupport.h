#ifndef RANDOMSUPPORT_H
#define RANDOMSUPPORT_H

#include <cstdint>
#include <string>

namespace minecraft {

/**
 * 128-bit seed structure.
 * Reference: RandomSupport.java lines 44-56
 */
struct Seed128bit {
    uint64_t seedLo;
    uint64_t seedHi;

    /**
     * XOR this seed with another 128-bit value.
     */
    Seed128bit xor_(uint64_t lo, uint64_t hi) const {
        return {seedLo ^ lo, seedHi ^ hi};
    }

    /**
     * XOR this seed with another Seed128bit.
     */
    Seed128bit xor_(const Seed128bit& other) const {
        return xor_(other.seedLo, other.seedHi);
    }

    /**
     * Apply Stafford13 mixing to both parts of the seed.
     */
    Seed128bit mixed() const;
};

/**
 * Random support functions for seed manipulation.
 * Reference: /minecraft/world/level/levelgen/RandomSupport.java
 */
class RandomSupport {
public:
    // Constants from RandomSupport.java
    static constexpr int64_t GOLDEN_RATIO_64 = -7046029254386353131LL;
    static constexpr int64_t SILVER_RATIO_64 = 7640891576956012809LL;

    /**
     * Stafford13 mixing function for improving seed distribution.
     * Reference: RandomSupport.java lines 17-21
     *
     * This is a high-quality mixing function that ensures even poorly
     * distributed input seeds produce well-distributed random numbers.
     */
    static uint64_t mixStafford13(uint64_t z);

    /**
     * Upgrade a 64-bit seed to 128-bit without mixing.
     * Reference: RandomSupport.java lines 23-27
     */
    static Seed128bit upgradeSeedTo128bitUnmixed(int64_t seed);

    /**
     * Upgrade a 64-bit seed to 128-bit with mixing.
     * Reference: RandomSupport.java lines 29-31
     *
     * This is the primary method used when creating XoroshiroRandomSource
     * from a single 64-bit seed.
     */
    static Seed128bit upgradeSeedTo128bit(int64_t seed);

    /**
     * Create a 128-bit seed from a string using MD5 hash.
     * Reference: RandomSupport.java lines 33-38
     *
     * CRITICAL: This uses MD5 hashing, NOT Java's String.hashCode()!
     * This is how Minecraft generates seeds for noise octaves ("octave_0", etc.)
     */
    static Seed128bit seedFromHashOf(const std::string& input);

    /**
     * Compute Java's String.hashCode() for a given string.
     *
     * Java's algorithm: hash = s[0]*31^(n-1) + s[1]*31^(n-2) + ... + s[n-1]
     *
     * This is CRITICAL for matching Minecraft's noise seeding when using
     * noise name hashes (e.g., "spaghetti_roughness".hashCode()).
     *
     * Reference: java.lang.String.hashCode()
     */
    static int32_t javaStringHashCode(const std::string& str);

    /**
     * Generate a unique seed for random initialization.
     * Uses current time and a counter to ensure uniqueness.
     * Reference: WorldgenRandom.java lines 24-26
     */
    static Seed128bit generateUniqueSeed();

private:
    /**
     * Convert 8 bytes to a 64-bit long in big-endian order.
     * Matches Java's Longs.fromBytes() from Guava library.
     */
    static uint64_t bytesToLong(const uint8_t* bytes);
};

} // namespace minecraft

#endif // RANDOMSUPPORT_H
