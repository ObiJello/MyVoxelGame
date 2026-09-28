// File: src/common/world/level/VisualClip.hpp
//
// MC Level.clip(new ClipContext(from, to, ClipContext.Block.VISUAL,
// ClipContext.Fluid.NONE, CollisionContext.empty())) — the line-of-sight
// clip the trial spawner and the vault's player detection use, and the one
// that places an ominous item spawner under a ceiling.
//
// VISUAL is BlockBehaviour.getVisualShape, which is the collision shape
// except where a block overrides it:
//   empty      TransparentBlock (glass, stained glass, tinted glass, the
//              copper grates — WaterloggedTransparentBlock), IronBarsBlock
//              (iron bars, glass panes, stained glass panes, copper bars)
//              and powder snow: sight passes straight through them;
//   full cube  mud and soul sand (whose collision box is shorter);
//   outline    fences (not the 1.5-block collision post) and snow layers.
//
// The walk is BlockGetter.traverseBlocks (BlockClip.hpp), each visited cell
// clipped against its shape boxes as VoxelShape.clip does.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

namespace Game {

    struct IBlockAccess;

    // MC BlockHitResult, reduced: whether a block was hit, which cell, and
    // where. On a miss `blockPos` is BlockPos.containing(to) and `location`
    // is `to` — exactly BlockHitResult.miss.
    struct VisualClipResult {
        bool       hit = false;
        glm::ivec3 blockPos{0};
        glm::dvec3 location{0.0};
    };

    // MC BlockBehaviour.getVisualShape emptiness — true for the blocks sight
    // passes through (the list above).
    bool HasEmptyVisualShape(BlockState state);

    VisualClipResult ClipVisual(const IBlockAccess& blocks, const glm::dvec3& from, const glm::dvec3& to);

} // namespace Game
