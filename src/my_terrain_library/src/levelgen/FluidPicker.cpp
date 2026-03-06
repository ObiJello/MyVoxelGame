#include "levelgen/FluidPicker.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/Blocks.h"

namespace minecraft {
namespace levelgen {

using Blocks = minecraft::world::level::block::Blocks;

// FluidStatus::at implementation
BlockState* FluidStatus::at(int32_t blockY) const {
    // Reference: Aquifer.java lines 461-463
    // If block Y is below the fluid level, return the fluid type
    // Otherwise return AIR (NOT nullptr!)
    // Java: return blockY < this.fluidLevel ? this.fluidType : Blocks.AIR.defaultBlockState();
    return (blockY < fluidLevel) ? fluidType : minecraft::world::level::block::Blocks::AIR->defaultBlockState();
}

// SeaLevelFluidPicker implementation
SeaLevelFluidPicker::SeaLevelFluidPicker(int32_t seaLevel, BlockState* waterBlock)
    : m_seaLevel(seaLevel)
    , m_waterBlock(waterBlock)
{
}

FluidStatus SeaLevelFluidPicker::computeFluid(int32_t blockX, int32_t blockY, int32_t blockZ) {
    // Simple implementation: always return water at sea level
    return FluidStatus{m_seaLevel, m_waterBlock};
}

// OverworldFluidPicker implementation
OverworldFluidPicker::OverworldFluidPicker(
    int32_t seaLevel,
    int32_t lavaLevel,
    BlockState* waterBlock,
    BlockState* lavaBlock
)
    : m_seaLevel(seaLevel)
    , m_lavaLevel(lavaLevel)
    , m_waterBlock(waterBlock)
    , m_lavaBlock(lavaBlock)
{
}

FluidStatus OverworldFluidPicker::computeFluid(int32_t blockX, int32_t blockY, int32_t blockZ) {
    // Java: y < Math.min(-54, seaLevel) ? lavaStatus : seaStatus
    // where lavaStatus = {fluidLevel: -54, fluidType: LAVA}
    //   and seaStatus = {fluidLevel: seaLevel, fluidType: WATER}
    // Note: m_lavaLevel is expected to be Math.min(-54, seaLevel), which is -54 for overworld
    if (blockY < m_lavaLevel) {
        return FluidStatus{m_lavaLevel, m_lavaBlock};  // FIXED: use m_lavaLevel not m_seaLevel
    } else {
        return FluidStatus{m_seaLevel, m_waterBlock};
    }
}

} // namespace levelgen
} // namespace minecraft
