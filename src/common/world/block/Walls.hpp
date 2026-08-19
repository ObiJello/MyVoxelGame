// File: src/common/world/block/Walls.hpp
//
// Cobblestone-style walls — port of MC WallBlock.
//
// A wall is a centre post plus up to four arms, like a fence, but with two
// differences that give it its own class in vanilla and its own file here:
//
//   • each side is LOW or TALL rather than on/off, and TALL is chosen when the
//     block ABOVE covers that arm — which is what makes a wall run flush into
//     a ceiling instead of stopping a pixel short;
//   • the post itself comes and goes (UP), because a straight run of wall has
//     no post but a corner, a stub and a T-junction all do.
//
// Both decisions read the block above, so a wall reacts to a change overhead
// as well as beside it. Same rendering stake as the fence family: the wall
// blockstates are multipart and dispatch on exactly these five properties.
#pragma once

#include "Blocks.hpp"
#include "Direction.hpp"
#include "BlockRegistry.hpp"
#include "../chunk/IBlockAccess.hpp"
#include <cstdint>
#include <glm/glm.hpp>

namespace Game {

    // MC WallSide (WallSide.java). NONE is what every side defaults to.
    enum class WallSide : uint8_t { None = 0, Low = 1, Tall = 2 };

    // #minecraft:walls — every block whose model name ends in "_wall".
    bool IsWallBlock(BlockID id);

    WallSide WallSideOf(BlockState state, Direction dir);
    bool     WallUpOf(BlockState state);

    // `state.setValue(UP, up).setValue(NORTH, n)…` in one go.
    BlockState WallStateFrom(BlockState state, bool up,
                             WallSide north, WallSide east, WallSide south, WallSide west);

    // MC WallBlock.connectsTo — another wall, a sturdy face that is not a
    // connection exception, iron bars/panes, or a fence gate turned across us.
    bool WallConnectsTo(const IBlockAccess& level, const glm::ivec3& pos, Direction dir);

    // MC getStateForPlacement — resolve the four connections, then run the
    // same shared updateShape that decides LOW/TALL and UP from the block above.
    BlockState WallPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                  BlockState fallback);

    // MC updateShape: DOWN changes nothing, UP re-runs the whole
    // low/tall/post decision, and a side change re-resolves that side's
    // connection first.
    BlockState WallUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                               BlockState state, Direction toNeighbour);

    // MC WallBlock.getShape — makeShapes(16, 14): a post 16 tall and arms 14
    // tall when LOW, 16 when TALL.
    BlockRegistry::BlockShapeSet WallShapeBoxes(BlockState state);

    // MC WallBlock.getCollisionShape — makeShapes(24, 24). Like a fence, a
    // wall is a block and a half tall to walk into, and at that height LOW and
    // TALL collide identically.
    BlockRegistry::BlockShapeSet WallCollisionBoxes(BlockState state);

} // namespace Game
