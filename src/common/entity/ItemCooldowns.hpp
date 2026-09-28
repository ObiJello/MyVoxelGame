// File: src/common/entity/ItemCooldowns.hpp
//
// MC net.minecraft.world.item.ItemCooldowns — the per-player "this item is
// resting" table behind the white sweep over a hotbar slot after an ender
// pearl, a wind charge or a chorus fruit.
//
// Keyed by COOLDOWN GROUP (ItemCooldowns.getCooldownGroup): the item's own
// registry id unless its use_cooldown component names a group. No vanilla
// item names one, and this engine carries no use_cooldown component, so the
// group here is always "minecraft:<item slug>".
//
// Both sides keep one. The server's (ServerPlayer) is the authority: it
// refuses a use while the group rests (ServerPlayerGameMode.useItem) and
// tells the client about every cooldown it starts or cancels
// (ServerItemCooldowns → ClientboundCooldownPacket, CooldownS2C here). The
// client's copy only draws the overlay (GuiGraphics.itemCooldown) and keeps
// the client from asking for a use the server would refuse
// (MultiPlayerGameMode.useItem).
#pragma once

#include "common/entity/GeneratedItemList.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace Game {

    struct ItemStack;

    class ItemCooldowns {
    public:
        virtual ~ItemCooldowns() = default;

        // MC isOnCooldown / getCooldownPercent(item, partialTick): 1 at the
        // start, falling to 0 at the end.
        bool  IsOnCooldown(const ItemStack& item) const { return GetCooldownPercent(item, 0.0f) > 0.0f; }
        float GetCooldownPercent(const ItemStack& item, float partialTick) const;
        float GetCooldownPercent(const std::string& group, float partialTick) const;

        // MC tick: advance the clock and drop what ran out.
        void Tick();

        // MC getCooldownGroup.
        static std::string GetCooldownGroup(const ItemStack& item);
        static std::string GetCooldownGroup(ItemID item);

        // MC addCooldown(item, time) / addCooldown(group, time).
        void AddCooldown(const ItemStack& item, int ticks);
        void AddCooldown(const std::string& group, int ticks);
        // MC removeCooldown(group).
        void RemoveCooldown(const std::string& group);

        // Everything forgotten (a new life / a new connection — MC builds a
        // fresh player, and with it a fresh table).
        void Clear();

    protected:
        // MC onCooldownStarted / onCooldownEnded — the server's hooks for the
        // cooldown packet.
        virtual void OnCooldownStarted(const std::string& group, int duration) { (void)group; (void)duration; }
        virtual void OnCooldownEnded(const std::string& group) { (void)group; }

    private:
        struct Instance { int startTime = 0; int endTime = 0; };
        std::unordered_map<std::string, Instance> m_cooldowns;
        int m_tickCount = 0;
    };

    // MC Item.Properties.useCooldown(seconds) — UseCooldown.ticks(),
    // (int)(seconds * 20): ender pearl 1 s, wind charge 0.5 s, chorus fruit
    // 1 s (Items.java). 0 = the item has none.
    int UseCooldownTicks(ItemID item);

    // MC ItemStack.applyAfterUseComponentSideEffects' cooldown half:
    // UseCooldown.apply(stackBeforeUsing, user) — start the group's rest.
    // False (nothing started) for an item without the component.
    bool ApplyUseCooldown(ItemCooldowns& cooldowns, const ItemStack& stackBeforeUsing);

} // namespace Game
