// File: src/common/world/block/RedstoneShapes.hpp
//
// MC's outline / collision shapes for the redstone set, transcribed from the
// block classes rather than derived from the models. A model is not a shape:
// redstone wire's model is a flat full-cell quad while its shape is a
// connection-dependent cross, a repeater's torches stick up out of a 2-pixel
// shape, a rail's shape is 2 pixels flat and 8 on a slope, and the hopper's
// shape is hollow so items fall in. gen_block_shapes.py only carries blocks
// whose getShape ignores state; everything here reads state.
//
// Pixel boxes below are quoted as MC writes them — Block.column / Block.boxZ
// (Block.java:169-200) — and the "north" shape is turned for the other
// facings the way Shapes.rotateHorizontal does.
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"

namespace Game {

    // One-box, state-dependent shapes: the wall torches, tripwire and its
    // hook, rails, pressure plates. Returns false for any other block.
    bool RedstoneShapeFor(BlockState state, BlockRegistry::BlockShape& out);

    // Shapes that are a union of boxes: redstone wire, hopper, lectern.
    bool IsRedstoneMultiBoxBlock(BlockID id);

    // `collision` picks getCollisionShape over getShape; they differ only for
    // the lectern (the wire has no collision at all, the hopper's are equal).
    BlockRegistry::BlockShapeSet RedstoneMultiBoxShape(BlockState state, bool collision);

} // namespace Game
