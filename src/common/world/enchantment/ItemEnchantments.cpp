// File: src/common/world/enchantment/ItemEnchantments.cpp
#include "ItemEnchantments.hpp"

namespace Game {

    const ItemEnchantments ItemEnchantments::EMPTY{};

    void ItemEnchantments::AddToTooltip(std::vector<Enchantment::FormattedLine>& out) const {
        for (const auto& [id, level] : entries) {
            const auto& e = EnchantmentRegistry::Get(id);
            out.push_back(Enchantment::GetFullname(e, level));
        }
    }

} // namespace Game
