// File: src/common/world/enchantment/ItemEnchantments.hpp
//
// Mirrors net/minecraft/world/item/enchantment/ItemEnchantments.java — the
// value type stored in the `STORED_ENCHANTMENTS` (and `ENCHANTMENTS`) data
// components.
//
// MC implements TooltipProvider here; we expose AddToTooltip(...) which mirrors
// addToTooltip lines 56–77 (iterate enchantments, format each as a coloured
// line via Enchantment.getFullname).
#pragma once

#include "Enchantment.hpp"
#include "EnchantmentInstance.hpp"
#include <vector>

namespace Game {

    struct ItemEnchantments {
        // (id, level) entries. Mirrors MC's Object2IntMap<Holder<Enchantment>>;
        // using a vector since per-stack count is tiny (typically 1).
        std::vector<EnchantmentInstance> entries;

        // Mirrors ItemEnchantments.java:31 — the singleton "no enchantments" value.
        static const ItemEnchantments EMPTY;

        // Append one line per enchantment to `out`, formatted exactly like MC's
        // Enchantment.getFullname (RED for curses, GRAY otherwise; level suffix
        // only when level != 1 || maxLevel != 1). Mirrors addToTooltip 56–77.
        void AddToTooltip(std::vector<Enchantment::FormattedLine>& out) const;
    };

} // namespace Game
