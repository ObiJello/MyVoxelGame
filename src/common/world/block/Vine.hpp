// File: src/common/world/block/Vine.hpp
//
// MC VineBlock (net.minecraft.world.level.block.VineBlock).
//
// A vine is five independent booleans — UP, NORTH, EAST, SOUTH, WEST — one per
// face it clings to. DOWN is deliberately absent: vines never attach downward,
// they hang from the block above.
//
// Everything here is a port of that class; the only behaviour NOT ported is
// randomTick spreading, which is gated on the `spreadVines` game rule and is a
// gameplay feature rather than part of making the block work.
#pragma once

#include "BlockRegistry.hpp"
#include "BlockState.hpp"
#include "Blocks.hpp"
#include "Direction.hpp"
#include <glm/vec3.hpp>

namespace Game {

    class IBlockAccess;

    // The `vine` block only. weeping_vines / twisting_vines / cave_vines are
    // different MC classes with different properties, and matching them on the
    // substring "vine" is exactly the trap the registry already documents.
    bool IsVineBlock(BlockID id);

    // Is this face clung to? Always false for DOWN, which vines do not model.
    bool VineFaceOf(BlockState state, Direction face);
    BlockState VineStateWithFace(BlockState state, Direction face, bool on);
    int  VineCountFaces(BlockState state);

    // MC VineBlock.canSupportAtFace:
    //   DOWN                      -> false
    //   neighbour's face is full  -> true
    //   vertical                  -> false
    //   otherwise                 -> the vine ABOVE clings to the same face
    //
    // That last clause is what lets a vine hang in open air below another vine.
    bool VineCanSupportAtFace(const IBlockAccess& level, const glm::ivec3& pos,
                              BlockState state, Direction face);

    // MC VineBlock.getUpdatedState — clears every face that lost its support,
    // leaving the rest alone. Does NOT decide whether the vine survives; that
    // is `VineCountFaces(result) > 0`.
    BlockState VineUpdatedState(const IBlockAccess& level, const glm::ivec3& pos,
                                BlockState state);

    // MC VineBlock.canSurvive — `hasFaces(getUpdatedState(...))`.
    bool VineCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, BlockState state);

    // MC VineBlock.updateShape. A change BELOW never affects a vine; anything
    // else re-derives the faces, and a vine with none left becomes AIR (the
    // caller destroys it with drops, as vanilla does).
    BlockState VineUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                               BlockState state, Direction toNeighbour);

    // MC VineBlock.getStateForPlacement's direction search: take the face the
    // click implies, then fall back through the others, and keep the first that
    // has support. Returns `state` unchanged when nothing can hold it — the
    // survival gate then refuses the placement.
    BlockState VinePlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                  BlockState state, Direction clickedFace);

    // MC VineBlock.makeShapes: `Shapes.rotateAll(Block.boxZ(16, 0, 1))` — a
    // 1-pixel slab against each clung face, unioned. A vine with no faces gets
    // the full cube, exactly as vanilla's `shape.isEmpty() ? Shapes.block()`.
    BlockRegistry::BlockShapeSet VineShapeBoxes(BlockState state);

} // namespace Game
