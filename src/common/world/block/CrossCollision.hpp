// File: src/common/world/block/CrossCollision.hpp
//
// Fences, glass panes and iron bars — port of MC CrossCollisionBlock and its
// two subclasses, FenceBlock and IronBarsBlock.
//
// All three share one shape: a centre post plus up to four arms reaching to
// the cell edge, chosen by four boolean properties NORTH/EAST/SOUTH/WEST that
// are derived from the neighbours. Vanilla's blockstate files are `multipart`
// and dispatch on exactly those four, which is why this matters for rendering
// as much as for collision: with the properties unmodelled the multipart file
// is unjudgeable, BlockStateModels refuses it, and the block falls back to a
// plain model that does not exist — a default cube with the missing texture.
//
// The two subclasses differ only in how thick they are and in what they agree
// to connect to (FenceBlock.connectsTo vs IronBarsBlock.attachsTo), so both
// live here, the way MC keeps them one class apart.
#pragma once

#include "Blocks.hpp"
#include "Direction.hpp"
#include "BlockRegistry.hpp"
#include "../chunk/IBlockAccess.hpp"
#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    // Any block in the family — declares NORTH/EAST/SOUTH/WEST.
    bool IsCrossCollisionBlock(BlockID id);

    // MC FenceBlock (every wooden fence plus nether_brick_fence).
    bool IsFenceBlock(BlockID id);

    // #minecraft:wooden_fences. FenceBlock.isSameFence refuses to connect a
    // wooden fence to a nether brick one and vice versa, so the two families
    // have to be told apart.
    bool IsWoodenFence(BlockID id);

    // MC IronBarsBlock (iron_bars and every glass pane).
    bool IsPaneBlock(BlockID id);

    // MC Block.isExceptionForConnection (Block.java:240) — the blocks that
    // look solid but that nothing agrees to connect to. Shared with WallBlock,
    // which asks the same question.
    bool IsConnectionException(BlockID id);

    // One side of the state. Asking a non-family block answers false.
    bool CrossSideOf(BlockState state, Direction dir);

    // `state.setValue(PROPERTY_BY_DIRECTION.get(dir), on)`, leaving every other
    // property — WATERLOGGED included — as it was.
    BlockState CrossStateWithSide(BlockState state, Direction dir, bool on);

    // MC FenceBlock.connectsTo / IronBarsBlock.attachsTo for the neighbour in
    // `dir`, both of which reduce to
    //   (!isExceptionForConnection(neighbour) && neighbourFaceIsSturdy)
    //   || <the family's own "same kind" test>
    bool CrossConnectsTo(const IBlockAccess& level, const glm::ivec3& pos,
                         BlockID id, Direction dir);

    // MC getStateForPlacement — resolve all four sides from the world. Applied
    // on top of whatever `fallback` already carries so the waterlog pass that
    // follows it still composes.
    BlockState CrossPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                   BlockState fallback);

    // MC updateShape: a horizontal neighbour change re-resolves THAT side only
    // (vanilla sets a single property rather than rebuilding the state).
    BlockState CrossUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                BlockState state, Direction toNeighbour);

    // MC CrossCollisionBlock.getShape — post + one box per connected side.
    BlockRegistry::BlockShapeSet CrossShapeBoxes(BlockState state);

    // MC CrossCollisionBlock.getCollisionShape. Same footprint, different
    // height: FenceBlock passes collisionHeight = 24 to the CrossCollisionBlock
    // constructor, which is what makes a fence a block and a half tall to walk
    // into and impossible to jump. IronBarsBlock passes 16, so for panes and
    // bars this equals the outline shape.
    BlockRegistry::BlockShapeSet CrossCollisionBoxes(BlockState state);

} // namespace Game
