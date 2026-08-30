// File: src/common/world/block/ExplosionTrigger.hpp
//
// MC's Explosion.BlockInteraction.TRIGGER_BLOCK responses — the per-block
// `onExplosionHit` overrides that a wind charge reaches.
//
// This is the half of an explosion that breaks nothing. A TRIGGER blast still
// runs ServerExplosion.interactWithBlocks over every cell it touched;
// BlockBehaviour.onExplosionHit then returns immediately for TRIGGER_BLOCK, so
// the DEFAULT behaviour is "do nothing". The blocks that do respond override
// it, and every one of them guards on `explosion.canTriggerBlocks()`:
//
//   ButtonBlock         press (POWERED = true), if not already powered
//   LeverBlock          pull  (cycle POWERED)
//   DoorBlock           toggle OPEN, lower half only, wooden only, if unpowered
//   TrapDoorBlock       toggle OPEN, wooden only, if unpowered
//   FenceGateBlock      toggle OPEN, if unpowered
//   AbstractCandleBlock extinguish, if LIT
//   BellBlock           ring
//
// Kept as a free-function module, matching Stairs.cpp / Walls.cpp / the rest of
// this directory, because Game::Block is a POD with function pointers and there
// is nothing to subclass.
//
// NOTE on redstone: pressing a button and pulling a lever write POWERED and
// nothing else, because this engine has no redstone yet (see RedstoneSignal.
// hpp). The state change, its model and its sound site are all correct — the
// day a signal graph lands, these become fully functional with no edit here.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;

    // Apply this block's TRIGGER_BLOCK response, if it has one. Returns true
    // when the block changed state.
    bool ExplosionTriggerBlock(ILevelWrite& level, const glm::ivec3& pos,
                               BlockState state);

} // namespace Game
