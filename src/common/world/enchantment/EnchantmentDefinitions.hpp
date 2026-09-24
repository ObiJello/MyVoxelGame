// File: src/common/world/enchantment/EnchantmentDefinitions.hpp
//
// The data-driven half of MC's Enchantment record — what
// data/minecraft/enchantment/<slug>.json carries and the enchanting rolls
// read: weight, max_level, min_cost / max_cost curves, supported_items,
// primary_items and exclusive_set. EnchantmentRegistry (Enchantment.hpp)
// keeps the baked identity (slug, display name, level range); this loads the
// rest at runtime from the shipped data pack, the same way DataTags reads
// block/item tags, so the loot and enchanting-table rolls can converge on
// EnchantmentHelper.selectEnchantment instead of picking uniformly.
//
// Item membership goes through DataTags (Registry::Item): `supported_items`
// is normally "#minecraft:enchantable/<group>", and the item's own tag list
// answers "is this item in that tag" without a second tag index.
#pragma once

#include "Enchantment.hpp"
#include "common/entity/Item.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace Game::EnchantmentDefinitions {

    // MC Enchantment.Cost: base + per_level_above_first * (level - 1).
    struct Cost {
        int base = 1;
        int perLevelAboveFirst = 0;
        int Calculate(int level) const { return base + perLevelAboveFirst * (level - 1); }
    };

    struct Definition {
        bool loaded = false;                 // false: no JSON — the enchantment never rolls
        int  weight = 0;                     // MC Enchantment.getWeight
        int  maxLevel = 1;                   // MC Enchantment.getMaxLevel
        Cost minCost;                        // MC Enchantment.getMinCost(level)
        Cost maxCost;                        // MC Enchantment.getMaxCost(level)
        // HolderSets, kept as the raw entries ("#minecraft:enchantable/sword"
        // or "minecraft:stick"); membership is decided per item at query time.
        std::vector<std::string> supportedItems;
        std::vector<std::string> primaryItems;   // empty = "same as supported"
        std::vector<EnchantmentId> exclusiveSet;
    };

    const Definition& Get(EnchantmentId id);

    // MC Enchantment.isSupportedItem / isPrimaryItem / canEnchant.
    bool IsSupportedItem(EnchantmentId id, ItemID item);
    bool IsPrimaryItem(EnchantmentId id, ItemID item);

    // MC Enchantment.areCompatible: different, and neither lists the other
    // in its exclusive_set.
    bool AreCompatible(EnchantmentId a, EnchantmentId b);

    // A HolderSet<Enchantment> as loot JSON writes it: "#minecraft:tag",
    // "minecraft:id", or an array of either. Tags come from
    // data/<ns>/tags/enchantment/**.json (nested "#tag" entries followed).
    // Unknown ids are dropped; a missing tag resolves to nothing.
    std::vector<EnchantmentId> ResolveSet(const std::vector<std::string>& entries);
    std::vector<EnchantmentId> ResolveTag(std::string_view tag);

    // Every registered enchantment that has a definition (MC's "the whole
    // registry" fallback when a function names no options).
    const std::vector<EnchantmentId>& All();

    void Reload();

} // namespace Game::EnchantmentDefinitions
