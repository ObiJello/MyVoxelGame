// File: src/common/world/enchantment/EnchantmentHelper.cpp
#include "EnchantmentHelper.hpp"

#include "../../entity/Item.hpp"
#include "../../entity/GeneratedItemList.hpp" // Game::Items::EnchantedBook
#include "../../data/DataComponents.hpp"

namespace Game::EnchantmentHelper {

    void SetEnchantments(ItemStack& stack, ItemEnchantments enchantments) {
        // For enchanted_book, MC stores enchantments in STORED_ENCHANTMENTS
        // (DataComponents.java:146) rather than ENCHANTMENTS. See
        // EnchantmentHelper.java:74. Only enchanted_book is wired this PR.
        stack.components.set(DataComponents::STORED_ENCHANTMENTS, std::move(enchantments));
    }

    ItemStack CreateBook(EnchantmentInstance inst) {
        // Mirrors EnchantmentHelper.java:118.
        ItemStack stack(Items::EnchantedBook, 1);
        SetEnchantments(stack, ItemEnchantments{{{inst.id, inst.level}}});
        return stack;
    }

} // namespace Game::EnchantmentHelper
