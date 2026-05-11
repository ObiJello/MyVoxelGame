// File: src/common/world/enchantment/EnchantmentHelper.hpp
//
// Mirrors net/minecraft/world/item/enchantment/EnchantmentHelper.java — utility
// functions for setting/reading enchantments on ItemStacks. We implement the
// subset needed for this PR; the full class has many more methods (combine,
// random rolls, applies-to checks, etc.) that arrive with future enchanting
// gameplay.
//
// Reference points in MC source:
//   - setEnchantments  — line 74
//   - createBook       — line 118
#pragma once

#include "EnchantmentInstance.hpp"
#include "ItemEnchantments.hpp"

namespace Game {

    struct ItemStack;

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

    } // namespace EnchantmentHelper
} // namespace Game
