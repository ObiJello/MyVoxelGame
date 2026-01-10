#include "random/RandomSupport.h"
#include <openssl/md5.h>
#include <cstring>
#include <chrono>
#include <atomic>

namespace minecraft {

// Static counter for unique seed generation
static std::atomic<int64_t> g_seedCounter{0};

uint64_t RandomSupport::mixStafford13(uint64_t z) {
    // Stafford13 mixing function from RandomSupport.java lines 17-21
    // These specific constants provide excellent avalanche properties

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;  // -4658895280553007687LL
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;  // -7723592293110705685LL
    return z ^ (z >> 31);
}

Seed128bit Seed128bit::mixed() const {
    return {
        RandomSupport::mixStafford13(seedLo),
        RandomSupport::mixStafford13(seedHi)
    };
}

Seed128bit RandomSupport::upgradeSeedTo128bitUnmixed(int64_t seed) {
    // Reference: RandomSupport.java lines 23-27
    uint64_t lo = static_cast<uint64_t>(seed) ^ static_cast<uint64_t>(SILVER_RATIO_64);
    uint64_t hi = lo + static_cast<uint64_t>(GOLDEN_RATIO_64);
    return {lo, hi};
}

Seed128bit RandomSupport::upgradeSeedTo128bit(int64_t seed) {
    // Reference: RandomSupport.java lines 29-31
    return upgradeSeedTo128bitUnmixed(seed).mixed();
}

uint64_t RandomSupport::bytesToLong(const uint8_t* bytes) {
    // Convert 8 bytes to 64-bit long in big-endian order
    // This matches Java's Longs.fromBytes(b0, b1, b2, b3, b4, b5, b6, b7)
    // Formula: (b0 << 56) | (b1 << 48) | (b2 << 40) | ... | b7

    return (static_cast<uint64_t>(bytes[0]) << 56) |
           (static_cast<uint64_t>(bytes[1]) << 48) |
           (static_cast<uint64_t>(bytes[2]) << 40) |
           (static_cast<uint64_t>(bytes[3]) << 32) |
           (static_cast<uint64_t>(bytes[4]) << 24) |
           (static_cast<uint64_t>(bytes[5]) << 16) |
           (static_cast<uint64_t>(bytes[6]) << 8) |
           (static_cast<uint64_t>(bytes[7]));
}

Seed128bit RandomSupport::seedFromHashOf(const std::string& input) {
    // Reference: RandomSupport.java lines 33-38
    // Uses MD5 hash to convert string to 128-bit seed

    // Compute MD5 hash
    unsigned char hash[MD5_DIGEST_LENGTH];  // 16 bytes
    MD5(reinterpret_cast<const unsigned char*>(input.c_str()),
        input.length(),
        hash);

    // Extract two 64-bit values in big-endian order
    uint64_t lo = bytesToLong(hash);      // First 8 bytes
    uint64_t hi = bytesToLong(hash + 8);  // Last 8 bytes

    return {lo, hi};
}

int32_t RandomSupport::javaStringHashCode(const std::string& str) {
    // Java's String.hashCode() algorithm
    // Reference: java.lang.String.hashCode()
    //
    // public int hashCode() {
    //     int h = 0;
    //     for (int i = 0; i < value.length; i++) {
    //         h = 31 * h + value[i];
    //     }
    //     return h;
    // }
    //
    // This uses int32_t arithmetic with wraparound (same as Java int)

    int32_t hash = 0;
    for (char c : str) {
        hash = 31 * hash + static_cast<int32_t>(c);
    }
    return hash;
}

Seed128bit RandomSupport::generateUniqueSeed() {
    // Reference: WorldgenRandom.java uses System.nanoTime() ^ seedUniquifier
    // We use high_resolution_clock and an atomic counter for thread safety

    auto now = std::chrono::high_resolution_clock::now();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    int64_t counter = g_seedCounter.fetch_add(1, std::memory_order_relaxed);
    int64_t seed = nanos ^ (counter * GOLDEN_RATIO_64);

    return upgradeSeedTo128bit(seed);
}

} // namespace minecraft
