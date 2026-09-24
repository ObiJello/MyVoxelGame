// File: src/common/world/block/RedstoneWire.hpp
//
// Port of MC RedstoneWireBlock — both halves. The SHAPE half decides which
// way the dust points (getStateForPlacement / getConnectionState /
// getConnectingSide / shouldConnectTo / updateShape / useWithoutItem); the
// POWER half is the classic DefaultRedstoneWireEvaluator plus the wire's own
// getSignal / getDirectSignal / neighborChanged / onPlace /
// affectNeighborsAfterRemoval. The experimental evaluator (feature flag
// `redstone_experiments`, off by default) is not ported: the default one is
// what every vanilla world and every published contraption runs on.
#pragma once

#include "Blocks.hpp"
#include "BlockState.hpp"
#include "Direction.hpp"
#include "../chunk/IBlockAccess.hpp"

#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;
    struct ScheduledTickAccess;

    // MC RedstoneSide.
    enum class RedstoneSide : uint8_t { None = 0, Side = 1, Up = 2 };
    inline bool IsConnected(RedstoneSide s) { return s != RedstoneSide::None; }

    RedstoneSide RedstoneSideOf(BlockState state, Direction dir);

    // Build a wire state from the four sides (MC PROPERTY_BY_DIRECTION
    // order) and a power level. `power` is carried through every shape
    // rebuild, exactly as vanilla's `setValue(POWER, state.getValue(POWER))`.
    BlockState RedstoneStateFrom(RedstoneSide north, RedstoneSide east,
                                 RedstoneSide south, RedstoneSide west, int power);

    bool RedstoneIsCross(BlockState state);
    bool RedstoneIsDot(BlockState state);

    // MC getConnectionState(level, state, pos): resolve the sides from the
    // world, then apply the "a wire with nothing on one axis still points
    // both ways along it" rule. `state` supplies the starting sides (a dot
    // stays a dot only if it started as one) and the power to keep.
    BlockState RedstoneConnectionState(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState state);

    // MC getStateForPlacement — getConnectionState(level, crossState, pos).
    BlockState RedstonePlacementState(const IBlockAccess& level, const glm::ivec3& pos);

    // MC updateShape. Returns the state this wire becomes (air when its
    // support below is gone). `changed` is the direction from the wire
    // toward the neighbour that changed.
    BlockState RedstoneUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                   BlockState state, Direction changed);

    // MC RedstoneWireBlock.shouldConnectTo(state, level, pos, direction) —
    // dispatches to the neighbour's `shouldRedstoneWireConnectTo`, which only
    // dust, repeaters and observers override. `haveDirection == false` is
    // vanilla passing null.
    bool RedstoneShouldConnectTo(const IBlockAccess& level, const glm::ivec3& pos,
                                 Direction dir, bool haveDirection);

    // MC canSurviveOn: a sturdy top face, or a hopper.
    bool RedstoneCanSurviveOn(const IBlockAccess& level, const glm::ivec3& pos);

    // ── Power (server-side, all reached through the block hooks) ──────────
    int  RedstoneWireGetSignal(const IBlockAccess& level, const glm::ivec3& pos,
                               BlockState state, Direction direction);
    int  RedstoneWireGetDirectSignal(const IBlockAccess& level, const glm::ivec3& pos,
                                     BlockState state, Direction direction);
    void RedstoneWireOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                             BlockState state, BlockState oldState, bool movedByPiston);
    void RedstoneWireAfterRemoval(ILevelWrite& level, const glm::ivec3& pos,
                                  BlockState state, bool movedByPiston);
    void RedstoneWireNeighborChanged(ILevelWrite& level, const glm::ivec3& pos,
                                     BlockState state, BlockID sourceBlock, bool movedByPiston);
    void RedstoneWireUpdateIndirectNeighbourShapes(ILevelWrite& level, const glm::ivec3& pos,
                                                   BlockState state, uint32_t updateFlags,
                                                   int updateLimit);
    bool RedstoneWireUpdateShapeHook(const IBlockAccess& level, const glm::ivec3& pos,
                                     BlockState state, Direction toNeighbour, BlockID neighbourId,
                                     BlockState& outState, ScheduledTickAccess* ticks);

    // MC useWithoutItem's toggle. Writes the world (setBlockAndUpdate +
    // updatesOnShapeChange) and returns whether anything changed.
    bool RedstoneWireToggle(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

    // MC RedstoneWireBlock.getColorForPower — the 16-entry tint table the
    // mesher reads for dust.
    uint32_t RedstoneWireColorForPower(int power);

} // namespace Game
