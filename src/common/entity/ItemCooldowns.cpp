// File: src/common/entity/ItemCooldowns.cpp
#include "common/entity/ItemCooldowns.hpp"
#include "common/data/DataComponents.hpp"

#include "common/entity/Item.hpp"

#include <algorithm>
#include <vector>

namespace Game {

    float ItemCooldowns::GetCooldownPercent(const ItemStack& item, float partialTick) const {
        if (item.IsEmpty() || m_cooldowns.empty()) return 0.0f;
        return GetCooldownPercent(GetCooldownGroup(item), partialTick);
    }

    float ItemCooldowns::GetCooldownPercent(const std::string& group, float partialTick) const {
        const auto it = m_cooldowns.find(group);
        if (it == m_cooldowns.end()) return 0.0f;
        const float duration  = static_cast<float>(it->second.endTime - it->second.startTime);
        const float remaining = static_cast<float>(it->second.endTime) -
                                (static_cast<float>(m_tickCount) + partialTick);
        if (duration <= 0.0f) return 0.0f;
        return std::clamp(remaining / duration, 0.0f, 1.0f);
    }

    void ItemCooldowns::Tick() {
        ++m_tickCount;
        if (m_cooldowns.empty()) return;
        // Collected first: onCooldownEnded may reach back into the table.
        std::vector<std::string> ended;
        for (auto it = m_cooldowns.begin(); it != m_cooldowns.end();) {
            if (it->second.endTime <= m_tickCount) {
                ended.push_back(it->first);
                it = m_cooldowns.erase(it);
            } else {
                ++it;
            }
        }
        for (const std::string& group : ended) OnCooldownEnded(group);
    }

    std::string ItemCooldowns::GetCooldownGroup(ItemID item) {
        // BuiltInRegistries.ITEM.getKey(item.getItem()).
        return "minecraft:" + std::string(ItemRegistry::Slug(item));
    }

    std::string ItemCooldowns::GetCooldownGroup(const ItemStack& item) {
        // MC ItemCooldowns.getCooldownGroup: the stack's USE_COOLDOWN
        // cooldown_group when it names one, else the item's registry id.
        if (auto cooldown = item.get(DataComponents::USE_COOLDOWN); cooldown && !cooldown->cooldownGroup.empty()) {
            return cooldown->cooldownGroup;
        }
        return GetCooldownGroup(item.itemId);
    }

    void ItemCooldowns::AddCooldown(const ItemStack& item, int ticks) {
        AddCooldown(GetCooldownGroup(item), ticks);
    }

    void ItemCooldowns::AddCooldown(const std::string& group, int ticks) {
        m_cooldowns[group] = Instance{m_tickCount, m_tickCount + ticks};
        OnCooldownStarted(group, ticks);
    }

    void ItemCooldowns::RemoveCooldown(const std::string& group) {
        m_cooldowns.erase(group);
        OnCooldownEnded(group);
    }

    void ItemCooldowns::Clear() {
        m_cooldowns.clear();
    }

    int UseCooldownTicks(ItemID item) {
        // The prototype's USE_COOLDOWN (Items.java: ENDER_PEARL
        // .useCooldown(1.0F), WIND_CHARGE .useCooldown(0.5F), CHORUS_FRUIT
        // .useCooldown(1.0F) — ConsumableComponents.cpp).
        if (auto cooldown = ItemRegistry::Get(item).defaultComponents.get(DataComponents::USE_COOLDOWN)) {
            return cooldown->Ticks();
        }
        return 0;
    }

    bool ApplyUseCooldown(ItemCooldowns& cooldowns, const ItemStack& stackBeforeUsing) {
        // MC UseCooldown.apply(stack, entity): the stack's USE_COOLDOWN
        // (any item a component patch gives one) on its cooldown group.
        if (stackBeforeUsing.IsEmpty()) return false;
        const auto cooldown = stackBeforeUsing.get(DataComponents::USE_COOLDOWN);
        if (!cooldown || cooldown->Ticks() <= 0) return false;
        cooldowns.AddCooldown(stackBeforeUsing, cooldown->Ticks());
        return true;
    }

} // namespace Game
