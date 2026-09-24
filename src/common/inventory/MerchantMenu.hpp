// File: src/common/inventory/MerchantMenu.hpp
//
// MC net.minecraft.world.inventory.{MerchantMenu, MerchantContainer,
// MerchantResultSlot} — the trading screen's menu.
//
//   slot 0, 1   the two payment squares (MerchantContainer 0/1)
//   slot 2      the result (MerchantResultSlot): taking it performs the trade
//   3..29       the player's main rows, 30..38 the hotbar (MC
//               addStandardInventorySlots at 108, 84)
//
// The same class runs on both sides, as every menu here does. The SERVER's
// trader is the villager itself (resolved by entity id on every use, so a
// villager that dies or unloads mid-trade leaves the menu trading with an
// empty stand-in rather than a dangling pointer — the session closes it the
// same tick). The CLIENT's trader is a ClientSideMerchant that holds the
// offers MerchantOffersS2C delivered: its trades are predicted (uses tick up,
// the XP bar moves) and the server's answer overwrites them.
#pragma once

#include "AbstractContainerMenu.hpp"
#include "common/entity/npc/Merchant.hpp"

#include <functional>
#include <memory>

namespace Game {

    class MerchantMenu;

    // MC MerchantContainer: the three trade squares. Any change to a payment
    // square re-runs updateSellItem, which picks the matching offer and fills
    // the result square.
    class MerchantContainer : public IContainer {
    public:
        explicit MerchantContainer(MerchantMenu& menu) : m_menu(menu) {}

        int GetContainerSize() const override { return 3; }
        ItemStack&       GetItem(int index) override;
        const ItemStack& GetItem(int index) const override;
        // MC setItem: a payment square re-evaluates the offer.
        void SetItem(int index, const ItemStack& stack) override;
        // MC setChanged → updateSellItem.
        void SetChanged() override { UpdateSellItem(); }

        // Write without re-evaluating (MC removeItemNoUpdate / the result
        // square's own writes).
        void SetItemNoUpdate(int index, const ItemStack& stack);

        // MC updateSellItem.
        void UpdateSellItem();
        // MC getActiveOffer — the offer the result square shows; null when
        // none (or the one the payment matched is out of stock and nothing
        // else fits).
        MerchantOffer* GetActiveOffer();
        void SetSelectionHint(int hint) { m_selectionHint = hint; UpdateSellItem(); }
        int  GetFutureXp() const { return m_futureXp; }

        // True when the payments differ from those the result was last
        // computed from (MerchantMenu::SlotsChanged uses it).
        bool PaymentsChangedSinceUpdate() const;

    private:
        MerchantMenu& m_menu;
        ItemStack m_items[3];
        // An index, not a pointer: the offer list can grow (a level-up
        // appends the next tier) or be replaced (a client offers resync).
        int  m_activeOffer = -1;
        int  m_selectionHint = 0;
        int  m_futureXp = 0;
        ItemStack m_lastPayA, m_lastPayB;
    };

    // MC MerchantResultSlot.
    class MerchantResultSlot : public Slot {
    public:
        MerchantResultSlot(MerchantMenu& menu, MerchantContainer* container, int x, int y)
            : Slot(container, 2, x, y), m_menu(menu), m_trade(container) {}

        bool MayPlace(const ItemStack&) const override { return false; }
        // MC MerchantContainer.removeItem(2, n): the WHOLE result, whatever
        // was asked for — a right-click cannot split a trade.
        ItemStack Remove(int amount) override;
        // MC onTake: pay for the offer and tell the merchant.
        void OnTake(const ItemStack& taken, ContainerClickResult& result) override;

    private:
        MerchantMenu&      m_menu;
        MerchantContainer* m_trade;
    };

    class MerchantMenu : public AbstractContainerMenu {
    public:
        static constexpr int PAYMENT1_SLOT     = 0;
        static constexpr int PAYMENT2_SLOT     = 1;
        static constexpr int RESULT_SLOT       = 2;
        static constexpr int INV_SLOT_START    = 3;
        static constexpr int INV_SLOT_END      = 30;
        static constexpr int USE_ROW_SLOT_START = 30;
        static constexpr int USE_ROW_SLOT_END  = 39;
        // ClickMenuButton ids (ServerboundContainerButtonClickPacket). MC's
        // MerchantMenu has no buttons; this one is the screen's reroll.
        static constexpr int BUTTON_REROLL_TRADES = 0;

        using TraderResolver = std::function<Merchant*()>;

        // Server: the trader is looked up on every use.
        MerchantMenu(Inventory* playerInventory, TraderResolver trader);
        // Client: MC's MerchantMenu(containerId, inventory) — a
        // ClientSideMerchant filled from MerchantOffersS2C.
        explicit MerchantMenu(Inventory* playerInventory);

        // The trader (never null: an empty stand-in when the server's
        // villager is gone).
        Merchant& Trader();
        const Merchant& Trader() const { return const_cast<MerchantMenu*>(this)->Trader(); }
        // Non-null on the client.
        ClientSideMerchant* ClientMerchant() { return m_clientMerchant.get(); }

        // ── AbstractContainerMenu ────────────────────────────────────────
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        void SlotsChanged(ContainerClickResult& result) override;
        void Removed(ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;
        // BUTTON_REROLL_TRADES: re-roll the trader's offers when they are not
        // locked yet (Merchant::RerollTrades) and select the first trade.
        bool ClickMenuButton(int buttonId, bool mayBuild, ContainerClickResult& result) override;

        // ── MC MerchantMenu ──────────────────────────────────────────────
        void UpdateSellItem() { m_trade.UpdateSellItem(); }
        void SetSelectionHint(int hint) { m_trade.SetSelectionHint(hint); }
        // MC tryMoveItems: selecting a trade puts the current payments back
        // and fills the squares from the inventory with what it costs.
        void TryMoveItems(int newTradeIndex, ContainerClickResult& result);

        int  GetTraderXp() const { return Trader().GetVillagerXp(); }
        int  GetFutureTraderXp() const { return m_trade.GetFutureXp(); }
        void SetXp(int xp) { Trader().OverrideXp(xp); }
        int  GetTraderLevel() const { return m_merchantLevel; }
        void SetMerchantLevel(int level) { m_merchantLevel = level; }
        bool ShowProgressBar() const { return m_showProgressBar; }
        void SetShowProgressBar(bool show) { m_showProgressBar = show; }
        bool CanRestock() const { return m_canRestock; }
        void SetCanRestock(bool v) { m_canRestock = v; }
        void SetOffers(const MerchantOffers& offers) { Trader().OverrideOffers(offers); }
        MerchantOffers& GetOffers() { return Trader().GetOffers(); }

        MerchantContainer& TradeContainer() { return m_trade; }

        static void NoteChanged(ContainerClickResult& r, int slot) { MarkChanged(r, slot); }

    private:
        void Build(Inventory* playerInventory);
        void MoveFromInventoryToPaymentSlot(int paymentSlot, const ItemCost& cost,
                                            ContainerClickResult& result);

        TraderResolver                      m_resolve;
        std::unique_ptr<ClientSideMerchant> m_clientMerchant;
        ClientSideMerchant                  m_absent;   // the stand-in
        MerchantContainer                   m_trade{ *this };
        int  m_merchantLevel = 0;
        bool m_showProgressBar = false;
        bool m_canRestock = false;
    };

} // namespace Game
