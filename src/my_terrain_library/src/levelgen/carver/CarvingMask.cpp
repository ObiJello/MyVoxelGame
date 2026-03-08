#include "levelgen/carver/CarvingMask.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace minecraft {
namespace levelgen {
namespace carver {

CarvingMask::CarvingMask(int32_t height, int32_t minY)
    : m_minY(minY)
    , m_additionalMask(nullptr)
{
    // BitSet size is 256 * height (16x16 horizontal, height vertical)
    // We need (256 * height + 63) / 64 longs to store this many bits
    int32_t totalBits = 256 * height;
    int32_t numLongs = (totalBits + 63) / 64;
    m_mask.resize(numLongs, 0);
}

CarvingMask::CarvingMask(const std::vector<int64_t>& array, int32_t minY)
    : m_minY(minY)
    , m_additionalMask(nullptr)
{
    m_mask.reserve(array.size());
    for (int64_t val : array) {
        m_mask.push_back(static_cast<uint64_t>(val));
    }
}

void CarvingMask::setAdditionalMask(Mask additionalMask) {
    m_additionalMask = additionalMask;
}

void CarvingMask::set(int32_t x, int32_t y, int32_t z) {
    int32_t index = getIndex(x, y, z);
    int32_t longIndex = index >> 6;  // index / 64
    int32_t bitIndex = index & 63;   // index % 64

    if (longIndex >= 0 && longIndex < static_cast<int32_t>(m_mask.size())) {
        m_mask[longIndex] |= (1ULL << bitIndex);
    }
}

bool CarvingMask::get(int32_t x, int32_t y, int32_t z) const {
    // Check additional mask first
    if (m_additionalMask && m_additionalMask(x, y, z)) {
        return true;
    }

    int32_t index = getIndex(x, y, z);
    int32_t longIndex = index >> 6;  // index / 64
    int32_t bitIndex = index & 63;   // index % 64

    if (longIndex >= 0 && longIndex < static_cast<int32_t>(m_mask.size())) {
        return (m_mask[longIndex] & (1ULL << bitIndex)) != 0;
    }
    return false;
}

void CarvingMask::forEachCarvedPosition(const minecraft::world::ChunkPos& pos,
                                         std::function<void(const core::BlockPos&)> callback) const {
    // Iterate through all set bits
    for (size_t longIndex = 0; longIndex < m_mask.size(); ++longIndex) {
        uint64_t bits = m_mask[longIndex];
        while (bits != 0) {
            // Find lowest set bit
#ifdef _MSC_VER
            unsigned long bitIndex;
            _BitScanForward64(&bitIndex, bits);
#else
            int bitIndex = __builtin_ctzll(bits);
#endif
            int32_t index = static_cast<int32_t>(longIndex * 64 + bitIndex);

            // Decode index back to x, z, y
            int32_t x = index & 15;
            int32_t z = (index >> 4) & 15;
            int32_t y = (index >> 8) + m_minY;

            callback(pos.getBlockAt(x, y, z));

            // Clear this bit and continue
            bits &= (bits - 1);
        }
    }
}

std::vector<int64_t> CarvingMask::toArray() const {
    std::vector<int64_t> result;
    result.reserve(m_mask.size());
    for (uint64_t val : m_mask) {
        result.push_back(static_cast<int64_t>(val));
    }
    return result;
}

} // namespace carver
} // namespace levelgen
} // namespace minecraft
