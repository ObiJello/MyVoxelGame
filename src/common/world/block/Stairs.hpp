// File: src/common/world/block/Stairs.hpp
//
// Stair blocks — port of MC StairBlock.java.
//
// Vanilla keeps FACING, HALF, SHAPE and WATERLOGGED on one block class and
// derives everything else (the VoxelShape, the corner it forms with its
// neighbours, the state it takes on placement) from those four. This file is
// that class's logic; the four properties themselves are declared by
// StateKind::Stairs in BlockRegistry.cpp, and WATERLOGGED is handled by the
// generic SimpleWaterloggedBlock pass in BlockPlacement.cpp — a stair needs no
// special case for it, exactly as in MC.
//
// Structured like RedstoneWire.hpp: free functions over (level, pos, state)
// rather than a Block subclass, because this engine's blocks are data rows in
// a registry and their behaviour hangs off function pointers.
#pragma once

#include "Blocks.hpp"
#include "Direction.hpp"
#include "BlockRegistry.hpp"
#include "../chunk/IBlockAccess.hpp"
#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    // MC StairsShape (StairsShape.java), in declaration order — which is also
    // the order kStairsShapeValues lists them, so the enum value IS the index
    // of the `shape` property's value.
    enum class StairsShape : uint8_t {
        Straight   = 0,
        InnerLeft  = 1,
        InnerRight = 2,
        OuterLeft  = 3,
        OuterRight = 4,
    };

    // MC Half (Half.java). BOTTOM is StairBlock's registered default.
    enum class StairHalf : uint8_t { Bottom = 0, Top = 1 };

    // MC StairBlock.isStairs(state) — `state.getBlock() instanceof StairBlock`.
    // Every block whose model name ends in "_stairs", which is how the rest of
    // this engine classifies families too.
    bool IsStairs(BlockID id);

    // The three properties. Asking these of a non-stair returns the defaults
    // (north / bottom / straight) rather than reading garbage out of another
    // block's property layout — BlockState::GetIndex answers -1 for a property
    // the block does not declare, which is what makes that safe.
    Direction    StairFacing(BlockState state);
    StairHalf    StairHalfOf(BlockState state);
    StairsShape  StairShapeOf(BlockState state);

    // `state.setValue(FACING, f).setValue(HALF, h).setValue(SHAPE, s)`, keeping
    // WATERLOGGED as it is on `state`.
    BlockState StairStateFrom(BlockState state,
                              Direction facing, StairHalf half, StairsShape shape);

    // MC StairBlock.getShape — the VoxelShape for one state, as a box union.
    // Bottom-half shapes are the model geometry; the top half is the bottom
    // one inverted in Y, which is exactly how vanilla derives SHAPE_TOP_* from
    // SHAPE_* (`Shapes.rotateHorizontal(SHAPE_*, OctahedralGroup.INVERT_Y)`).
    BlockRegistry::BlockShapeSet StairShapeBoxes(BlockState state);

    // MC StairBlock.getStairsShape(state, level, pos) — which of the five
    // shapes this stair takes given what its neighbours are doing.
    StairsShape StairsShapeAt(const IBlockAccess& level, const glm::ivec3& pos,
                              BlockState state);

    // MC StairBlock.getStateForPlacement's FACING + HALF half. SHAPE needs the
    // world and is applied afterwards by StairsWorldPlacementState.
    //
    //   FACING = context.getHorizontalDirection()
    //   HALF   = clickedFace != DOWN && (clickedFace == UP || hitY <= 0.5)
    //              ? BOTTOM : TOP
    //
    // `clickedFace` is the face of the block that was clicked, `hitY` the click
    // point's height within the cell the stair will occupy, both in MC terms.
    BlockState StairsPlacementState(BlockID id, Direction horizontalFacing,
                                    Direction clickedFace, float hitY);

    // The `.setValue(SHAPE, getStairsShape(state, level, pos))` that closes
    // getStateForPlacement.
    BlockState StairsWorldPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state);

    // MC StairBlock.updateShape: a HORIZONTAL neighbour change re-derives
    // SHAPE; a vertical one leaves the stair alone. Returns the state the
    // stair becomes.
    BlockState StairsUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                 BlockState state, Direction toNeighbour);

} // namespace Game
