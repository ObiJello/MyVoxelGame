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
#include "../chunk/IBlockAccess.hpp"

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

    // World-aware canSurvive. Same question as CanSurviveOn, for the blocks
    // whose MC rule cannot be answered from the block below alone:
    //
    //   sugar cane — dirt or sand with WATER beside the block underneath
    //   cactus     — sand or cactus, and no solid block on any horizontal side
    //   bamboo     — the #bamboo_plantable_on tag
    //
    // Everything else delegates to CanSurviveOn, so this is the form every
    // placement path should call. Both the server's placement gate and the
    // client's prediction use it, which is what keeps a rejected planting from
    // flickering: the two sides ask the same function the same question.
    bool CanSurviveAt(const IBlockAccess& level, const glm::ivec3& pos, BlockID id);

    // State-aware form. Some blocks cannot answer "can I survive here" without
    // knowing their own state: a button attached to a WALL is held up by a
    // different neighbour than one on the FLOOR, and only its `face`/`facing`
    // says which. MC has the same split — canSurvive reads
    // getConnectedDirection(state).
    //
    // Must be called AFTER the placement state is computed, which is why the
    // placement paths check twice: the cheap state-free gate first, then this
    // once they know what they are about to write.
    bool CanSurviveAt(const IBlockAccess& level, const glm::ivec3& pos, BlockID id,
                      uint8_t stateIndex);

    // True when CanSurviveAt/CanSurviveOn reproduce this block's MC canSurvive
    // rule, rather than falling through to "no modelled rule, allow anything".
    //
    // World::CanBlockSurviveAt needs the distinction: where a real rule exists
    // it must be used, and where one doesn't it keeps its "the block below has
    // a solid top face" heuristic. Asking CanSurviveAt unconditionally would
    // make every unmodelled block indestructible-by-support (it answers true),
    // and using the heuristic unconditionally deletes blocks that legitimately
    // stack on themselves — sugar cane on sugar cane, which is noCollision and
    // so fails the heuristic from the second segment up.
    bool HasModelledSurvivalRule(BlockID id);

    // Placement state for blocks whose orientation depends on the WORLD rather
    // than on how the player was standing — today just redstone dust, which
    // resolves its four connections against its neighbours.
    //
    // Separate from ComputePlacementState because that one takes a
    // UseOnContext whose `world` the client predictor deliberately leaves null
    // ("the placement rules read no world state"). This takes the block access
    // both sides already have, so server and client compute the same answer
    // and a placed wire does not visibly snap when the ack lands.
    //
    // Returns `fallback` unchanged for blocks with no world-aware rule.
    uint8_t ComputeWorldPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockID id, uint8_t fallback);

} // namespace Game
