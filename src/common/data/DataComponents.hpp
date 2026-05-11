// File: src/common/data/DataComponents.hpp
//
// Mirrors net/minecraft/core/component/DataComponents.java — the registry of
// every DataComponentType. Components are added here as features need them;
// the eventual MC-parity list is ~50 entries.
//
// Source line numbers cited per declaration so the C++ side can be diffed
// against MC's source.
#pragma once

#include "DataComponentType.hpp"
#include "../world/enchantment/ItemEnchantments.hpp"

namespace Game::DataComponents {

    // Explicit override for whether an item shows the enchantment "foil" (purple
    // glint) regardless of whether it has stored enchantments. Mirrors MC
    // DataComponents.java:125. Used by `enchanted_book` (Items.java:2854) so the
    // book glints even when its STORED_ENCHANTMENTS is still EMPTY.
    extern const DataComponentType<bool> ENCHANTMENT_GLINT_OVERRIDE;

    // Enchantments stored on an enchanted_book (NOT the same as ENCHANTMENTS,
    // which lives on the actual tool/armor — DataComponents.java:117). MC
    // DataComponents.java:146. Default for enchanted_book is ItemEnchantments::EMPTY
    // per Items.java:2854.
    extern const DataComponentType<ItemEnchantments> STORED_ENCHANTMENTS;

} // namespace Game::DataComponents
