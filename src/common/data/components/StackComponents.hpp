// File: src/common/data/components/StackComponents.hpp
//
// MC 26.3 item components (stack): custom_data, max_stack_size,
// creative_slot_lock — plus ItemStack.validateComponents / validateStrict,
// the rules the durability trio (max_damage / damage / unbreakable) and
// max_stack_size must satisfy together.
// Wire ids 210-229 (DataComponents.hpp's id table). The NBT codecs live in
// server/world/storage/anvil/components/StackNbt.cpp.
//
// The stack limit itself is read through Game::GetMaxStackSize (Item.hpp) —
// MC ItemStack.getMaxStackSize — never through Item::maxStackSize directly:
// the component on a stack's patch overrides the item's default.
#pragma once

#include "../DataComponents.hpp"
#include "../NbtCompoundValue.hpp"

#include <string>

namespace Game {

    // MC ItemStack.validateStrict: validateComponents (a damageable item —
    // MAX_DAMAGE — that stacks past 1; a CONTAINER / BUNDLE_CONTENTS /
    // CHARGED_PROJECTILES entry over its own limit), then the stack's count
    // against its limit. Empty when valid, else MC's message ("Item cannot
    // be both damageable and stackable", "Item stack with stack size of 5
    // was larger than maximum: 1").
    std::string ValidateItemStack(const ItemStack& stack);

    // MC ItemStack.getMaxStackSize for the prototype alone (no patch): the
    // prototype's MAX_STACK_SIZE, else Item::maxStackSize.
    int GetItemDefaultMaxStackSize(ItemID item);

} // namespace Game

namespace Game::DataComponents {

    // MC DataComponents.CUSTOM_DATA (id 210) — an arbitrary compound the
    // game never interprets (maps, datapacks, commands tag stacks with it).
    // Carried whole: saved, synced and compared (two stacks with different
    // custom data never merge).
    extern const DataComponentType<NbtCompoundValue> CUSTOM_DATA;

    // MC DataComponents.MAX_STACK_SIZE (id 211) — ExtraCodecs.intRange(1,
    // 99). Every item's default lives in Item::maxStackSize (MC's
    // COMMON_ITEM_COMPONENTS 64, Item.Properties.stacksTo / durability);
    // a stack's patch overrides it. Read with GetMaxStackSize.
    inline constexpr int kMaxStackSizeLimit = 99;   // Item.ABSOLUTE_MAX_STACK_SIZE
    extern const DataComponentType<int32_t> MAX_STACK_SIZE;

    // MC DataComponents.CREATIVE_SLOT_LOCK (id 212) — a Unit, network-only
    // (not persistent: no NBT codec, not settable by /give). A creative
    // picker cell holding such a stack cannot be taken (CustomCreativeSlot.
    // mayPickup) — the saved-toolbar tab's "press … to save" paper.
    extern const DataComponentType<bool> CREATIVE_SLOT_LOCK;

} // namespace Game::DataComponents
