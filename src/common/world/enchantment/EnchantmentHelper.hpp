// File: src/common/world/enchantment/EnchantmentHelper.hpp
//
// Mirrors net/minecraft/world/item/enchantment/EnchantmentHelper.java — utility
// functions for setting/reading enchantments on ItemStacks, plus the random
// roll the enchanting table and loot tables share (selectEnchantment /
// enchantItem). The per-enchantment data those rolls read (weight, cost
// curves, supported items, exclusive sets) comes from EnchantmentDefinitions.
//
// Reference points in MC source:
//   - setEnchantments               — line 74
//   - createBook                    — line 118
//   - enchantItem                   — line 556 / 564
//   - selectEnchantment             — line 580
//   - getAvailableEnchantmentResults — line 632
//   - filterCompatibleEnchantments  — line 611
#pragma once

#include "EnchantmentInstance.hpp"
#include "ItemEnchantments.hpp"
#include "common/entity/Item.hpp"

#include <vector>

namespace Game {

    struct ItemStack;
    class JavaRandom;

    namespace EnchantmentHelper {

        // Mirrors EnchantmentHelper.java:74 — overwrites the stack's
        // STORED_ENCHANTMENTS component (for enchanted_book) or ENCHANTMENTS
        // (for tools — future). For now only STORED_ENCHANTMENTS is wired
        // since tool enchanting isn't implemented.
        void SetEnchantments(ItemStack& stack, ItemEnchantments enchantments);

        // Mirrors EnchantmentHelper.java:118 — produce a fresh enchanted_book
        // ItemStack carrying exactly one enchantment at the given level.
        // Used by the inventory's "all enchantment book variants" search.
        ItemStack CreateBook(EnchantmentInstance inst);

        // MC ItemStack.enchant(enchantment, level): adds (or raises) one
        // enchantment on the stack. An enchanted book stores it in
        // STORED_ENCHANTMENTS; every other item has no ENCHANTMENTS component
        // in this engine yet, so the call is a no-op there (the roll still
        // consumed the same random draws MC's would).
        void Enchant(ItemStack& stack, EnchantmentId id, int level);

        // MC DataComponents.ENCHANTABLE per item (Items.java `enchantable(n)`,
        // ToolMaterial / ArmorMaterial enchantment values). 0 = not
        // enchantable, which makes selectEnchantment return nothing.
        int Enchantability(ItemID item);

        // MC EnchantmentHelper.selectEnchantment: the weighted roll at
        // `enchantmentCost` levels over `candidates` (a HolderSet — the
        // whole registry when a caller has no options).
        std::vector<EnchantmentInstance> SelectEnchantment(JavaRandom& random, const ItemStack& stack,
                                                           int enchantmentCost,
                                                           const std::vector<EnchantmentId>& candidates);

        // MC EnchantmentHelper.enchantItem: selectEnchantment, a plain book
        // becomes an enchanted book, then every result is applied.
        ItemStack EnchantItem(JavaRandom& random, ItemStack stack, int enchantmentCost,
                              const std::vector<EnchantmentId>& candidates);

    } // namespace EnchantmentHelper
} // namespace Game
