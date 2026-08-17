// File: src/common/world/block/entity/DoubleChest.hpp
//
// Double-chest pairing — the C++ counterpart of MC's ChestBlock +
// DoubleBlockCombiner.
//
// MC carries a `type` blockstate property (SINGLE / LEFT / RIGHT) written at
// placement, and reads the partner straight off it:
//
//     getConnectedDirection(state):
//         type == LEFT ? facing.clockWise() : facing.counterClockWise()
//     getBlockType(state):
//         RIGHT -> FIRST, LEFT -> SECOND        (ChestBlock.java:93, 135-138)
//
// This engine has no `type` property — its chest state is `facing` only — so
// the pair is resolved GEOMETRICALLY instead, which lands on the same answer:
// two chests pair when they are horizontally adjacent along an axis
// perpendicular to their (identical) facing, and the one whose partner lies at
// its OWN counter-clockwise side is the RIGHT chest, i.e. MC's FIRST. First
// means its 27 slots occupy the top half of the 54-slot screen.
//
// Resolving it from geometry rather than storing it has a bonus: there is no
// `type` to get out of sync when one half is broken. A pair is whatever the
// world currently looks like.
#pragma once

#include "../Blocks.hpp"
#include <glm/glm.hpp>
#include <optional>

namespace Game {

    struct IBlockAccess;

    struct ChestPairing {
        glm::ivec3 partnerPos{0, 0, 0};
        // True when the chest that was ASKED about is MC's FIRST (the RIGHT
        // chest) and so contributes the top 27 slots.
        bool selfIsFirst = false;
    };

    // Find the chest paired with the one at `pos`, or nothing when it stands
    // alone. `pos` must hold a chest-family block; the partner must be the SAME
    // block id with the SAME facing, exactly as MC requires.
    // Takes IBlockAccess so BOTH sides can ask: the server against its World
    // when opening the menu, and the client against its block cache when
    // deciding which half of the joined model to draw. Both must agree, or a
    // chest would open as a double and render as two singles.
    std::optional<ChestPairing> FindChestPartner(const IBlockAccess& world,
                                                 const glm::ivec3& pos);

} // namespace Game
