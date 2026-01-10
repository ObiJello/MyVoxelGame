#include "world/ChunkPos.h"
#include "core/SectionPos.h"
#include "math/Mth.h"
#include <sstream>

namespace minecraft {
namespace world {

// Constructor from BlockPos (ChunkPos.java lines 45-48)
ChunkPos::ChunkPos(const core::BlockPos& pos)
    : m_x(core::SectionPos::blockToSectionCoord(pos.getX()))
    , m_z(core::SectionPos::blockToSectionCoord(pos.getZ())) {
}

// Static ZERO instance (ChunkPos.java line 251)
const ChunkPos& ChunkPos::ZERO() {
    static ChunkPos zero(0, 0);
    return zero;
}

// Block coordinate conversions (ChunkPos.java lines 120-162)

int32_t ChunkPos::getMinBlockX() const {
    return core::SectionPos::sectionToBlockCoord(m_x);
}

int32_t ChunkPos::getMinBlockZ() const {
    return core::SectionPos::sectionToBlockCoord(m_z);
}

int32_t ChunkPos::getMaxBlockX() const {
    return getBlockX(15);
}

int32_t ChunkPos::getMaxBlockZ() const {
    return getBlockZ(15);
}

int32_t ChunkPos::getBlockX(int32_t offset) const {
    return core::SectionPos::sectionToBlockCoord(m_x, offset);
}

int32_t ChunkPos::getBlockZ(int32_t offset) const {
    return core::SectionPos::sectionToBlockCoord(m_z, offset);
}

core::BlockPos ChunkPos::getBlockAt(int32_t x, int32_t y, int32_t z) const {
    return core::BlockPos(getBlockX(x), y, getBlockZ(z));
}

core::BlockPos ChunkPos::getWorldPosition() const {
    return core::BlockPos(getMinBlockX(), 0, getMinBlockZ());
}

// Packed long from BlockPos (ChunkPos.java lines 79-81)
int64_t ChunkPos::asLong(const core::BlockPos& pos) {
    return asLong(
        core::SectionPos::blockToSectionCoord(pos.getX()),  // Instance method
        core::SectionPos::blockToSectionCoord(pos.getZ())   // Instance method
    );
}

// Distance calculations (ChunkPos.java lines 180-186)

int32_t ChunkPos::getChessboardDistance(const ChunkPos& other) const {
    return getChessboardDistance(other.m_x, other.m_z);
}

int32_t ChunkPos::getChessboardDistance(int32_t x, int32_t z) const {
    return Mth::chessboardDistance(x, z, m_x, m_z);
}

// toString() is now inline in ChunkPos.h

} // namespace world
} // namespace minecraft
