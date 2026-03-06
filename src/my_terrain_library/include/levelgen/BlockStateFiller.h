#pragma once

#include "levelgen/DensityFunction.h"
#include "world/level/block/state/BlockState.h"

namespace minecraft {
namespace levelgen {

using BlockState = minecraft::world::level::block::state::BlockState;

/**
 * BlockStateFiller - Interface for block decision rules
 * Reference: DensityFunctions.java line 1340 (BlockStateFiller interface)
 *
 * This interface is used to determine what block type should be placed
 * at a given position based on the density function context.
 *
 * Implementations include:
 * - Aquifer: Determines water/lava/air placement
 * - OreVeinifier: Adds ore veins
 * - MaterialRuleList: Chains multiple rules together
 */
class BlockStateFiller {
public:
    virtual ~BlockStateFiller() = default;

    /**
     * Calculate the block type to place at the given context position.
     *
     * @param context The density function context containing position and density values
     * @return The block state to place, or nullptr if no block should be placed
     *         (nullptr typically means use the default block, e.g., stone)
     */
    virtual BlockState* calculate(
        const density::DensityFunction::FunctionContext& context
    ) const = 0;
};

} // namespace levelgen
} // namespace minecraft
