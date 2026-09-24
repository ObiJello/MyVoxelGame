// File: src/common/world/enchantment/EnchantmentHelper.cpp
#include "EnchantmentHelper.hpp"
#include "EnchantmentDefinitions.hpp"

#include "../../entity/Item.hpp"
#include "../../entity/GeneratedItemList.hpp" // Game::Items::EnchantedBook
#include "../../data/DataComponents.hpp"
#include "common/core/JavaRandom.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

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

    void Enchant(ItemStack& stack, EnchantmentId id, int level) {
        if (stack.itemId != Items::EnchantedBook) return;   // no ENCHANTMENTS component yet
        ItemEnchantments current = stack.components.get(DataComponents::STORED_ENCHANTMENTS)
                                       .value_or(ItemEnchantments{});
        // ItemEnchantments.Mutable.upgrade: keep the higher level.
        bool found = false;
        for (EnchantmentInstance& e : current.entries) {
            if (e.id == id) { e.level = std::max(e.level, level); found = true; break; }
        }
        if (!found) current.entries.push_back({id, level});
        SetEnchantments(stack, std::move(current));
    }

    int Enchantability(ItemID item) {
        const std::string_view slug = ItemRegistry::Slug(item);
        if (slug.empty()) return 0;
        auto endsWith = [&](std::string_view suffix) {
            return slug.size() >= suffix.size()
                && slug.compare(slug.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        auto startsWith = [&](std::string_view prefix) { return slug.rfind(prefix, 0) == 0; };

        // Items.java literals.
        if (slug == "book" || slug == "bow" || slug == "crossbow" || slug == "trident"
            || slug == "fishing_rod") return 1;
        if (slug == "mace" || slug == "elytra") return 15;

        // ToolMaterial enchantment values (ToolMaterial.java:85-91).
        const bool tool = endsWith("_sword") || endsWith("_pickaxe") || endsWith("_axe")
                       || endsWith("_shovel") || endsWith("_hoe");
        if (tool) {
            if (startsWith("wooden_"))    return 15;
            if (startsWith("stone_"))     return 5;
            if (startsWith("copper_"))    return 13;
            if (startsWith("iron_"))      return 14;
            if (startsWith("diamond_"))   return 10;
            if (startsWith("golden_"))    return 22;
            if (startsWith("netherite_")) return 15;
            return 0;
        }
        // ArmorMaterials.java enchantment values.
        const bool armor = endsWith("_helmet") || endsWith("_chestplate") || endsWith("_leggings")
                        || endsWith("_boots");
        if (armor) {
            if (startsWith("leather_"))   return 15;
            if (startsWith("copper_"))    return 8;
            if (startsWith("chainmail_")) return 12;
            if (startsWith("iron_"))      return 9;
            if (startsWith("golden_"))    return 25;
            if (startsWith("diamond_"))   return 10;
            if (startsWith("turtle_"))    return 9;
            if (startsWith("netherite_")) return 15;
            return 0;
        }
        if (slug == "wolf_armor") return 10;
        return 0;
    }

    namespace {

        struct WeightedInstance {
            EnchantmentInstance inst;
            int weight;
        };

        // MC EnchantmentHelper.getAvailableEnchantmentResults.
        std::vector<WeightedInstance> AvailableResults(int value, const ItemStack& stack,
                                                       const std::vector<EnchantmentId>& source) {
            std::vector<WeightedInstance> results;
            const bool isBook = stack.itemId == Items::Book;
            for (const EnchantmentId id : source) {
                if (!(EnchantmentDefinitions::IsPrimaryItem(id, stack.itemId) || isBook)) continue;
                const auto& d = EnchantmentDefinitions::Get(id);
                if (!d.loaded) continue;
                for (int level = d.maxLevel; level >= 1; --level) {
                    if (value >= d.minCost.Calculate(level) && value <= d.maxCost.Calculate(level)) {
                        results.push_back({{id, level}, d.weight});
                        break;
                    }
                }
            }
            return results;
        }

        // MC WeightedRandom.getRandomItem(random, items, weightGetter).
        bool PickWeighted(JavaRandom& random, const std::vector<WeightedInstance>& items,
                          EnchantmentInstance& out) {
            int total = 0;
            for (const auto& w : items) total += w.weight;
            if (total <= 0) return false;
            int i = random.NextInt(total);
            for (const auto& w : items) {
                i -= w.weight;
                if (i < 0) { out = w.inst; return true; }
            }
            return false;
        }

    } // namespace

    std::vector<EnchantmentInstance> SelectEnchantment(JavaRandom& random, const ItemStack& stack,
                                                       int enchantmentCost,
                                                       const std::vector<EnchantmentId>& candidates) {
        std::vector<EnchantmentInstance> results;
        const int enchantable = Enchantability(stack.itemId);
        if (enchantable <= 0) return results;              // MC: no ENCHANTABLE component

        enchantmentCost += 1 + random.NextInt(enchantable / 4 + 1) + random.NextInt(enchantable / 4 + 1);
        const float randomSpan = (random.NextFloat() + random.NextFloat() - 1.0f) * 0.15f;
        enchantmentCost = std::max(1, static_cast<int>(std::lround(
            static_cast<float>(enchantmentCost) + static_cast<float>(enchantmentCost) * randomSpan)));

        std::vector<WeightedInstance> available = AvailableResults(enchantmentCost, stack, candidates);
        if (available.empty()) return results;

        EnchantmentInstance picked{};
        if (PickWeighted(random, available, picked)) results.push_back(picked);

        while (random.NextInt(50) <= enchantmentCost) {
            if (!results.empty()) {
                // filterCompatibleEnchantments against the last pick.
                const EnchantmentId last = results.back().id;
                available.erase(std::remove_if(available.begin(), available.end(),
                                               [&](const WeightedInstance& w) {
                                                   return !EnchantmentDefinitions::AreCompatible(last, w.inst.id);
                                               }),
                                available.end());
            }
            if (available.empty()) break;
            if (PickWeighted(random, available, picked)) results.push_back(picked);
            enchantmentCost /= 2;
        }
        return results;
    }

    ItemStack EnchantItem(JavaRandom& random, ItemStack stack, int enchantmentCost,
                          const std::vector<EnchantmentId>& candidates) {
        const std::vector<EnchantmentInstance> enchants =
            SelectEnchantment(random, stack, enchantmentCost, candidates);
        if (stack.itemId == Items::Book) {
            stack = ItemStack(Items::EnchantedBook, stack.count > 0 ? stack.count : 1);
        }
        for (const EnchantmentInstance& e : enchants) Enchant(stack, e.id, e.level);
        return stack;
    }

} // namespace Game::EnchantmentHelper
