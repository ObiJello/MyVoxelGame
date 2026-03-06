#pragma once

#include "world/level/block/state/BlockState.h"
#include <cstdint>

namespace minecraft {

namespace levelgen {

using BlockState = ::minecraft::world::level::block::state::BlockState;

/**
 * FluidStatus - Represents the fluid state at a position
 * Reference: Aquifer.java lines 460-464
 *
 * Contains the fluid type and the Y-level where the fluid surface is.
 */
struct FluidStatus {
    int32_t fluidLevel;        // Y-coordinate of fluid surface
    BlockState* fluidType;     // Type of fluid (water, lava, or air)

    /**
     * Get the block type at a given Y position.
     * If Y is below the fluid level, returns the fluid type.
     * Otherwise returns nullptr (air).
     *
     * Reference: Aquifer.java lines 461-463
     */
    BlockState* at(int32_t blockY) const;

    /**
     * Equality comparison for FluidStatus.
     * Two FluidStatus are equal if they have the same fluid level and type.
     */
    bool operator==(const FluidStatus& other) const {
        return fluidLevel == other.fluidLevel && fluidType == other.fluidType;
    }

    bool operator!=(const FluidStatus& other) const {
        return !(*this == other);
    }
};

/**
 * FluidPicker - Interface for determining fluid at a position
 * Reference: Aquifer.java lines 466-468
 *
 * This interface is used to compute the global fluid state at any position.
 * Typically returns water at sea level, but can be customized for different dimensions.
 */
class FluidPicker {
public:
    virtual ~FluidPicker() = default;

    /**
     * Compute the fluid status at the given block coordinates.
     *
     * @param blockX World X coordinate
     * @param blockY World Y coordinate
     * @param blockZ World Z coordinate
     * @return FluidStatus containing fluid type and surface level
     */
    virtual FluidStatus computeFluid(int32_t blockX, int32_t blockY, int32_t blockZ) = 0;
};

/**
 * SeaLevelFluidPicker - Simple implementation for overworld-style sea level
 *
 * Returns water at a fixed sea level (typically Y=63).
 */
class SeaLevelFluidPicker : public FluidPicker {
private:
    int32_t m_seaLevel;
    BlockState* m_waterBlock;

public:
    SeaLevelFluidPicker(int32_t seaLevel, BlockState* waterBlock);

    FluidStatus computeFluid(int32_t blockX, int32_t blockY, int32_t blockZ) override;
};

/**
 * OverworldFluidPicker - Overworld fluid picker with lava at low depths
 *
 * Returns water at sea level, but returns lava below a certain Y level.
 * This is used for the overworld dimension.
 */
class OverworldFluidPicker : public FluidPicker {
private:
    int32_t m_seaLevel;
    int32_t m_lavaLevel;  // Below this Y, return lava
    BlockState* m_waterBlock;
    BlockState* m_lavaBlock;

public:
    OverworldFluidPicker(
        int32_t seaLevel,
        int32_t lavaLevel,
        BlockState* waterBlock,
        BlockState* lavaBlock
    );

    FluidStatus computeFluid(int32_t blockX, int32_t blockY, int32_t blockZ) override;
};

} // namespace levelgen
} // namespace minecraft
