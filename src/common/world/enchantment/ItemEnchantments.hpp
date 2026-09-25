// File: src/common/world/enchantment/ItemEnchantments.hpp
//
// Mirrors net/minecraft/world/item/enchantment/ItemEnchantments.java — the
// value type stored in the `STORED_ENCHANTMENTS` (and `ENCHANTMENTS`) data
// components.
//
// MC implements TooltipProvider here; we expose AddToTooltip(...) which mirrors
// addToTooltip (the #minecraft:tooltip_order tag first, then the rest; each as
// a coloured line via Enchantment.getFullname).
#pragma once

#include "Enchantment.hpp"
#include "EnchantmentInstance.hpp"
#include <cstddef>
#include <vector>

namespace Game {

    struct ItemEnchantments {
        // (id, level) entries. Mirrors MC's Object2IntMap<Holder<Enchantment>>;
        // using a vector since per-stack count is tiny (typically 1). Kept
        // sorted by id and free of duplicates by Set / Upgrade, so two equal
        // maps serialize to the same bytes (DataComponentMap::Equals compares
        // bytes — an insertion-ordered vector would stop two identically
        // enchanted swords from stacking or matching a trade). Write through
        // Set / Upgrade rather than pushing entries by hand.
        std::vector<EnchantmentInstance> entries;

        // Mirrors ItemEnchantments.java:31 — the singleton "no enchantments" value.
        static const ItemEnchantments EMPTY;

        // MC ItemEnchantments.getLevel: 0 when absent.
        int GetLevel(EnchantmentId id) const;

        // MC ItemEnchantments.Mutable.set: level <= 0 removes the entry,
        // otherwise it is replaced or inserted (clamped to 255, LEVEL_CODEC).
        void Set(EnchantmentId id, int level);

        // MC ItemEnchantments.Mutable.upgrade: keeps the higher of the two
        // levels; a level <= 0 is ignored.
        void Upgrade(EnchantmentId id, int level);

        bool   IsEmpty() const { return entries.empty(); }
        size_t size()    const { return entries.size(); }

        // Append one line per enchantment to `out`, formatted exactly like MC's
        // Enchantment.getFullname (RED for curses, GRAY otherwise; level suffix
        // only when level != 1 || maxLevel != 1), in MC addToTooltip's order.
        void AddToTooltip(std::vector<Enchantment::FormattedLine>& out) const;
    };

} // namespace Game
