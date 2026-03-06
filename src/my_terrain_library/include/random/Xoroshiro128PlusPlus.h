#ifndef XOROSHIRO128PLUSPLUS_H
#define XOROSHIRO128PLUSPLUS_H

#include <cstdio>
#include <cstdint>

#include <cstdint>

namespace minecraft {

/**
 * Xoroshiro128++ random number generator.
 * This is the core algorithm used by Minecraft for Overworld terrain generation.
 *
 * Reference: /minecraft/world/level/levelgen/Xoroshiro128PlusPlus.java
 */
class Xoroshiro128PlusPlus {
public:
    // Constants from RandomSupport.java
    static constexpr int64_t GOLDEN_RATIO_64 = -7046029254386353131LL;
    static constexpr int64_t SILVER_RATIO_64 = 7640891576956012809LL;

    /**
     * Constructor with 128-bit seed.
     * If both seeds are zero, uses default non-zero values to prevent degenerate state.
     */
    Xoroshiro128PlusPlus(uint64_t seedLo, uint64_t seedHi);

    /**
     * Generate next random 64-bit value.
     * This is the complete Xoroshiro128++ algorithm.
     */
    uint64_t nextLong();

    // Getters for testing/debugging
    uint64_t getSeedLo() const { return seedLo; }
    uint64_t getSeedHi() const { return seedHi; }


private:
    uint64_t seedLo;  // Lower 64 bits of 128-bit state
    uint64_t seedHi;  // Upper 64 bits of 128-bit state

    /**
     * Rotate left operation (Java's Long.rotateLeft).
     * C++ doesn't have this built-in, so we implement it.
     */
    static inline uint64_t rotl64(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
};

} // namespace minecraft

#endif // XOROSHIRO128PLUSPLUS_H
