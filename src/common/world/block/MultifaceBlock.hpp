// File: src/common/world/block/MultifaceBlock.hpp
//
// MC MultifaceBlock (net.minecraft.world.level.block.MultifaceBlock) —
// glow lichen, sculk vein, resin clump.
//
// Six independent booleans, one per face, plus `waterlogged`. It looks like
// VineBlock and is NOT the same class; the differences are load-bearing:
//
//   * DOWN is a real face here. A vine has no DOWN at all.
//   * canSurvive demands that EVERY face it wears still has something to cling
//     to, where a vine survives on any one surviving face.
//   * updateShape re-tests only the face whose neighbour changed, where a vine
//     re-derives all of them.
//   * There is no "the one above holds me up" fallback. That is a vine rule.
//
// Growth (MultifaceSpreader) is not ported — same call as vine spreading: it is
// a gameplay feature, not what makes the block work.
#pragma once

#include "BlockRegistry.hpp"
#include "BlockState.hpp"
#include "Blocks.hpp"
#include "Direction.hpp"
#include <glm/vec3.hpp>

namespace Game {

    class IBlockAccess;

    // Matched by BlockID, the way MC matches `instanceof MultifaceBlock`.
    //
    // NOT sniffed from "declares all six face booleans": chorus_plant, fire and
    // the huge mushroom blocks all carry the same six names and are completely
    // different blocks. The property set is not the identity.
    bool IsMultifaceBlock(BlockID id);

    bool MultifaceFaceOf(BlockState state, Direction face);
    BlockState MultifaceStateWithFace(BlockState state, Direction face, bool on);
    int  MultifaceCountFaces(BlockState state);
    bool MultifaceHasAnyVacantFace(BlockState state);

    // MC MultifaceBlock.canAttachTo — the neighbour's face toward us is full.
    bool MultifaceCanAttachTo(const IBlockAccess& level, const glm::ivec3& pos,
                              Direction toNeighbour);

    // MC canSurvive: at least one face, and every face it has can attach.
    bool MultifaceCanSurvive(const IBlockAccess& level, const glm::ivec3& pos,
                             BlockState state);

    // MC updateShape: drop the ONE face whose neighbour just stopped supporting
    // it; air when that was the last one.
    BlockState MultifaceUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                    BlockState state, Direction toNeighbour);

    // MC getStateForPlacement's direction search. Returns `state` unchanged when
    // no face can take it, and the survival gate then refuses the placement.
    BlockState MultifacePlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState state, Direction clickedFace);

    // MC makeShapes: a 1-pixel slab per clad face, unioned; full cube when bare.
    BlockRegistry::BlockShapeSet MultifaceShapeBoxes(BlockState state);

} // namespace Game
