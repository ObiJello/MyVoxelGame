#pragma once

#include "core/BlockPos.h"
#include <cstdint>
#include <string>

namespace minecraft {
namespace world {

/**
 * Chunk position (chunk coordinates, not block coordinates)
 * Reference: ChunkPos.java
 *
 * Chunk coordinates are block coordinates divided by 16
 */
class ChunkPos {
private:
    // Reference: ChunkPos.java lines 34-35
    int32_t m_x;
    int32_t m_z;

    // Hash constants (ChunkPos.java lines 36-38)
    static constexpr int32_t HASH_A = 1664525;
    static constexpr int32_t HASH_C = 1013904223;
    static constexpr int32_t HASH_Z_XOR = -559038737;

    // Packed long format constants (ChunkPos.java lines 28-29)
    static constexpr int64_t COORD_BITS = 32L;
    static constexpr int64_t COORD_MASK = 0xFFFFFFFFL;  // 4294967295L

public:
    // Invalid chunk position sentinel (used by ChunkTracker)
    static constexpr int64_t INVALID_CHUNK_POS = INT64_MAX;
    // Constructors (ChunkPos.java lines 40-53)
    ChunkPos(int32_t x, int32_t z) : m_x(x), m_z(z) {}

    ChunkPos(const core::BlockPos& pos);

    ChunkPos(int64_t packed)
        : m_x(static_cast<int32_t>(packed))
        , m_z(static_cast<int32_t>(packed >> 32)) {}

    // Default constructor
    ChunkPos() : m_x(0), m_z(0) {}

    // Static factory from packed long (convenience wrapper for constructor)
    static ChunkPos fromLong(int64_t packed) {
        return ChunkPos(packed);
    }

    // Getters
    int32_t x() const { return m_x; }
    int32_t z() const { return m_z; }

    // Validity check (not INVALID_CHUNK_POS)
    bool isValid() const {
        return toLong() != INVALID_CHUNK_POS;
    }

    // String representation
    std::string toString() const {
        return "[" + std::to_string(m_x) + ", " + std::to_string(m_z) + "]";
    }

    // Block coordinate conversions (ChunkPos.java lines 120-134)

    /**
     * Get minimum block X coordinate in this chunk
     * Reference: ChunkPos.java lines 120-122
     */
    int32_t getMinBlockX() const;

    /**
     * Get minimum block Z coordinate in this chunk
     * Reference: ChunkPos.java lines 124-126
     */
    int32_t getMinBlockZ() const;

    /**
     * Get maximum block X coordinate in this chunk
     * Reference: ChunkPos.java lines 128-130
     */
    int32_t getMaxBlockX() const;

    /**
     * Get maximum block Z coordinate in this chunk
     * Reference: ChunkPos.java lines 132-134
     */
    int32_t getMaxBlockZ() const;

    /**
     * Get block coordinate at offset within chunk
     * Reference: ChunkPos.java lines 156-162
     */
    int32_t getBlockX(int32_t offset) const;
    int32_t getBlockZ(int32_t offset) const;

    /**
     * Get middle block X coordinate in this chunk
     * Reference: ChunkPos.java lines 112-114
     */
    int32_t getMiddleBlockX() const {
        return (m_x << 4) + 8;
    }

    /**
     * Get middle block Z coordinate in this chunk
     * Reference: ChunkPos.java lines 116-118
     */
    int32_t getMiddleBlockZ() const {
        return (m_z << 4) + 8;
    }

    /**
     * Get middle block position at given Y
     * Reference: ChunkPos.java lines 164-166
     */
    core::BlockPos getMiddleBlockPosition(int32_t y) const {
        return core::BlockPos(getMiddleBlockX(), y, getMiddleBlockZ());
    }

    /**
     * Get block position at coordinates within chunk
     * Reference: ChunkPos.java lines 152-154
     */
    core::BlockPos getBlockAt(int32_t x, int32_t y, int32_t z) const;

    /**
     * Get world block position (min corner)
     * Reference: ChunkPos.java lines 176-178
     */
    core::BlockPos getWorldPosition() const;

    // Packed long representation (ChunkPos.java lines 71-89)

    /**
     * Pack chunk coordinates into a single int64_t
     * Reference: ChunkPos.java lines 71-77
     *
     * Format: low 32 bits = X, high 32 bits = Z
     */
    int64_t toLong() const {
        return asLong(m_x, m_z);
    }

    static int64_t asLong(int32_t x, int32_t z) {
        return (static_cast<int64_t>(x) & COORD_MASK) |
               ((static_cast<int64_t>(z) & COORD_MASK) << 32);
    }

    static int64_t asLong(const core::BlockPos& pos);

    /**
     * Extract X coordinate from packed long
     * Reference: ChunkPos.java lines 83-85
     */
    static int32_t getX(int64_t packed) {
        return static_cast<int32_t>(packed & COORD_MASK);
    }

    /**
     * Extract Z coordinate from packed long
     * Reference: ChunkPos.java lines 87-89
     */
    static int32_t getZ(int64_t packed) {
        return static_cast<int32_t>((packed >> 32) & COORD_MASK);
    }

    // Equality and hashing (ChunkPos.java lines 91-110)

    bool operator==(const ChunkPos& other) const {
        return m_x == other.m_x && m_z == other.m_z;
    }

    bool operator!=(const ChunkPos& other) const {
        return !(*this == other);
    }

    /**
     * Hash function for chunk position
     * Reference: ChunkPos.java lines 91-99
     */
    int32_t hashCode() const {
        return hash(m_x, m_z);
    }

    static int32_t hash(int32_t x, int32_t z) {
        int32_t xTransform = HASH_A * x + HASH_C;
        int32_t zTransform = HASH_A * (z ^ HASH_Z_XOR) + HASH_C;
        return xTransform ^ zTransform;
    }

    // Distance calculations (ChunkPos.java lines 180-200)

    /**
     * Chessboard distance (max of dx, dz)
     * Reference: ChunkPos.java lines 180-186
     */
    int32_t getChessboardDistance(const ChunkPos& other) const;
    int32_t getChessboardDistance(int32_t x, int32_t z) const;

    /**
     * Squared Euclidean distance
     * Reference: ChunkPos.java lines 188-200
     */
    int32_t distanceSquared(const ChunkPos& other) const {
        return distanceSquared(other.m_x, other.m_z);
    }

    int32_t distanceSquared(int64_t packed) const {
        return distanceSquared(getX(packed), getZ(packed));
    }

    int32_t distanceSquared(int32_t x, int32_t z) const {
        int32_t deltaX = x - m_x;
        int32_t deltaZ = z - m_z;
        return deltaX * deltaX + deltaZ * deltaZ;
    }

    // Static ZERO constant (ChunkPos.java line 27)
    static const ChunkPos& ZERO();

    // Region coordinates (ChunkPos.java lines 30-31, 136-150)
    static constexpr int32_t REGION_SIZE = 32;
    static constexpr int32_t REGION_MASK = 31;

    int32_t getRegionX() const {
        return m_x >> 5;
    }

    int32_t getRegionZ() const {
        return m_z >> 5;
    }

    int32_t getRegionLocalX() const {
        return m_x & REGION_MASK;
    }

    int32_t getRegionLocalZ() const {
        return m_z & REGION_MASK;
    }
};

} // namespace world
} // namespace minecraft

// For backwards compatibility with code using ::world::ChunkPos
namespace world {
    using ChunkPos = minecraft::world::ChunkPos;
}
