// File: src/common/world/block/piston/PistonBaseBlock.hpp
//
// MC PistonBaseBlock, PistonHeadBlock and MovingPistonBlock — the piston,
// the sticky piston, the arm, and the invisible cell a block occupies while
// in transit. PistonMovingBlockEntity carries the transit state;
// PistonStructureResolver decides what moves.
//
// Replication is vanilla's: the server writes the moving cells with flags
// 324 / 276 (no UPDATE_CLIENTS) and broadcasts the block EVENT; the client
// runs this same triggerEvent against its own level, creates its own
// PistonMovingBlockEntity, ticks it, and pushes its own player. The server's
// final block write (flag 67) is what both sides converge on.
#pragma once

#include "common/world/block/BlockRegistry.hpp"

#include <array>

namespace Game {

    void RegisterPistonBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

    // MC PistonHeadBlock.canSurvive — the head needs its extended base
    // (or a moving piston) behind it.
    bool PistonHeadCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, BlockState state);

    // MC PistonBaseBlock.setPlacedBy → checkIfExtend.
    void PistonPlacedBy(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

    // MC Block.updateFromNeighbourShapes — the moved block asks each
    // neighbour in UPDATE_SHAPE_ORDER what it should become before it is
    // written for real.
    BlockState UpdateFromNeighbourShapes(ILevelWrite& level, BlockState state, const glm::ivec3& pos);

} // namespace Game
