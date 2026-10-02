// File: src/common/world/block/CopperChestBlock.hpp
//
// Mirrors net.minecraft.world.level.block.CopperChestBlock and
// WeatheringCopperChestBlock (26.3). The copper chests are ChestBlocks: the
// CHEST block entity and menu, single or double (DoubleChest.hpp's family
// helpers), the chest renderer with the copper sheets, and the copper hinge
// sounds (ChestBlockEntity::PlayLidSound). What is theirs alone lives here:
//
//   * updateShape — a paired copper chest takes its partner's block, so both
//     halves always show one oxidation and one wax;
//   * the random tick of the unwaxed four — WeatheringCopper.changeOverTime,
//     skipped for the RIGHT half (the LEFT one ages the pair) and while
//     anyone has the chest open. The block entity, and the items in it, carry
//     across (World::SetBlock's shouldChangedStateKeepBlockEntity).
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    // WeatheringCopper.getAge of a weathering (unwaxed) copper block — 0
    // unaffected, 1 exposed, 2 weathered, 3 oxidized — or -1 for anything
    // that does not weather. Defined in ItemBehaviors.cpp beside the copper
    // family table the axe and honeycomb use.
    int WeatheringCopperAge(BlockID id);
    // WeatheringCopper.getNext: the next oxidation stage, or Air at the last.
    BlockID WeatheringCopperNext(BlockID id);
    // WeatheringCopper.getPrevious(state) — one stage back with the shared
    // properties kept, or `state` itself when there is none.
    BlockState WeatheringCopperPreviousState(BlockState state);
    // WeatheringCopper.getFirst(state) — the family's unaffected stage.
    BlockState WeatheringCopperFirstState(BlockState state);
    // HoneycombItem.WAX_OFF_BY_BLOCK has the block: a waxed copper stage.
    bool IsWaxedCopperBlock(BlockID id);

    // Wires the copper chests' hooks, and ChestBlock.updateShape (the pair's
    // TYPE kept in step, DoubleChest.hpp ChestUpdateShape) on the chest and
    // the trapped chest. Called from BlockRegistry_RegisterBehaviors.
    void RegisterCopperChestBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
