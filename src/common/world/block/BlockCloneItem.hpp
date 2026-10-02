// File: src/common/world/block/BlockCloneItem.hpp
//
// What pick-block gives for a block — MC Block.asItem (Item.BY_BLOCK) and
// BlockState.getCloneItemStack with every vanilla override, in one place.
//
// This engine derives a block item's ItemID from its BlockID
// (ItemRegistry::FromBlock), so every block — redstone_wire, wall_torch,
// wheat, water — has an "item" of its own id. MC does not: BY_BLOCK maps a
// block to the BlockItem registered FOR it, whose name may differ
// (redstone_wire -> redstone, wheat -> wheat_seeds, wall_torch -> torch,
// water_cauldron -> cauldron), and a block nobody registered an item for
// answers AIR (water, fire, portals, piston heads). BlockAsItem is that map;
// GetCloneItemStack adds the per-block getCloneItemStack overrides on top.
#pragma once

#include "common/entity/Item.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/vec3.hpp>

namespace Game {

    class ILevelWrite;

    // MC Block.asItem: the item registered for `block` (BlockItem
    // .registerBlocks — a renamed BlockItem, a StandingAndWallBlockItem's
    // wall twin, the cauldron's filled forms), Items::Air when MC registers
    // none. Built once from the block and item registries.
    ItemID BlockAsItem(BlockID block);

    // MC BlockState.getCloneItemStack(level, pos, includeData = false): the
    // stack pick-block conjures for the block at `pos` — empty when the
    // block gives nothing. Block-entity driven overrides (banner patterns,
    // pot decorations, the copper golem statue's pose) read `level`.
    ItemStack GetCloneItemStack(ILevelWrite& level, const glm::ivec3& pos, BlockState state);

} // namespace Game
