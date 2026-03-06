#include "random/Xoroshiro128PlusPlus.h"

namespace minecraft {

Xoroshiro128PlusPlus::Xoroshiro128PlusPlus(uint64_t seedLo, uint64_t seedHi)
    : seedLo(seedLo), seedHi(seedHi) {

    // If both seeds are zero, use default non-zero values
    // This prevents the degenerate all-zero state where the algorithm fails
    // Reference: Xoroshiro128PlusPlus.java lines 19-22
    if ((this->seedLo | this->seedHi) == 0) {
        this->seedLo = static_cast<uint64_t>(GOLDEN_RATIO_64);
        this->seedHi = static_cast<uint64_t>(SILVER_RATIO_64);
    }
}

uint64_t Xoroshiro128PlusPlus::nextLong() {
    uint64_t s0 = seedLo;
    uint64_t s1 = seedHi;
    uint64_t result = rotl64(s0 + s1, 17) + s0;

    s1 ^= s0;
    seedLo = rotl64(s0, 49) ^ s1 ^ (s1 << 21);
    seedHi = rotl64(s1, 28);

    return result;
}

} // namespace minecraft
