// File: src/common/world/block/ShapeOcclusion.hpp
//
// The two face-coverage questions MC answers with VoxelShape booleans
// (Shapes.blockOccludes / Shapes.mergedFaceOccludes), reduced to the box
// lists this engine keeps per block state.
//
// Both ask "does the union of these boxes' faces cover a rectangle on the
// cell boundary?" and are answered exactly: the rectangle is cut along every
// box edge and each cell's centre is tested, which for at most eight boxes
// is a few dozen point tests. Shared by the fluid spread rules
// (canPassThroughWall) and the fluid mesher (per-face culling), so a fluid
// flows through exactly the gaps it is drawn through.
//
// Tangent coordinates on a face are fixed per axis: Up/Down faces are (x, z),
// East/West faces are (y, z), North/South faces are (x, y).
#pragma once

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/Direction.hpp"

namespace Game::Shapes {

    struct FaceRect {
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    };

    // The rectangle a fluid `height` tall presents on its `direction` face:
    // the full square on top and bottom, [0, height] tall on the sides.
    FaceRect FluidFaceRect(Direction direction, float height);

    // Does the face `occluder` presents at `occluderFace` cover `target`?
    // An empty box list covers nothing.
    bool FaceCovers(const BlockRegistry::BlockShapeSet& occluder, Direction occluderFace,
                    const FaceRect& target);

    // MC Shapes.mergedFaceOccludes(first, second, direction): the face
    // `first` presents toward `direction`, merged with the face `second`
    // presents back, covers the whole square between the two cells.
    // `firstFull` / `secondFull` are the callers' `== Shapes.block()` tests.
    bool MergedFaceOccludes(const BlockRegistry::BlockShapeSet& first, bool firstFull,
                            const BlockRegistry::BlockShapeSet& second, bool secondFull,
                            Direction direction);

} // namespace Game::Shapes
