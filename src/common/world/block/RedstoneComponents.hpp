// File: src/common/world/block/RedstoneComponents.hpp
//
// The redstone components that are neither dust (RedstoneWire.hpp), rails
// (Rails.hpp), pistons (piston/) nor containers (entity/): ports of MC's
// RedstoneTorchBlock, RedstoneWallTorchBlock, DiodeBlock, RepeaterBlock,
// ComparatorBlock, ObserverBlock, RedstoneLampBlock, PoweredBlock,
// LeverBlock, ButtonBlock, BasePressurePlateBlock (+ the plain and weighted
// plates), NoteBlock, TargetBlock, DaylightDetectorBlock, RedStoneOreBlock,
// CopperBulbBlock, TntBlock's redstone half, the DoorBlock / TrapDoorBlock /
// FenceGateBlock neighborChanged, and TripWireBlock + TripWireHookBlock.
//
// Every function here is one MC method, named after it, and the file is
// organised block by block so a port can be checked against the decompile
// top to bottom. Wiring into the block table is RegisterRedstoneBehaviors,
// called from BlockRegistry_RegisterBehaviors.
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    // Fill in the redstone hooks for every block this file ports.
    void RegisterRedstoneBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

    // MC BlockBehaviour.canSurvive for the components with a rule of their
    // own (torches, diodes, plates, hooks, tripwire, daylight…). False only
    // for a block this file models AND that cannot stand where it is; a
    // block outside the set answers true so the caller can ask blindly.
    bool RedstoneComponentCanSurvive(const IBlockAccess& level, const glm::ivec3& pos,
                                     BlockState state);
    bool HasRedstoneSurvivalRule(BlockID id);

    // MC LightningRodBlock — the eight copper stages of the rod.
    bool IsLightningRodBlock(BlockID id);
    // MC LightningRodBlock.onLightningStrike: POWERED for 8 ticks (strong
    // power into the block behind it), the neighbours told, the electric
    // sparks (level event 3002). Called by the LightningBolt's first tick.
    void LightningRodOnLightningStrike(ILevelWrite& level, const glm::ivec3& pos, BlockState state);
    // redstone_plus: run the delayed components' re-checks that were deferred
    // during this tick's update cascades (see RedstoneComponents.cpp).
    void RedstoneFlushDeferredChecks(ILevelWrite& level);

    // MC DaylightDetectorBlock.updateSignalStrength — public because the
    // block entity's 20-tick ticker drives it.
    void DaylightDetectorUpdateSignalStrength(ILevelWrite& level, const glm::ivec3& pos,
                                              BlockState state);

    // MC NoteBlock.setInstrument's lookup — public for placement.
    BlockState NoteBlockWithInstrument(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState state);

    // MC TripWireBlock.getStateForPlacement's connection half — public for
    // placement.
    BlockState TripWirePlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                      BlockState state);

    // MC RepeaterBlock.getStateForPlacement's LOCKED half.
    BlockState RepeaterPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                      BlockState state);

    // MC ButtonBlock / LeverBlock.onExplosionHit for a blast that can
    // trigger blocks (a wind charge): press the button (unless already
    // pressed) / pull the lever, exactly as a click would — neighbours and
    // the block it hangs on updated, the release tick scheduled, the click
    // sounded. False for any other block, or a button already down.
    bool RedstoneComponentOnExplosionHit(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

    // MC TripWireHookBlock.setPlacedBy / DiodeBlock.setPlacedBy — the bits of
    // placement that need a writable level, run by the server after the
    // block is in.
    void RedstoneComponentPlacedBy(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

} // namespace Game
