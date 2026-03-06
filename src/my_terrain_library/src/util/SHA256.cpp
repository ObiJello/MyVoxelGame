#include "util/SHA256.h"
#include <cstring>

namespace minecraft {
namespace util {

// SHA-256 constants (first 32 bits of fractional parts of cube roots of first 64 primes)
// Reference: FIPS 180-4
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// Initial hash values (first 32 bits of fractional parts of square roots of first 8 primes)
// Reference: FIPS 180-4
static const uint32_t H0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

// Right rotate
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

// SHA-256 functions
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIGMA0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SIGMA1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define sigma0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define sigma1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

void SHA256::computeSHA256(const uint8_t* data, size_t length, uint8_t* hash) {
    // Initialize hash values
    uint32_t H[8];
    memcpy(H, H0, sizeof(H0));

    // Prepare message with padding
    // Message length in bits
    uint64_t bitLength = length * 8;

    // Calculate padded length (must be multiple of 512 bits = 64 bytes)
    size_t paddedLength = length + 1; // +1 for the '1' bit
    while ((paddedLength % 64) != 56) { // Need 56 bytes data + 8 bytes length = 64
        paddedLength++;
    }
    paddedLength += 8; // For the 64-bit length field

    // Allocate padded message
    uint8_t* paddedMessage = new uint8_t[paddedLength];
    memset(paddedMessage, 0, paddedLength);

    // Copy original data
    memcpy(paddedMessage, data, length);

    // Add padding bit (0x80 = 10000000 in binary)
    paddedMessage[length] = 0x80;

    // Add length in bits as big-endian 64-bit integer at the end
    for (int i = 0; i < 8; i++) {
        paddedMessage[paddedLength - 1 - i] = (bitLength >> (i * 8)) & 0xFF;
    }

    // Process message in 512-bit (64-byte) chunks
    for (size_t chunkStart = 0; chunkStart < paddedLength; chunkStart += 64) {
        uint32_t W[64];

        // Prepare message schedule
        // First 16 words are directly from the chunk (big-endian)
        for (int t = 0; t < 16; t++) {
            W[t] = ((uint32_t)paddedMessage[chunkStart + t * 4] << 24) |
                   ((uint32_t)paddedMessage[chunkStart + t * 4 + 1] << 16) |
                   ((uint32_t)paddedMessage[chunkStart + t * 4 + 2] << 8) |
                   ((uint32_t)paddedMessage[chunkStart + t * 4 + 3]);
        }

        // Extend the first 16 words into the remaining 48 words
        for (int t = 16; t < 64; t++) {
            W[t] = sigma1(W[t - 2]) + W[t - 7] + sigma0(W[t - 15]) + W[t - 16];
        }

        // Initialize working variables
        uint32_t a = H[0];
        uint32_t b = H[1];
        uint32_t c = H[2];
        uint32_t d = H[3];
        uint32_t e = H[4];
        uint32_t f = H[5];
        uint32_t g = H[6];
        uint32_t h = H[7];

        // Main loop
        for (int t = 0; t < 64; t++) {
            uint32_t T1 = h + SIGMA1(e) + CH(e, f, g) + K[t] + W[t];
            uint32_t T2 = SIGMA0(a) + MAJ(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + T1;
            d = c;
            c = b;
            b = a;
            a = T1 + T2;
        }

        // Compute intermediate hash value
        H[0] += a;
        H[1] += b;
        H[2] += c;
        H[3] += d;
        H[4] += e;
        H[5] += f;
        H[6] += g;
        H[7] += h;
    }

    // Produce final hash value (big-endian)
    for (int i = 0; i < 8; i++) {
        hash[i * 4] = (H[i] >> 24) & 0xFF;
        hash[i * 4 + 1] = (H[i] >> 16) & 0xFF;
        hash[i * 4 + 2] = (H[i] >> 8) & 0xFF;
        hash[i * 4 + 3] = H[i] & 0xFF;
    }

    delete[] paddedMessage;
}

int64_t SHA256::hashLong(int64_t value) {
    // Convert long to bytes (little-endian like Java does)
    // Reference: Guava's Hasher.putLong() uses little-endian byte order
    uint8_t data[8];
    for (int i = 0; i < 8; i++) {
        data[i] = (value >> (i * 8)) & 0xFF;
    }

    // Compute SHA-256 hash
    uint8_t hash[32];
    computeSHA256(data, 8, hash);

    // Return first 8 bytes as long (little-endian, matching Guava's asLong())
    // Reference: HashCode.asLong() in Guava
    int64_t result = 0;
    for (int i = 0; i < 8; i++) {
        result |= ((int64_t)hash[i] & 0xFF) << (i * 8);
    }

    return result;
}

#undef ROTR
#undef CH
#undef MAJ
#undef SIGMA0
#undef SIGMA1
#undef sigma0
#undef sigma1

} // namespace util
} // namespace minecraft
