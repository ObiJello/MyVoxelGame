// File: src/common/entity/npc/MerchantOffer.hpp
//
// MC net.minecraft.world.item.trading.{ItemCost, MerchantOffer, MerchantOffers}
// — one trade and a merchant's list of them.
//
// A trade's PRICE is not a fixed number. getCostA derives it every time from
// the base price and three adjustments, which is the whole villager economy:
//
//   demand      rises when a trade sells out between restocks and falls when
//               it does not (updateDemand, run at each restock), scaled by
//               priceMultiplier — popular trades get dearer;
//   specialPriceDiff
//               the per-player discount/markup applied as the screen opens
//               (reputation × priceMultiplier, Hero of the Village), reset
//               for the next player;
//   clamp       1 .. the item's max stack size.
//
// Only cost A floats; cost B (the book under an enchanted book) is fixed.
//
// Saved as MC's MerchantOffer.CODEC (VillagerNbt.cpp) and synced as MC's
// MerchantOffer.STREAM_CODEC (WriteMerchantOffers below).
#pragma once

#include "common/entity/Item.hpp"
#include "common/data/DataComponentMap.hpp"

#include <optional>
#include <vector>

namespace Network { class PacketBuffer; class PacketReader; }

namespace Game {

    // MC ItemCost — an item, a count and an exact component predicate.
    struct ItemCost {
        ItemID           item = Items::Air;
        int              count = 1;
        // MC DataComponentExactPredicate: components the paying stack must
        // carry exactly (the water bottle's potion_contents).
        DataComponentMap components;

        ItemCost() = default;
        ItemCost(ItemID i, int c) : item(i), count(c) {}

        // MC itemStack(): the displayed stack — the item, the count and the
        // predicate's components as its patch.
        ItemStack AsStack() const;
        ItemStack AsStackWithCount(int c) const;
        // MC ItemCost.test: the same item carrying every predicate component.
        bool Test(const ItemStack& stack) const;
    };

    class MerchantOffer {
    public:
        MerchantOffer() = default;
        // MC's public constructors, collapsed.
        MerchantOffer(ItemCost baseCostA, std::optional<ItemCost> costB, ItemStack result,
                      int uses, int maxUses, int xp, float priceMultiplier, int demand = 0)
            : m_baseCostA(std::move(baseCostA)), m_costB(std::move(costB)),
              m_result(std::move(result)), m_uses(uses), m_maxUses(maxUses),
              m_demand(demand), m_priceMultiplier(priceMultiplier), m_xp(xp) {}

        // ── Prices ───────────────────────────────────────────────────────
        ItemStack GetBaseCostA() const { return m_baseCostA.AsStack(); }
        // MC getCostA: the base count plus demand and the special price,
        // clamped to 1..maxStackSize.
        ItemStack GetCostA() const { return m_baseCostA.AsStackWithCount(GetModifiedCostCount(m_baseCostA)); }
        ItemStack GetCostB() const { return m_costB ? m_costB->AsStack() : ItemStack{}; }
        const ItemCost&                GetItemCostA() const { return m_baseCostA; }
        const std::optional<ItemCost>& GetItemCostB() const { return m_costB; }
        const ItemStack&               GetResult()    const { return m_result; }
        // MC assemble — a copy of the result for the output slot.
        ItemStack Assemble() const { return m_result; }

        // ── Stock ────────────────────────────────────────────────────────
        int  GetUses()    const { return m_uses; }
        int  GetMaxUses() const { return m_maxUses; }
        void ResetUses()        { m_uses = 0; }
        void IncreaseUses()     { ++m_uses; }
        bool IsOutOfStock() const { return m_uses >= m_maxUses; }
        void SetToOutOfStock()    { m_uses = m_maxUses; }
        // MC needsRestock — any use since the last restock.
        bool NeedsRestock() const { return m_uses > 0; }
        // MC updateDemand: demand += uses - (maxUses - uses).
        void UpdateDemand() { m_demand = m_demand + m_uses - (m_maxUses - m_uses); }

        int   GetDemand() const { return m_demand; }
        void  SetDemand(int d)  { m_demand = d; }
        void  AddToSpecialPriceDiff(int add) { m_specialPriceDiff += add; }
        void  ResetSpecialPriceDiff()        { m_specialPriceDiff = 0; }
        int   GetSpecialPriceDiff() const    { return m_specialPriceDiff; }
        void  SetSpecialPriceDiff(int v)     { m_specialPriceDiff = v; }
        float GetPriceMultiplier() const     { return m_priceMultiplier; }
        int   GetXp() const                  { return m_xp; }
        bool  ShouldRewardExp() const        { return m_rewardExp; }
        void  SetRewardExp(bool v)           { m_rewardExp = v; }

        // MC satisfiedBy / take — payment in (buyA, buyB). take shrinks the
        // two stacks by the CURRENT prices.
        bool SatisfiedBy(const ItemStack& buyA, const ItemStack& buyB) const;
        bool Take(ItemStack& buyA, ItemStack& buyB) const;

        // MC STREAM_CODEC.
        void Write(Network::PacketBuffer& out) const;
        static MerchantOffer Read(Network::PacketReader& in);

    private:
        int GetModifiedCostCount(const ItemCost& cost) const;

        ItemCost                m_baseCostA;
        std::optional<ItemCost> m_costB;
        ItemStack               m_result;
        int   m_uses = 0;
        int   m_maxUses = 4;
        bool  m_rewardExp = true;
        int   m_specialPriceDiff = 0;
        int   m_demand = 0;
        float m_priceMultiplier = 0.0f;
        int   m_xp = 1;
    };

    // MC MerchantOffers (an ArrayList).
    class MerchantOffers : public std::vector<MerchantOffer> {
    public:
        // MC getRecipeFor: the hinted offer when the hint is in range (and
        // non-zero — MC's `selectionHint > 0`), else the first offer the
        // payment satisfies. Null when none does.
        MerchantOffer*       GetRecipeFor(const ItemStack& buyA, const ItemStack& buyB, int selectionHint);
        const MerchantOffer* GetRecipeFor(const ItemStack& buyA, const ItemStack& buyB,
                                          int selectionHint) const;
    };

    // MC MerchantOffers.STREAM_CODEC (a VarInt count, then each offer).
    void WriteMerchantOffers(Network::PacketBuffer& out, const MerchantOffers& offers);
    MerchantOffers ReadMerchantOffers(Network::PacketReader& in);

} // namespace Game
