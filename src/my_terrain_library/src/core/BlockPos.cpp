#include "core/BlockPos.h"
#include "math/Mth.h"
#include "core/SectionPos.h"

namespace minecraft {
namespace core {

// ===========================================================================
// BlockPos Implementation
// ===========================================================================

// Packed long format constants (BlockPos.java lines 37-44, 550-557)
// PACKED_HORIZONTAL_LENGTH = 1 + log2(smallestEncompassingPowerOfTwo(30000000))
// = 1 + log2(33554432) = 1 + 25 = 26
static constexpr int32_t PACKED_HORIZONTAL_LENGTH = 26;
static constexpr int32_t PACKED_Y_LENGTH = 64 - 2 * PACKED_HORIZONTAL_LENGTH;  // 12 bits
static constexpr int64_t PACKED_X_MASK = (1L << PACKED_HORIZONTAL_LENGTH) - 1L;
static constexpr int64_t PACKED_Y_MASK = (1L << PACKED_Y_LENGTH) - 1L;
static constexpr int64_t PACKED_Z_MASK = (1L << PACKED_HORIZONTAL_LENGTH) - 1L;
static constexpr int32_t Y_OFFSET = 0;
static constexpr int32_t Z_OFFSET = PACKED_Y_LENGTH;
static constexpr int32_t X_OFFSET = PACKED_Y_LENGTH + PACKED_HORIZONTAL_LENGTH;

// Static ZERO instance (BlockPos.java line 549)
const BlockPos& BlockPos::ZERO() {
    static BlockPos zero(0, 0, 0);
    return zero;
}

// Create BlockPos from double coordinates (BlockPos.java lines 79-81)
BlockPos BlockPos::containing(double x, double y, double z) {
    return BlockPos(Mth::floor(x), Mth::floor(y), Mth::floor(z));
}

// Packed long representation (BlockPos.java lines 99-105)
int64_t BlockPos::asLong(int32_t x, int32_t y, int32_t z) {
    int64_t packed = 0L;
    packed |= (static_cast<int64_t>(x) & PACKED_X_MASK) << X_OFFSET;
    packed |= (static_cast<int64_t>(y) & PACKED_Y_MASK) << Y_OFFSET;
    packed |= (static_cast<int64_t>(z) & PACKED_Z_MASK) << Z_OFFSET;
    return packed;
}

int64_t BlockPos::asLong() const {
    return asLong(m_x, m_y, m_z);
}

// Extract coordinates from packed long (BlockPos.java lines 63-73)
int32_t BlockPos::getPackedX(int64_t packed) {
    // Sign-extend the value
    return static_cast<int32_t>(packed << (64 - X_OFFSET - PACKED_HORIZONTAL_LENGTH) >> (64 - PACKED_HORIZONTAL_LENGTH));
}

int32_t BlockPos::getPackedY(int64_t packed) {
    // Sign-extend the value
    return static_cast<int32_t>(packed << (64 - PACKED_Y_LENGTH) >> (64 - PACKED_Y_LENGTH));
}

int32_t BlockPos::getPackedZ(int64_t packed) {
    // Sign-extend the value
    return static_cast<int32_t>(packed << (64 - Z_OFFSET - PACKED_HORIZONTAL_LENGTH) >> (64 - PACKED_HORIZONTAL_LENGTH));
}

// Create BlockPos from packed long (BlockPos.java lines 75-77)
BlockPos BlockPos::of(int64_t packed) {
    return BlockPos(getPackedX(packed), getPackedY(packed), getPackedZ(packed));
}

// Create mutable copy (BlockPos.java lines 231-233)
BlockPos::MutableBlockPos BlockPos::mutableCopy() const {
    return MutableBlockPos(m_x, m_y, m_z);
}

// ===========================================================================
// MutableBlockPos Implementation
// ===========================================================================

// Constructor from doubles (BlockPos.java lines 569-571)
BlockPos::MutableBlockPos::MutableBlockPos(double x, double y, double z)
    : BlockPos(Mth::floor(x), Mth::floor(y), Mth::floor(z)) {
}

// Set from doubles (BlockPos.java lines 600-602)
BlockPos::MutableBlockPos& BlockPos::MutableBlockPos::set(double x, double y, double z) {
    return set(Mth::floor(x), Mth::floor(y), Mth::floor(z));
}

} // namespace core
} // namespace minecraft
