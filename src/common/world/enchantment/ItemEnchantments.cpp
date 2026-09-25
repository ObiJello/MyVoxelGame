// File: src/common/world/enchantment/ItemEnchantments.cpp
#include "ItemEnchantments.hpp"
#include "EnchantmentDefinitions.hpp"

#include <algorithm>

namespace Game {

    const ItemEnchantments ItemEnchantments::EMPTY{};

    namespace {
        auto FindSlot(std::vector<EnchantmentInstance>& entries, EnchantmentId id) {
            return std::lower_bound(entries.begin(), entries.end(), id,
                                    [](const EnchantmentInstance& e, EnchantmentId v) { return e.id < v; });
        }
    }

    int ItemEnchantments::GetLevel(EnchantmentId id) const {
        for (const EnchantmentInstance& e : entries) {
            if (e.id == id) return e.level;
        }
        return 0;
    }

    void ItemEnchantments::Set(EnchantmentId id, int level) {
        auto it = FindSlot(entries, id);
        const bool present = it != entries.end() && it->id == id;
        if (level <= 0) {
            if (present) entries.erase(it);
            return;
        }
        level = std::min(level, 255);
        if (present) it->level = level;
        else         entries.insert(it, EnchantmentInstance{id, level});
    }

    void ItemEnchantments::Upgrade(EnchantmentId id, int level) {
        if (level <= 0) return;
        Set(id, std::max(GetLevel(id), std::min(level, 255)));
    }

    void ItemEnchantments::AddToTooltip(std::vector<Enchantment::FormattedLine>& out) const {
        // MC addToTooltip: every enchantment of #minecraft:tooltip_order the
        // map holds, in the tag's order; then whatever the tag does not name.
        const std::vector<EnchantmentId>& order = EnchantmentDefinitions::TooltipOrder();
        for (const EnchantmentId id : order) {
            const int level = GetLevel(id);
            if (level > 0) out.push_back(Enchantment::GetFullname(EnchantmentRegistry::Get(id), level));
        }
        for (const auto& [id, level] : entries) {
            if (std::find(order.begin(), order.end(), id) != order.end()) continue;
            out.push_back(Enchantment::GetFullname(EnchantmentRegistry::Get(id), level));
        }
    }

} // namespace Game
