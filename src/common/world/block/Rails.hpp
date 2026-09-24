// File: src/common/world/block/Rails.hpp
//
// Port of MC BaseRailBlock, RailBlock, PoweredRailBlock, DetectorRailBlock
// and RailState — the shape logic that joins tracks into curves and slopes,
// and the power logic that lets a powered rail carry a signal up to eight
// rails from its source. Minecarts do not exist in this engine yet, so a
// detector rail never presses; its shape and removal rules are still needed
// for the tracks around it to form correctly.
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    // MC RailShape, in property-value order.
    enum class RailShape : uint8_t {
        NorthSouth = 0, EastWest, AscendingEast, AscendingWest, AscendingNorth,
        AscendingSouth, SouthEast, SouthWest, NorthWest, NorthEast,
    };
    inline bool RailShapeIsSlope(RailShape s) {
        return s == RailShape::AscendingNorth || s == RailShape::AscendingEast ||
               s == RailShape::AscendingSouth || s == RailShape::AscendingWest;
    }

    RailShape  RailShapeOf(BlockState state);
    BlockState WithRailShape(BlockState state, RailShape shape);

    // MC BaseRailBlock.getStateForPlacement's shape half: the track runs
    // along the axis the player faces.
    BlockState RailPlacementState(BlockState state, Direction horizontalDirection);

    void RegisterRailBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
