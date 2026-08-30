// File: src/common/world/block/TntBlock.hpp
//
// MC net.minecraft.world.level.block.TntBlock.
//
// TNT itself does almost nothing: every path here ends in `TntPrime`, which
// swaps the block for a PrimedTnt entity. The explosion lives on that entity,
// not on the block.
//
// ── Which ignition paths work today ──────────────────────────────────────
//
// Working: flint & steel, fire charge, chain detonation from another blast,
// a burning projectile, and breaking an `unstable` TNT in survival.
//
// Blocked on subsystems that do not exist yet, each reduced to ONE named seam
// so that wiring it up later is a one-line change rather than an archaeology
// exercise:
//
//   * REDSTONE — `HasNeighborSignal` in RedstoneSignal.hpp answers false today
//     because there is no power simulation at all (RedstoneWire.hpp:8-12 holds
//     POWER at 0). onPlace and neighborChanged below already call it, so a
//     lever will light TNT the day that function grows a body.
//
//   * FIRE SPREAD — MC ignites TNT inside FireBlock.checkBurnOut, gated on the
//     flammability table (TNT is 15/100). The table entry and the call are in
//     BlockFlammability.hpp; nothing runs the fire tick that would reach them.
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/Direction.hpp"

#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;
    class  ILevelWrite;
    class  Entity;
    struct ScheduledTickAccess;
    struct ItemStack;
    class  IUsePlayer;
    struct BlockHitResult;
    enum class UseResult : int;

    // MC TntBlock.prime(level, pos, source) — replace the block with a lit
    // entity. Returns false when the `tntExplodes` gamerule is off, which is
    // the signal TntUseItemOn uses to leave the block alone and tell the player.
    //
    // Does NOT clear the cell — MC's callers each remove the block themselves,
    // and they do it with different flags (removeBlock for the redstone path,
    // setBlock(AIR, 11) for the flint-and-steel one).
    bool TntPrime(ILevelWrite& level, const glm::ivec3& pos, Entity* igniter);

    // MC TntBlock.onPlace / neighborChanged — both are the redstone check.
    void TntOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                    BlockState newState, BlockState oldState);
    bool TntNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                            BlockState state,
                            Direction toNeighbour, BlockID neighbourId,
                            BlockState& outState,
                            ScheduledTickAccess* ticks);

    // MC TntBlock.useItemOn — flint & steel or a fire charge lights it.
    //
    // Runs BEFORE the item's own useOn, which is the entire point: without it
    // flint & steel would place a FIRE BLOCK on top of the TNT instead of
    // lighting it (see BlockUseItemOnFn's contract in BlockRegistry.hpp).
    UseResult TntUseItemOn(ItemStack& stack, ILevelWrite* level, const glm::ivec3& pos,
                           IUsePlayer* player, uint32_t hand,
                           const BlockHitResult& hit);

    // MC TntBlock.playerWillDestroy — an `unstable` TNT primes instead of
    // dropping when a survival player breaks it. Returns true when it primed,
    // in which case the caller must NOT also drop the block.
    bool TntPlayerWillDestroy(ILevelWrite& level, const glm::ivec3& pos,
                              BlockState state, Entity* player, bool creative);

    // MC TntBlock.onProjectileHit — a BURNING projectile lights it. Returns
    // true when it primed (the caller then removes the block).
    bool TntOnProjectileHit(ILevelWrite& level, const glm::ivec3& pos,
                            Entity* projectile, Entity* owner);

} // namespace Game
