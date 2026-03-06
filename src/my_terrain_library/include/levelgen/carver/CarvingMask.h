#pragma once

#include "core/BlockPos.h"
#include "world/ChunkPos.h"
#include <cstdint>
#include <vector>
#include <functional>

// Reference: net/minecraft/world/level/chunk/CarvingMask.java

namespace minecraft {
namespace levelgen {
namespace carver {

/**
 * Mask - Functional interface for additional mask testing
 * Reference: CarvingMask.java line 52-54
 */
using Mask = std::function<bool(int32_t x, int32_t y, int32_t z)>;

/**
 * CarvingMask - Tracks which blocks have been carved in a chunk
 * Uses a BitSet representation with efficient indexing
 * Reference: CarvingMask.java
 */
class CarvingMask {
private:
    int32_t m_minY;
    std::vector<uint64_t> m_mask;  // BitSet equivalent
    Mask m_additionalMask;

    /**
     * Calculate index into bitset for given position
     * Reference: CarvingMask.java line 27-29
     * Layout: x in bits 0-3, z in bits 4-7, y-minY in bits 8+
     */
    int32_t getIndex(int32_t x, int32_t y, int32_t z) const {
        return (x & 15) | ((z & 15) << 4) | ((y - m_minY) << 8);
    }

public:
    /**
     * Create a carving mask for the given height range
     * Reference: CarvingMask.java lines 13-16
     * @param height The vertical height of the chunk
     * @param minY The minimum Y coordinate
     */
    CarvingMask(int32_t height, int32_t minY);

    /**
     * Create a carving mask from serialized data
     * Reference: CarvingMask.java lines 22-25
     * @param array Long array from serialization
     * @param minY The minimum Y coordinate
     */
    CarvingMask(const std::vector<int64_t>& array, int32_t minY);

    /**
     * Set an additional mask function to be tested
     * Reference: CarvingMask.java lines 18-20
     */
    void setAdditionalMask(Mask additionalMask);

    /**
     * Mark a position as carved
     * Reference: CarvingMask.java lines 31-33
     */
    void set(int32_t x, int32_t y, int32_t z);

    /**
     * Check if a position has been carved
     * Reference: CarvingMask.java lines 35-37
     * @return true if carved (via mask or additionalMask)
     */
    bool get(int32_t x, int32_t y, int32_t z) const;

    /**
     * Get all carved positions in the chunk
     * Reference: CarvingMask.java lines 39-45
     * @param pos The chunk position
     * @param callback Function called for each carved position
     */
    void forEachCarvedPosition(const minecraft::world::ChunkPos& pos,
                                std::function<void(const core::BlockPos&)> callback) const;

    /**
     * Serialize to long array
     * Reference: CarvingMask.java lines 47-49
     */
    std::vector<int64_t> toArray() const;

    /**
     * Get minimum Y coordinate
     */
    int32_t getMinY() const { return m_minY; }
};

} // namespace carver
} // namespace levelgen
} // namespace minecraft
