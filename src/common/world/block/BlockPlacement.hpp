// File: src/common/world/block/BlockPlacement.hpp
//
// Port of MC's Block.getStateForPlacement dispatch. In vanilla each Block
// subclass overrides getStateForPlacement; we have no per-block classes, so the
// same rules live in one table keyed by BlockID and are applied uniformly.
//
// This is deliberately shared (`common`) rather than server-only: the client
// predicts placement locally so the block appears on the same frame, and if the
// two sides computed facing differently the block would visibly snap when the
// server's authoritative update arrived.
#pragma once

#include "Blocks.hpp"
#include "Direction.hpp"
#include "BlockInteraction.hpp"

namespace Game {

    // The placement-orientation rules MC actually uses, named after what they
    // do rather than after any one block. The `Opposite` vs raw distinction is
    // the whole bug surface here: a container's front should look BACK at the
    // player, while a structural block's facing points AWAY from them.
    enum class PlacementRule : uint8_t {
        None,                  // block has no orientation
        HorizontalOpposite,    // FACING = horizontalDirection.getOpposite()
        Horizontal,            // FACING = horizontalDirection            (raw)
        HorizontalClockwise,   // FACING = horizontalDirection.getClockWise()
        NearestOpposite,       // FACING = nearestLookingDirection.getOpposite()
        ClickedFace,           // FACING = clickedFace
        ClickedFaceOpposite,   // FACING = clickedFace.getOpposite()
        ClickedFaceAxis,       // AXIS   = clickedFace.getAxis()
    };

    // Which rule a block uses. Decided from the MC model name, matching how
    // BlockRegistry already classifies mining traits and collision.
    PlacementRule GetPlacementRule(BlockID id);

    // The block-state index a freshly placed block should carry, given how the
    // player was standing and what they clicked. Returns 0 (the block's default
    // state) for blocks with no orientation, so callers can apply it
    // unconditionally.
    uint8_t ComputePlacementState(BlockID id, const UseOnContext& context);

    // ── Segmented ground cover (MC SegmentableBlock) ─────────────────────────
    //
    // Leaf litter, wildflowers and pink petals stack in place: right-clicking an
    // existing clump with the same item raises `segment_amount` instead of
    // placing a second block, up to 4
    // (SegmentableBlock.canBeReplaced + getStateForPlacement).
    //
    // Vanilla expresses that as one block with a property; this engine spends a
    // BlockID per segment count, so "raise the count" is a BlockID step. The
    // facing must be carried over unchanged — MC's `state.setValue(segment,
    // n + 1)` mutates the existing state, so a clump never re-orients when you
    // add to it.

    // The family's 1-segment BlockID, or Air when `id` isn't segmented. Two
    // blocks belong to the same family iff this returns the same non-Air value.
    BlockID SegmentedFamilyBase(BlockID id);

    // The BlockID with one more segment, or Air when `id` isn't segmented or is
    // already at 4 (where MC's canBeReplaced returns false and placement falls
    // through to the neighbouring cell).
    BlockID SegmentedGrowth(BlockID id);

    // ── MC BlockPlaceContext / BlockItem gates ──────────────────────────────
    //
    // Placement in vanilla is three questions asked in a fixed order, and the
    // order is what makes segmented blocks behave:
    //
    //   1. replaceClicked = clickedState.canBeReplaced(ctx)
    //   2. placementPos   = replaceClicked ? clickedPos : clickedPos.relative(face)
    //   3. canPlace()     = replaceClicked || stateAt(placementPos).canBeReplaced(ctx)
    //
    // and only THEN does getStateForPlacement read the block at placementPos.
    // Clicking the grass block under a leaf litter clump therefore still grows
    // the clump: the grass isn't replaceable, so the position resolves UP into
    // the litter's own cell, and the growth check runs there.

    // MC BlockBehaviour.canBeReplaced(state, BlockPlaceContext) plus
    // SegmentableBlock's override. `held` is the block form of the item in
    // hand (Air if none), `secondaryUse` is MC's isSecondaryUseActive (sneak).
    //
    // Note the base rule's second clause — `!context.getItemInHand().is(asItem())`.
    // A replaceable block is NOT replaceable by MORE OF ITSELF, which is what
    // stops a full 4-segment clump from being overwritten and pushes placement
    // into the cell above.
    bool CanBeReplacedByPlacement(BlockID existing, uint8_t existingState,
                                  BlockID held, bool secondaryUse);

    // MC BlockBehaviour.canSurvive, for the families this engine models.
    // `belowId`/`belowState` are the block underneath the placement position.
    // Blocks with no modelled rule return true, matching the previous
    // unconditional behaviour.
    bool CanSurviveOn(BlockID id, BlockID belowId, uint8_t belowState);

} // namespace Game
