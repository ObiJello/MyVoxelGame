// File: src/common/data/components/ToolComponents.hpp
//
// MC 26.3 item components (tools & equipment): glider — plus the behaviour
// helpers of tool, equippable and damage_resistant (whose structs live in
// DataComponents.hpp with the rest of the original component set).
// Wire ids 250-269 (DataComponents.hpp's id table). The NBT codecs live in
// server/world/storage/anvil/components/ToolNbt.cpp.
#pragma once

#include "../DataComponents.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace Game {

    struct Block;
    class Inventory;

    // ── tool ────────────────────────────────────────────────────────────

    // MC `state.is(HolderSet<Block>)` for one rule's raw entries: a
    // "#namespace:tag" entry asks the block's tags (DataTags), a plain id is
    // the block itself. Blocks the vanilla tag files do not list (the
    // engine's own and the mod ports', which take their tool data from the
    // vanilla block they are modelled on through the hardness generator)
    // still answer the tool tags from that data: #mineable/<tool> from
    // Block::preferredTool, #needs_<tier>_tool and #incorrect_for_<tier>_tool
    // from Block::minTier — and #leaves / #wool from their registry slug.
    bool BlockMatchesHolderSet(const Block& block, const std::vector<std::string>& entries);
    bool BlockInTag(const Block& block, std::string_view tag);

    // The block tag a tier's tools may not harvest (ToolMaterial.
    // incorrectBlocksForDrops): #minecraft:incorrect_for_<material>_tool;
    // the Hush's resonite tier answers #obeycraft:incorrect_for_resonite_tool
    // (no vanilla block — it alone opens the echo core).
    std::string IncorrectForDropsTag(MiningTier tier);
    // #minecraft:mineable/<tool> for a digging tool type ("" otherwise).
    std::string MineableTag(ToolType type);

    // ToolMaterial.applyToolProperties' TOOL: deniesDrops(incorrect tag),
    // minesAndDrops(mineable tag, material speed); default speed 1, one
    // point of wear per block, can destroy in creative.
    Tool MakeDiggerTool(std::string incorrectTag, std::string mineableTag, float speed);
    // Shorthand over the engine's tier / tool-type classification.
    Tool MakeDiggerTool(ToolType type, MiningTier tier, float speed);
    // ToolMaterial.applySwordProperties' TOOL: cobweb 15 (and drops),
    // #sword_instantly_mines at Float.MAX_VALUE, #sword_efficient 1.5; two
    // points of wear per block, cannot destroy in creative.
    Tool MakeSwordTool();
    // ShearsItem.createToolProperties: cobweb 15 (and drops),
    // #shears_extreme_breaking_speed 15, _major 5, _minor 2.
    Tool MakeShearsTool();

    // MC Item.canDestroyBlock (the base behaviour): a TOOL that cannot
    // destroy blocks in creative stops an instabuilding player's break.
    bool CanDestroyBlockWith(const ItemStack& held, bool instabuild);

    // ── glider ──────────────────────────────────────────────────────────

    // MC LivingEntity.canGlideUsing(stack, slot): GLIDER, worn in its
    // EQUIPPABLE slot, and not one point from breaking.
    bool CanGlideUsing(const ItemStack& stack, EquipmentSlot slot);
    // The player's slots (EquipmentSlot.VALUES order) holding a usable
    // glider — canGlide asks "any", updateFallFlying wears a random one.
    std::vector<EquipmentSlot> InventoryGliderSlots(const Inventory& inventory);

    // ── equippable ──────────────────────────────────────────────────────

    // The stack's equipment asset path ("iron", "leather", "elytra") — its
    // EQUIPPABLE asset_id without the "minecraft:" namespace, or "" (no
    // component, no asset: nothing is drawn, HumanoidArmorLayer.
    // shouldRender). A namespaced asset outside minecraft keeps its
    // namespace ("aether:zanite").
    std::string EquipmentAssetPath(const ItemStack& stack);
    // Whether the equipment asset has a WINGS layer — in vanilla's
    // equipment definitions only minecraft:elytra does.
    bool EquipmentAssetHasWings(std::string_view assetPath);
    // The texture an Equippable camera_overlay identifier names
    // (Identifier.withPath("textures/" + p + ".png")): an engine asset path
    // ("assets/textures/misc/pumpkinblur.png"), namespaces other than
    // minecraft under assets/<namespace>/textures/.
    std::string CameraOverlayTexturePath(std::string_view overlayId);

    // MC LivingEntity.isEquippableInSlot for a player (ArmorSlot.mayPlace):
    // the stack's EQUIPPABLE names exactly `slot` and admits the player.
    bool IsEquippableInPlayerSlot(const ItemStack& stack, EquipmentSlot slot);

    // ── damage_resistant ────────────────────────────────────────────────

    // MC ItemStack.canBeHurtBy(source): no DAMAGE_RESISTANT, or the damage
    // type (its id, "minecraft:lava") is not in the resisted tag.
    bool ItemStackCanBeHurtBy(const ItemStack& stack, std::string_view damageTypeId);

} // namespace Game

namespace Game::DataComponents {

    // MC DataComponents.GLIDER (Unit, id 250): the stack lets its wearer
    // glide (elytra flight) while worn in its EQUIPPABLE slot. The bool is
    // always true — presence is the value.
    extern const DataComponentType<bool> GLIDER;

} // namespace Game::DataComponents
