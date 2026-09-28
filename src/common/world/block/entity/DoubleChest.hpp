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
#include "../BlockState.hpp"
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

    // ── The ChestBlock family (MC ChestBlock and its subclasses) ────────────
    //
    // Every block that is a pairable ChestBlock with a TYPE: the chest, the
    // trapped chest and the eight copper chests (CopperChestBlock /
    // WeatheringCopperChestBlock, #minecraft:copper_chests). The ender chest
    // is its own block (EnderChestBlock) and never pairs.
    bool IsChestBlock(BlockID id);
    bool IsCopperChestBlock(BlockID id);
    // MC ChestBlock.chestCanConnectTo: `blockState.is(this)` for the chest
    // and the trapped chest; any copper chest for a copper chest
    // (CopperChestBlock: `is(#copper_chests)`), whatever its oxidation or wax.
    bool ChestCanConnectTo(BlockID self, BlockID other);
    // WeatheringCopper.WeatherState of a copper chest — 0 unaffected, 1
    // exposed, 2 weathered, 3 oxidized — or -1 for anything else.
    int CopperChestWeatherState(BlockID id);
    bool IsWaxedCopperChest(BlockID id);
    // The copper chest of that weather state, waxed or not.
    BlockID CopperChestOf(int weatherState, bool waxed);

    // The cell a chest's TYPE connects it to (ChestBlock.getConnectedDirection:
    // LEFT → clockwise of FACING, RIGHT → counter-clockwise), or nullopt for
    // a SINGLE chest.
    std::optional<glm::ivec3> ChestConnectedCell(BlockState state, const glm::ivec3& pos);

    // MC CopperChestBlock.getLeastOxidizedChestOfConnectedBlocks: a paired
    // copper chest `state` at `pos` becomes the less oxidized of itself and
    // the copper chest it connects to (both unwaxed first when their wax
    // differs), keeping its own properties. Anything else comes back as is.
    BlockState CopperChestLeastOxidizedState(const IBlockAccess& level, const glm::ivec3& pos,
                                             BlockState state);

    // `block.withPropertiesOf(state)` between two chests: the same FACING,
    // TYPE and WATERLOGGED on another ChestBlock.
    BlockState ChestWithPropertiesOf(BlockID block, BlockState state);

} // namespace Game
