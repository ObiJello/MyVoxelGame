#pragma once

#include <cstdint>
#include <cstring>

namespace minecraft {
namespace util {

/**
 * SHA256 - SHA-256 hashing utility
 *
 * Implements SHA-256 hashing to match Google Guava's Hashing.sha256().hashLong(seed).asLong()
 * Reference: com.google.common.hash.Hashing
 *
 * This is used by BiomeManager.obfuscateSeed()
 */
class SHA256 {
public:
    /**
     * Hash a 64-bit long value using SHA-256 and return 64 bits
     * Matches Guava's: Hashing.sha256().hashLong(seed).asLong()
     *
     * @param value - Input value to hash
     * @return First 64 bits of SHA-256 hash as signed long
     */
    static int64_t hashLong(int64_t value);

private:
    /**
     * Compute SHA-256 hash of data
     * @param data - Input data
     * @param length - Length of data in bytes
     * @param hash - Output buffer (must be 32 bytes)
     */
    static void computeSHA256(const uint8_t* data, size_t length, uint8_t* hash);
};

} // namespace util
} // namespace minecraft
