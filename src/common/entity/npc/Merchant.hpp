// File: src/common/entity/npc/Merchant.hpp
//
// MC net.minecraft.world.item.trading.Merchant and
// net.minecraft.world.entity.npc.ClientSideMerchant — the trading partner a
// MerchantMenu talks to.
//
// The SERVER's merchant is the villager (or wandering trader) itself: taking
// a trade's result calls NotifyTrade, which spends a use, pays the villager's
// XP, rolls the orb and may level it up. The CLIENT has no villager to talk
// to — the menu it builds on OpenScreen trades against a ClientSideMerchant
// holding the offers the server sent (MerchantOffersS2C). Its NotifyTrade
// only marks the use and its OverrideXp only moves the bar, so the screen
// reacts to a click the instant it is predicted; the server's copy decides.
#pragma once

#include "common/entity/npc/MerchantOffer.hpp"

namespace Game {

    class Merchant {
    public:
        virtual ~Merchant() = default;

        // MC getOffers. The server's villager generates its trades lazily on
        // first read (AbstractVillager.getOffers → updateTrades).
        virtual MerchantOffers& GetOffers() = 0;
        // MC overrideOffers / overrideXp — client-side only in effect.
        virtual void OverrideOffers(const MerchantOffers& offers) { (void)offers; }
        virtual void OverrideXp(int xp) { (void)xp; }

        // MC notifyTrade — one trade happened (after MerchantOffer.take).
        virtual void NotifyTrade(MerchantOffer& offer) = 0;
        // MC notifyTradeUpdated — the result square changed (yes/no voice).
        virtual void NotifyTradeUpdated(const ItemStack& result) = 0;

        virtual int  GetVillagerXp() const = 0;
        virtual bool ShowProgressBar() const = 0;
        virtual bool CanRestock() const { return false; }
        // MC getNotifyTradeSound.
        virtual const char* GetNotifyTradeSound() const { return "entity.villager.yes"; }
        virtual bool IsClientSideMerchant() const = 0;
        // MC setTradingPlayer(null) — the menu closed (MerchantMenu.removed).
        virtual void ClearTradingPlayer() {}

        // The merchant screen's reroll button (MerchantMenu::
        // BUTTON_REROLL_TRADES). A villager whose trades are not locked yet
        // (MC ResetProfession's test: a real profession, no XP, level 1) can
        // have them re-rolled, as losing and re-taking its job site would.
        // Server-side only; false for everything else.
        virtual bool CanRerollTrades() const { return false; }
        virtual bool RerollTrades() { return false; }
    };

    // MC ClientSideMerchant.
    class ClientSideMerchant final : public Merchant {
    public:
        MerchantOffers& GetOffers() override { return m_offers; }
        void OverrideOffers(const MerchantOffers& offers) override { m_offers = offers; }
        void OverrideXp(int xp) override { m_xp = xp; }
        // MC ClientSideMerchant.notifyTrade: offer.increaseUses() only.
        void NotifyTrade(MerchantOffer& offer) override { offer.IncreaseUses(); }
        void NotifyTradeUpdated(const ItemStack&) override {}
        int  GetVillagerXp() const override { return m_xp; }
        bool ShowProgressBar() const override { return true; }
        bool IsClientSideMerchant() const override { return true; }

    private:
        MerchantOffers m_offers;
        int            m_xp = 0;
    };

} // namespace Game
