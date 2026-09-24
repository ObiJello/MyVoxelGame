// File: src/common/world/block/RedstoneSignal.hpp
//
// MC net.minecraft.world.level.SignalGetter — the six default methods every
// level inherits for asking "how much power reaches this cell?" — plus the
// two BlockState predicates they lean on (isRedstoneConductor, isSignalSource).
//
// These are free functions over IBlockAccess rather than members of it
// because they need the block registry, which IBlockAccess (included by half
// the engine) must not pull in. Vanilla's are interface defaults for the same
// reason in reverse: they live on the reader, not on the block.
//
// VOCABULARY, because it is easy to invert:
//   * `getSignal(pos, direction)` asks the block AT `pos` how much WEAK power
//     it emits toward `direction`, where `direction` points FROM the block
//     that is asking TOWARD `pos`. `hasNeighborSignal(p)` therefore calls
//     `getSignal(p.below(), DOWN)`.
//   * `getDirectSignal` is the same question for STRONG power — the kind that
//     charges a solid block so it can power dust on its far side.
//   * A conductor answers `getSignal` with the max of its own emission and
//     the strong power pushed INTO it from any side (getDirectSignalTo).
#pragma once

#include "Blocks.hpp"
#include "BlockState.hpp"
#include "Direction.hpp"

#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;

    // BlockState.isRedstoneConductor(level, pos): Blocks.java's never/always
    // override, else "the collision shape is a full cube".
    bool IsRedstoneConductor(const IBlockAccess& level, const glm::ivec3& pos, BlockState state);
    bool IsRedstoneConductor(const IBlockAccess& level, const glm::ivec3& pos);

    // BlockState.isSignalSource.
    bool IsSignalSource(BlockState state);

    // BlockState.getSignal / getDirectSignal — the block's own answer, with
    // no conductor logic. Public because the wire's evaluator and the
    // comparator ask blocks directly.
    int GetBlockSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                       Direction direction);
    int GetBlockDirectSignal(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                             Direction direction);

    // SignalGetter.getDirectSignal(pos, direction).
    int GetDirectSignal(const IBlockAccess& level, const glm::ivec3& pos, Direction direction);

    // SignalGetter.getDirectSignalTo(pos): the strongest strong signal any
    // of the six neighbours pushes into `pos`.
    int GetDirectSignalTo(const IBlockAccess& level, const glm::ivec3& pos);

    // SignalGetter.getSignal(pos, direction).
    int GetSignal(const IBlockAccess& level, const glm::ivec3& pos, Direction direction);

    // SignalGetter.hasSignal.
    bool HasSignal(const IBlockAccess& level, const glm::ivec3& pos, Direction direction);

    // SignalGetter.hasNeighborSignal — "is anything next to this cell
    // powering it?" The question a lamp, a door, a piston and TNT ask.
    bool HasNeighborSignal(const IBlockAccess& level, const glm::ivec3& pos);

    // SignalGetter.getBestNeighborSignal — the strength version, for dust
    // and comparators.
    int GetBestNeighborSignal(const IBlockAccess& level, const glm::ivec3& pos);

    // SignalGetter.getControlInputSignal — a diode's rear or side input:
    // with `onlyDiodes` only another repeater/comparator counts (side
    // inputs); otherwise a redstone block is 15, dust is its power, and any
    // other source is its strong output.
    int GetControlInputSignal(const IBlockAccess& level, const glm::ivec3& pos,
                              Direction direction, bool onlyDiodes);

    // SignalGetter.getBestOwnOrNeighbourSignal — used by nothing here yet
    // but part of the interface; kept for the sculk/target ports.
    int GetBestOwnOrNeighbourSignal(const IBlockAccess& level, const glm::ivec3& pos);

} // namespace Game
