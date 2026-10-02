// File: src/common/world/portal/EndPortalFrame.hpp
//
// Port of the parts of net.minecraft.world.level.block.EndPortalFrameBlock
// that are logic rather than rendering: the completed-frame pattern and the
// two state accessors every caller needs.
//
// The engine has no per-block C++ class the way MC does — behaviour hangs off
// function pointers in BlockRegistry — so the pattern lives here as a free
// function and BlockBehaviors.cpp wires the interaction.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/pattern/BlockPattern.hpp"

namespace Game {

    namespace EndPortalFrame {

        // EndPortalFrameBlock.java:89 — HAS_EYE is BlockStateProperties.EYE.
        bool HasEye(BlockState state);

        // EndPortalFrameBlock.java:88 — FACING is HORIZONTAL_FACING. Frames
        // face INWARD in a completed ring (a frame on the north side faces
        // south), which is what makes the pattern's four distinct characters
        // necessary. Returns North for a state that has no facing.
        Direction Facing(BlockState state);

        // The state this block becomes when an eye is placed / the eye is
        // removed, preserving facing.
        BlockState WithEye(BlockState state, bool hasEye);

        // EndPortalFrameBlock.java:75 — getOrCreatePortalShape(). Built once
        // on first use, as in vanilla, because the predicates capture nothing
        // and the object is immutable.
        //
        // A 5x5 pattern with a `?` (any block) at each corner and a
        // DIRECTION-SPECIFIC predicate on each of the twelve frame cells,
        // found by the generic matcher (common/world/block/pattern). That is
        // load-bearing: EnderEyeItem.useOn fills the portal at
        // `match.getFrontTopLeft().offset(-3, 0, -3)` with raw world offsets,
        // which is only correct because the search order guarantees the match
        // comes back with forwards=DOWN and up=SOUTH — the one orientation
        // where the returned corner is the ring's south-east cell.
        const BlockPattern& PortalShapePattern();

    } // namespace EndPortalFrame

} // namespace Game
