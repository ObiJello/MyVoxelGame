// File: src/common/entity/npc/MerchantOffer.cpp
#include "common/entity/npc/MerchantOffer.hpp"

#include "common/network/ItemStackSerialization.hpp"
#include "common/network/PacketRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    // ── ItemCost ─────────────────────────────────────────────────────────

    ItemStack ItemCost::AsStack() const { return AsStackWithCount(count); }

    ItemStack ItemCost::AsStackWithCount(int c) const {
        if (item == Items::Air || c <= 0) return ItemStack{};
        ItemStack stack(item, c);
        stack.components = components;
        return stack;
    }

    bool ItemCost::Test(const ItemStack& stack) const {
        if (stack.IsEmpty() || stack.itemId != item) return false;
        return components.IsExactSubsetOf(stack.components,
                                          &ItemRegistry::Get(stack.itemId).defaultComponents);
    }

    // ── MerchantOffer ────────────────────────────────────────────────────

    int MerchantOffer::GetModifiedCostCount(const ItemCost& cost) const {
        // MC: max(0, floor(basePrice * demand * priceMultiplier)), then
        // clamp(base + demandDiff + specialPriceDiff, 1, maxStackSize).
        const int basePrice = cost.count;
        const int demandDiff = std::max(0, static_cast<int>(std::floor(
            static_cast<float>(basePrice * m_demand) * m_priceMultiplier)));
        const int maxStack = ItemRegistry::Get(cost.item).maxStackSize;
        return std::clamp(basePrice + demandDiff + m_specialPriceDiff, 1, std::max(1, maxStack));
    }

    bool MerchantOffer::SatisfiedBy(const ItemStack& buyA, const ItemStack& buyB) const {
        if (!m_baseCostA.Test(buyA) || buyA.count < GetModifiedCostCount(m_baseCostA)) return false;
        if (!m_costB) return buyB.IsEmpty();
        return m_costB->Test(buyB) && buyB.count >= m_costB->count;
    }

    bool MerchantOffer::Take(ItemStack& buyA, ItemStack& buyB) const {
        if (!SatisfiedBy(buyA, buyB)) return false;
        buyA.count -= GetCostA().count;
        if (buyA.count <= 0) buyA.Clear();
        const ItemStack costB = GetCostB();
        if (!costB.IsEmpty()) {
            buyB.count -= costB.count;
            if (buyB.count <= 0) buyB.Clear();
        }
        return true;
    }

    namespace {
        // MC ItemCost.STREAM_CODEC: item, VarInt count, the exact predicate
        // (a component patch — the engine's DataComponentMap wire form).
        void WriteItemCost(Network::PacketBuffer& out, const ItemCost& cost) {
            out.WriteVarInt(cost.item);
            out.WriteVarInt(static_cast<uint32_t>(cost.count));
            cost.components.Serialize(out);
        }
        ItemCost ReadItemCost(Network::PacketReader& in) {
            ItemCost cost;
            cost.item = in.ReadVarInt();
            cost.count = static_cast<int>(in.ReadVarInt());
            cost.components = DataComponentMap::Deserialize(in);
            return cost;
        }
    }

    void MerchantOffer::Write(Network::PacketBuffer& out) const {
        // MC MerchantOffer.writeToStream, field for field.
        WriteItemCost(out, m_baseCostA);
        Network::Serialization::WriteItemStack(out, m_result);
        out.WriteByte(m_costB ? 1 : 0);
        if (m_costB) WriteItemCost(out, *m_costB);
        out.WriteByte(IsOutOfStock() ? 1 : 0);
        out.WriteInt(static_cast<uint32_t>(m_uses));
        out.WriteInt(static_cast<uint32_t>(m_maxUses));
        out.WriteInt(static_cast<uint32_t>(m_xp));
        out.WriteInt(static_cast<uint32_t>(m_specialPriceDiff));
        out.WriteFloat(m_priceMultiplier);
        out.WriteInt(static_cast<uint32_t>(m_demand));
    }

    MerchantOffer MerchantOffer::Read(Network::PacketReader& in) {
        // MC MerchantOffer.createFromStream.
        ItemCost buy = ReadItemCost(in);
        ItemStack sell = Network::Serialization::ReadItemStack(in);
        std::optional<ItemCost> buyB;
        if (in.ReadByte() != 0) buyB = ReadItemCost(in);
        const bool exhausted = in.ReadByte() != 0;
        const int uses     = static_cast<int32_t>(in.ReadInt());
        const int maxUses  = static_cast<int32_t>(in.ReadInt());
        const int xp       = static_cast<int32_t>(in.ReadInt());
        const int special  = static_cast<int32_t>(in.ReadInt());
        const float mult   = in.ReadFloat();
        const int demand   = static_cast<int32_t>(in.ReadInt());
        MerchantOffer offer(std::move(buy), std::move(buyB), std::move(sell),
                            uses, maxUses, xp, mult, demand);
        if (exhausted) offer.SetToOutOfStock();
        offer.SetSpecialPriceDiff(special);
        return offer;
    }

    // ── MerchantOffers ───────────────────────────────────────────────────

    MerchantOffer* MerchantOffers::GetRecipeFor(const ItemStack& buyA, const ItemStack& buyB,
                                                int selectionHint) {
        if (selectionHint > 0 && selectionHint < static_cast<int>(size())) {
            MerchantOffer& offer = (*this)[static_cast<size_t>(selectionHint)];
            return offer.SatisfiedBy(buyA, buyB) ? &offer : nullptr;
        }
        for (MerchantOffer& offer : *this) {
            if (offer.SatisfiedBy(buyA, buyB)) return &offer;
        }
        return nullptr;
    }

    const MerchantOffer* MerchantOffers::GetRecipeFor(const ItemStack& buyA, const ItemStack& buyB,
                                                      int selectionHint) const {
        return const_cast<MerchantOffers*>(this)->GetRecipeFor(buyA, buyB, selectionHint);
    }

    void WriteMerchantOffers(Network::PacketBuffer& out, const MerchantOffers& offers) {
        out.WriteVarInt(static_cast<uint32_t>(offers.size()));
        for (const MerchantOffer& o : offers) o.Write(out);
    }

    MerchantOffers ReadMerchantOffers(Network::PacketReader& in) {
        MerchantOffers offers;
        const uint32_t n = in.ReadVarInt();
        // A trade list is a handful (a master villager has 10); anything
        // huge is a corrupt stream, and reserving for it would be the crash.
        if (n > 4096) return offers;
        offers.reserve(n);
        for (uint32_t i = 0; i < n; ++i) offers.push_back(MerchantOffer::Read(in));
        return offers;
    }

} // namespace Game
