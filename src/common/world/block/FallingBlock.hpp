// File: src/common/world/block/FallingBlock.hpp
//
// MC net.minecraft.world.level.block.FallingBlock and its family — sand,
// gravel, concrete powder, anvils, the dragon egg, scaffolding, pointed
// dripstone and the two suspicious (brushable) blocks.
//
// HOW FALLING ACTUALLY WORKS, because it is not what people assume: a falling
// block does NOT check its support every tick. It books a SCHEDULED TICK two
// ticks out whenever it is placed or a neighbour changes, and that tick asks
// once whether the cell below is free. Everything else — the cascade of a
// collapsing column, the two-tick pause before a pillar goes — falls out of
// that one mechanism plus the neighbour updates each removal fires.
//
// So the family needs exactly three hooks: `onPlace` and `neighborChanged` to
// book the appointment, and `tick` to keep it. None of them is per-frame work.
//
// A free-function module wired from BlockBehaviors.cpp, following Stairs.cpp /
// Walls.cpp / MultifaceBlock.cpp rather than a class, because Game::Block is a
// POD with function pointers and there is nothing to subclass.
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/Direction.hpp"

#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;
    struct EntityLevel;
    class  ILevelWrite;
    class  JavaRandom;
    struct ScheduledTickAccess;

    // ── Family membership ──────────────────────────────────────────────────

    // Every block that falls, including the ones whose fall is driven by their
    // own tick rule (scaffolding, dripstone) rather than by FallingBlockTick.
    bool IsFallingBlock(BlockID id);
    bool IsConcretePowder(BlockID id);
    bool IsAnvil(BlockID id);

    // MC FallingBlock.isFree(state) — what counts as "nothing holding this up".
    // Air, anything in #fire, any liquid, and anything replaceable. Note this
    // is deliberately LOOSER than "is not solid": a torch is not replaceable,
    // so sand rests on top of one, which is vanilla.
    bool FallingBlockIsFree(BlockState state);

    // MC FallingBlock.getDelayAfterPlace(). Two for the sand/gravel/concrete/
    // anvil set, five for the dragon egg, one for scaffolding, and for pointed
    // dripstone one when the tip points up and two when it points down.
    int FallingBlockDelayAfterPlace(BlockState state);

    // ── The three hooks ────────────────────────────────────────────────────

    // MC FallingBlock.onPlace — schedules, nothing else.
    void FallingBlockOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                             BlockState newState, BlockState oldState);

    // MC FallingBlock.updateShape — also only schedules, and returns the state
    // unchanged (so this always answers false: no transform).
    bool FallingBlockNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                     BlockState state,
                                     Direction toNeighbour, BlockID neighbourId,
                                     BlockState& outState,
                                     ScheduledTickAccess* ticks);

    // MC ConcretePowderBlock.updateShape — solidifies on contact with water
    // BEFORE it would schedule a fall, which is why concrete powder needs its
    // own hook rather than sharing the family one. Returning a transform makes
    // World::NotifyNeighborBlocks write the concrete and skip the schedule,
    // which is exactly vanilla's ordering.
    bool ConcretePowderNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState state,
                                       Direction toNeighbour, BlockID neighbourId,
                                       BlockState& outState,
                                       ScheduledTickAccess* ticks);

    // MC ConcretePowderBlock.getStateForPlacement — placing powder into or
    // beside water gives CONCRETE, not powder waiting for a tick. Returns a
    // state of a DIFFERENT BLOCK in that case, which is why the placement
    // callers must read the block back off the returned state.
    BlockState ConcretePowderPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                            BlockState fallback);

    // MC FallingBlock.tick — the appointment. Spawns the entity if the cell
    // below is free.
    void FallingBlockTick(ILevelWrite& level, const glm::ivec3& pos,
                          BlockState state, JavaRandom& random);

    // MC ScaffoldingBlock.tick — recompute `distance`, and at 7 either fall
    // (if it was already 7) or break.
    void ScaffoldingTick(ILevelWrite& level, const glm::ivec3& pos,
                         BlockState state, JavaRandom& random);
    void ScaffoldingOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                            BlockState newState, BlockState oldState);
    bool ScaffoldingNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                    BlockState state,
                                    Direction toNeighbour, BlockID neighbourId,
                                    BlockState& outState,
                                    ScheduledTickAccess* ticks);
    // MC ScaffoldingBlock.getDistance — 0 on a sturdy floor, else one more than
    // the smallest neighbouring scaffolding, capped at 7 (= unsupported).
    int ScaffoldingDistance(const IBlockAccess& level, const glm::ivec3& pos);
    // MC ScaffoldingBlock.canSurvive — distance < 7.
    bool ScaffoldingCanSurvive(const IBlockAccess& level, const glm::ivec3& pos);
    // MC ScaffoldingBlock.getStateForPlacement — DISTANCE and BOTTOM resolved
    // against the neighbours at placement time.
    BlockState ScaffoldingPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState fallback);

    // MC PointedDripstoneBlock.tick — a stalagmite that lost its support
    // breaks; a stalactite collapses as a column of falling entities.
    void PointedDripstoneTick(ILevelWrite& level, const glm::ivec3& pos,
                              BlockState state, JavaRandom& random);
    bool PointedDripstoneNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state,
                                         Direction toNeighbour, BlockID neighbourId,
                                         BlockState& outState,
                                         ScheduledTickAccess* ticks);
    // MC PointedDripstoneBlock.canSurvive — the cell behind the tip holds.
    bool PointedDripstoneCanSurvive(const IBlockAccess& level, const glm::ivec3& pos,
                                    BlockState state);
    // MC PointedDripstoneBlock.getStateForPlacement — TIP_DIRECTION from the
    // player's vertical look (falling back to the opposite when that side has
    // no support) and THICKNESS from the column it joins.
    BlockState PointedDripstonePlacementState(const IBlockAccess& level,
                                              const glm::ivec3& pos,
                                              BlockState fallback,
                                              bool defaultTipDown, bool secondaryUse);

    // ── Landing behaviour (MC Fallable) ────────────────────────────────────

    // MC Fallable.onLand — called after the entity successfully placed itself.
    // `replaced` is what was in the cell just before.
    void FallingBlockOnLand(ILevelWrite& level, const glm::ivec3& pos,
                            BlockState placed, BlockState replaced);

    // MC Fallable.onBrokenAfterFall — called when the entity could NOT place
    // itself and popped as an item instead.
    void FallingBlockOnBrokenAfterFall(ILevelWrite& level, const glm::ivec3& pos,
                                       BlockState state);

    // MC AnvilBlock.damage — anvil -> chipped -> damaged -> gone (Air means the
    // anvil is destroyed outright, which is MC returning null).
    BlockID AnvilDamaged(BlockID anvil);

    // MC ConcretePowderBlock's `concrete` field — the solid block this powder
    // becomes. Air for anything that is not concrete powder.
    BlockID ConcreteFor(BlockID powder);

    // MC ConcretePowderBlock.shouldSolidify / touchesLiquid.
    bool ConcretePowderShouldSolidify(const IBlockAccess& level, const glm::ivec3& pos,
                                      BlockState replaced);

    // ── Falling dust (MC FallingBlock.getDustColor) ────────────────────────

    // 0xRRGGBB for the FALLING_DUST particle under an unsupported block. Sand,
    // red sand and gravel carry a hardcoded ColorRGBA in their
    // ColoredFallingBlock constructor; everything else reads its map colour.
    uint32_t FallingBlockDustColor(BlockState state);

    // MC FallingBlock.animateTick — one dust particle below the block, 1 tick
    // in 16, only while the cell below is free.
    //
    // CLIENT-SIDE ONLY, and driven by the client's animate-tick sweep rather
    // than by the server: in MC this runs from ClientLevel.animateTick, which
    // samples ~1300 random positions around the camera every tick. Takes
    // EntityLevel rather than ILevelWrite because the particle sink lives
    // there.
    void FallingBlockAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                                 BlockState state, JavaRandom& random);

} // namespace Game
