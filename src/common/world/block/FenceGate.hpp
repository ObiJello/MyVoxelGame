// File: src/common/world/block/FenceGate.hpp
//
// Fence gates — port of MC FenceGateBlock.
//
// A HorizontalDirectionalBlock, not a CrossCollisionBlock: it carries FACING
// plus OPEN, POWERED and IN_WALL. Three of those do visible work here —
//
//   OPEN     right-click swings the gate, and an open gate has NO collision
//            shape at all, which is how you walk through it;
//   IN_WALL  a gate flanked by walls drops three pixels so its top lines up
//            with the wall it is set into;
//   FACING   which way it swings, re-aimed at the player when they open a gate
//            that was facing away from them.
//
// POWERED is modelled because MC's state definition has it and the blockstate
// index has to line up, but nothing drives it — there is no redstone here.
#pragma once

#include "Blocks.hpp"
#include "Direction.hpp"
#include "BlockRegistry.hpp"
#include "../chunk/IBlockAccess.hpp"
#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    bool IsFenceGateBlock(BlockID id);

    Direction FenceGateFacing(BlockState state);
    bool      FenceGateOpen(BlockState state);
    bool      FenceGateInWall(BlockState state);

    BlockState FenceGateStateFrom(BlockState state,
                                  Direction facing, bool open, bool powered, bool inWall);

    // MC FenceGateBlock.connectsToDirection:
    //   state.getValue(FACING).getAxis() == direction.getClockWise().getAxis()
    // A fence or wall meets the gate's HINGE side, never its face — which is
    // why a gate set into a fence line looks continuous.
    bool FenceGateConnectsToDirection(BlockState state, Direction dir);

    // MC getStateForPlacement's IN_WALL half (FACING comes from the shared
    // horizontal placement rule, and OPEN/POWERED both start false with no
    // redstone to read).
    BlockState FenceGatePlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState fallback);

    // MC updateShape: only a change along the gate's hinge axis matters, and
    // only IN_WALL can change.
    BlockState FenceGateUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                    BlockState state, Direction toNeighbour);

    // MC useWithoutItem: an open gate closes; a closed one re-aims at the
    // player if it was facing away, then opens. Returns the new state.
    BlockState FenceGateToggle(BlockState state, Direction playerFacing);

    // MC getShape — the gate's outline. IN_WALL lowers the top by three pixels.
    BlockRegistry::BlockShapeSet FenceGateShapeBoxes(BlockState state);

    // MC getCollisionShape — EMPTY when open (walk through), and a block and a
    // half tall when shut, like the fence it joins.
    BlockRegistry::BlockShapeSet FenceGateCollisionBoxes(BlockState state);

} // namespace Game
