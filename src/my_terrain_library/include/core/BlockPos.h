#pragma once

#include "core/Vec3i.h"
#include <cstdint>
#include <cstdlib>

namespace minecraft {
namespace core {

/**
 * Immutable block position in world coordinates
 * Reference: BlockPos.java
 */
class BlockPos : public Vec3i {
public:
    // Constructors (BlockPos.java lines 47-53)
    BlockPos(int32_t x, int32_t y, int32_t z) : Vec3i(x, y, z) {}
    BlockPos(const Vec3i& vec) : Vec3i(vec.getX(), vec.getY(), vec.getZ()) {}
    BlockPos() : Vec3i(0, 0, 0) {}

    // Static factory methods

    /**
     * Create BlockPos from double coordinates (floor)
     * Reference: BlockPos.java lines 79-81
     */
    static BlockPos containing(double x, double y, double z);

    /**
     * Offset by vector
     * Reference: BlockPos.java lines 111-113, 123-129
     */
    BlockPos offset(int32_t x, int32_t y, int32_t z) const {
        if (x == 0 && y == 0 && z == 0) {
            return *this;
        }
        return BlockPos(m_x + x, m_y + y, m_z + z);
    }

    BlockPos offset(const Vec3i& vec) const {
        return offset(vec.getX(), vec.getY(), vec.getZ());
    }

    BlockPos subtract(const Vec3i& vec) const {
        return offset(-vec.getX(), -vec.getY(), -vec.getZ());
    }

    /**
     * Common directional offsets
     * Reference: BlockPos.java lines 139-185
     */
    BlockPos above() const { return offset(0, 1, 0); }
    BlockPos above(int32_t steps) const { return offset(0, steps, 0); }
    BlockPos below() const { return offset(0, -1, 0); }
    BlockPos below(int32_t steps) const { return offset(0, -steps, 0); }
    BlockPos north() const { return offset(0, 0, -1); }
    BlockPos north(int32_t steps) const { return offset(0, 0, -steps); }
    BlockPos south() const { return offset(0, 0, 1); }
    BlockPos south(int32_t steps) const { return offset(0, 0, steps); }
    BlockPos west() const { return offset(-1, 0, 0); }
    BlockPos west(int32_t steps) const { return offset(-steps, 0, 0); }
    BlockPos east() const { return offset(1, 0, 0); }
    BlockPos east(int32_t steps) const { return offset(steps, 0, 0); }

    /**
     * Get position relative to this in given direction
     * Reference: BlockPos.java relative(Direction)
     */
    template<typename DirectionType>
    BlockPos relative(DirectionType dir, int32_t steps = 1) const {
        // Use ADL to find getStepX/Y/Z
        return offset(getStepX(dir) * steps, getStepY(dir) * steps, getStepZ(dir) * steps);
    }

    /**
     * Create new BlockPos with same X/Z but different Y
     * Reference: BlockPos.java lines 223-225
     */
    BlockPos atY(int32_t y) const {
        return BlockPos(m_x, y, m_z);
    }

    /**
     * Return immutable copy (this is already immutable)
     * Reference: BlockPos.java lines 227-229
     */
    BlockPos immutable() const {
        return *this;
    }

    /**
     * Calculate Manhattan distance to another position
     * Reference: BlockPos.java distManhattan()
     */
    int32_t distManhattan(const Vec3i& other) const {
        return std::abs(m_x - other.getX()) +
               std::abs(m_y - other.getY()) +
               std::abs(m_z - other.getZ());
    }

    /**
     * Create mutable copy
     * Reference: BlockPos.java lines 231-233
     */
    class MutableBlockPos;
    MutableBlockPos mutableCopy() const;

    // Static ZERO constant (BlockPos.java line 36)
    static const BlockPos& ZERO();

    /**
     * Packed long representation
     * Reference: BlockPos.java lines 95-105
     *
     * Format: 26 bits X, 12 bits Y, 26 bits Z
     */
    int64_t asLong() const;
    static int64_t asLong(int32_t x, int32_t y, int32_t z);
    static BlockPos of(int64_t packed);
    static int32_t getPackedX(int64_t packed);
    static int32_t getPackedY(int64_t packed);
    static int32_t getPackedZ(int64_t packed);
};

/**
 * Mutable block position for performance-critical code
 * Reference: BlockPos.java lines 560-674
 */
class BlockPos::MutableBlockPos : public BlockPos {
public:
    // Constructors (BlockPos.java lines 561-571)
    MutableBlockPos() : BlockPos(0, 0, 0) {}
    MutableBlockPos(int32_t x, int32_t y, int32_t z) : BlockPos(x, y, z) {}
    MutableBlockPos(double x, double y, double z);

    /**
     * Set position (returns self for chaining)
     * Reference: BlockPos.java lines 593-622
     */
    MutableBlockPos& set(int32_t x, int32_t y, int32_t z) {
        setX(x);
        setY(y);
        setZ(z);
        return *this;
    }

    MutableBlockPos& set(double x, double y, double z);

    MutableBlockPos& set(const Vec3i& vec) {
        return set(vec.getX(), vec.getY(), vec.getZ());
    }

    MutableBlockPos& set(int64_t packed) {
        return set(BlockPos::getPackedX(packed), BlockPos::getPackedY(packed), BlockPos::getPackedZ(packed));
    }

    /**
     * Set with offset from another position
     * Reference: BlockPos.java lines 616-622
     */
    MutableBlockPos& setWithOffset(const Vec3i& pos, int32_t x, int32_t y, int32_t z) {
        return set(pos.getX() + x, pos.getY() + y, pos.getZ() + z);
    }

    /**
     * Move by offset (modifies in place)
     * Reference: BlockPos.java lines 636-638
     */
    MutableBlockPos& move(int32_t x, int32_t y, int32_t z) {
        return set(m_x + x, m_y + y, m_z + z);
    }

    MutableBlockPos& move(const Vec3i& offset) {
        return move(offset.getX(), offset.getY(), offset.getZ());
    }

    /**
     * Move by direction
     * Reference: BlockPos.java move(Direction)
     */
    template<typename DirectionType>
    MutableBlockPos& move(DirectionType dir) {
        // Use ADL to find getStepX/Y/Z
        return move(getStepX(dir), getStepY(dir), getStepZ(dir));
    }

    /**
     * Move by direction with multiple steps
     * Reference: BlockPos.java move(Direction, int)
     */
    template<typename DirectionType>
    MutableBlockPos& move(DirectionType dir, int32_t steps) {
        return move(getStepX(dir) * steps, getStepY(dir) * steps, getStepZ(dir) * steps);
    }

    /**
     * Return immutable copy
     * Reference: BlockPos.java lines 671-673
     */
    BlockPos immutable() const {
        return BlockPos(m_x, m_y, m_z);
    }
};

} // namespace core
} // namespace minecraft
