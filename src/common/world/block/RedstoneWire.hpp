// File: src/common/world/block/RedstoneWire.hpp
//
// Redstone dust connection logic — port of MC RedStoneWireBlock's SHAPE half
// (getStateForPlacement / getConnectionState / getConnectingSide /
// shouldConnectTo / updateShape / useWithoutItem).
//
// SHAPE only. MC's RedStoneWireBlock does two separable jobs: work out which
// way the dust points, and propagate power. This is the first. There is no
// redstone power simulation in this engine, so POWER is not modelled at all —
// see StateKind::RedstoneWire in BlockRegistry.cpp for why it also could not
// fit in the state index. Everything here is exactly what vanilla does with
// power held at 0.
#pragma once

#include "Blocks.hpp"
#include "Direction.hpp"
#include "../chunk/IBlockAccess.hpp"
#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    // MC RedstoneSide.
    enum class RedstoneSide : uint8_t { None = 0, Side = 1, Up = 2 };
    inline bool IsConnected(RedstoneSide s) { return s != RedstoneSide::None; }

    // Read one side out of a wire's state index.
    RedstoneSide RedstoneSideOf(BlockState state, Direction dir);

    // Build a state index from the four sides (MC's PROPERTY_BY_DIRECTION order).
    BlockState RedstoneStateFrom(RedstoneSide north, RedstoneSide east,
                                 RedstoneSide south, RedstoneSide west);

    // MC RedStoneWireBlock.isCross / isDot — all four connected / none connected.
    bool RedstoneIsCross(BlockState state);
    bool RedstoneIsDot(BlockState state);

    // MC getConnectionState: resolve every side from the world, then apply the
    // "a wire with nothing on one axis still points both ways along it" rule
    // that turns a bare dust into a cross.
    //
    // `startFromCross` mirrors vanilla passing either crossState (placement,
    // and the right-click toggle's cross half) or the block's current state.
    BlockState RedstoneConnectionState(const IBlockAccess& level, const glm::ivec3& pos,
                                       bool startFromCross);

    // MC getStateForPlacement — getConnectionState(level, crossState, pos).
    inline BlockState RedstonePlacementState(const IBlockAccess& level, const glm::ivec3& pos) {
        return RedstoneConnectionState(level, pos, /*startFromCross=*/true);
    }

    // MC updateShape, for a horizontal or upward neighbour change. Returns the
    // state this wire becomes. `changed` is the direction from the wire toward
    // the neighbour that changed.
    BlockState RedstoneUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                   BlockState state, Direction changed);

    // MC useWithoutItem: a wire that is currently a full cross collapses to a
    // dot, and a dot expands back to a cross. Anything in between is left
    // alone. Returns true and fills `outState` when the state should change.
    bool RedstoneToggleShape(const IBlockAccess& level, const glm::ivec3& pos,
                             BlockState state, BlockState& outState);

} // namespace Game
