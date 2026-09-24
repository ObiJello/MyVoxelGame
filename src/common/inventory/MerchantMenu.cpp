// File: src/common/inventory/MerchantMenu.cpp
#include "MerchantMenu.hpp"

#include <algorithm>
#include <utility>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;
        const ItemStack kEmptyStack{};
    }

    // ── MerchantContainer ────────────────────────────────────────────────

    ItemStack& MerchantContainer::GetItem(int index) {
        static ItemStack scratch{};
        if (index < 0 || index >= 3) { scratch = ItemStack{}; return scratch; }
        return m_items[index];
    }

    const ItemStack& MerchantContainer::GetItem(int index) const {
        if (index < 0 || index >= 3) return kEmptyStack;
        return m_items[index];
    }

    void MerchantContainer::SetItemNoUpdate(int index, const ItemStack& stack) {
        if (index < 0 || index >= 3) return;
        m_items[index] = stack;
    }

    void MerchantContainer::SetItem(int index, const ItemStack& stack) {
        if (index < 0 || index >= 3) return;
        m_items[index] = stack;
        // MC itemStack.limitSize(getMaxStackSize(itemStack)).
        if (!m_items[index].IsEmpty()) {
            m_items[index].count = std::min(m_items[index].count, IContainer::GetMaxStackSize(m_items[index]));
        }
        if (index == 0 || index == 1) UpdateSellItem();
    }

    MerchantOffer* MerchantContainer::GetActiveOffer() {
        MerchantOffers& offers = m_menu.Trader().GetOffers();
        if (m_activeOffer < 0 || m_activeOffer >= static_cast<int>(offers.size())) return nullptr;
        return &offers[static_cast<size_t>(m_activeOffer)];
    }

    bool MerchantContainer::PaymentsChangedSinceUpdate() const {
        const auto differs = [](const ItemStack& a, const ItemStack& b) {
            return a.count != b.count || (!(a.IsEmpty() && b.IsEmpty()) && !IsSameItemSameComponents(a, b));
        };
        return differs(m_items[0], m_lastPayA) || differs(m_items[1], m_lastPayB);
    }

    void MerchantContainer::UpdateSellItem() {
        m_lastPayA = m_items[0];
        m_lastPayB = m_items[1];
        m_activeOffer = -1;
        ItemStack buyA, buyB;
        if (m_items[0].IsEmpty()) {
            buyA = m_items[1];
        } else {
            buyA = m_items[0];
            buyB = m_items[1];
        }

        if (buyA.IsEmpty()) {
            m_items[2] = ItemStack{};
            m_futureXp = 0;
            return;
        }
        Merchant& merchant = m_menu.Trader();
        MerchantOffers& offers = merchant.GetOffers();
        if (offers.empty()) return;

        const auto indexOf = [&offers](const MerchantOffer* o) {
            return o ? static_cast<int>(o - offers.data()) : -1;
        };
        MerchantOffer* offer = offers.GetRecipeFor(buyA, buyB, m_selectionHint);
        if (!offer || offer->IsOutOfStock()) {
            m_activeOffer = indexOf(offer);
            offer = offers.GetRecipeFor(buyB, buyA, m_selectionHint);
        }
        if (offer && !offer->IsOutOfStock()) {
            m_activeOffer = indexOf(offer);
            m_items[2] = offer->Assemble();
            m_futureXp = offer->GetXp();
        } else {
            m_items[2] = ItemStack{};
            m_futureXp = 0;
        }
        merchant.NotifyTradeUpdated(m_items[2]);
    }

    // ── MerchantResultSlot ───────────────────────────────────────────────

    ItemStack MerchantResultSlot::Remove(int amount) {
        (void)amount;
        ItemStack out = m_trade->GetItem(2);
        if (out.IsEmpty()) return {};
        m_trade->SetItemNoUpdate(2, ItemStack{});
        // MC tryRemove → setByPlayer(EMPTY) → setChanged → updateSellItem.
        SetChanged();
        return out;
    }

    void MerchantResultSlot::OnTake(const ItemStack& taken, ContainerClickResult& result) {
        (void)taken;
        MerchantOffer* offer = m_trade->GetActiveOffer();
        if (!offer) return;
        Merchant& merchant = m_menu.Trader();
        // Read before notifyTrade: a level-up appends offers, which can move
        // the list and with it `offer`.
        const int offerXp = offer->GetXp();
        ItemStack& buyA = m_trade->GetItem(0);
        ItemStack& buyB = m_trade->GetItem(1);
        if (offer->Take(buyA, buyB) || offer->Take(buyB, buyA)) {
            merchant.NotifyTrade(*offer);
            // (MC awards Stats.TRADED_WITH_VILLAGER — no statistics here.)
            const ItemStack a = buyA, b = buyB;
            m_trade->SetItem(0, a);
            m_trade->SetItem(1, b);
        }
        merchant.OverrideXp(merchant.GetVillagerXp() + offerXp);
        MerchantMenu::NoteChanged(result, MerchantMenu::PAYMENT1_SLOT);
        MerchantMenu::NoteChanged(result, MerchantMenu::PAYMENT2_SLOT);
        MerchantMenu::NoteChanged(result, MerchantMenu::RESULT_SLOT);
    }

    // ── MerchantMenu ─────────────────────────────────────────────────────

    MerchantMenu::MerchantMenu(Inventory* playerInventory, TraderResolver trader)
        : AbstractContainerMenu(playerInventory), m_resolve(std::move(trader)) {
        Build(playerInventory);
    }

    MerchantMenu::MerchantMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory),
          m_clientMerchant(std::make_unique<ClientSideMerchant>()) {
        Build(playerInventory);
    }

    void MerchantMenu::Build(Inventory* playerInventory) {
        AddSlot(std::make_unique<Slot>(&m_trade, 0, 136, 37));
        AddSlot(std::make_unique<Slot>(&m_trade, 1, 162, 37));
        AddSlot(std::make_unique<MerchantResultSlot>(*this, &m_trade, 220, 37));
        // MC addStandardInventorySlots(inventory, 108, 84).
        for (int i = 0; i < 27; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::MAIN_BEGIN + i,
                                           108 + (i % 9) * SLOT_STEP, 84 + (i / 9) * SLOT_STEP));
        }
        for (int i = 0; i < 9; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::HOTBAR_BEGIN + i,
                                           108 + i * SLOT_STEP, 84 + 58));
        }
    }

    Merchant& MerchantMenu::Trader() {
        if (m_clientMerchant) return *m_clientMerchant;
        if (m_resolve) {
            if (Merchant* m = m_resolve()) return *m;
        }
        return m_absent;
    }

    int MerchantMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return INV_SLOT_START + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return USE_ROW_SLOT_START + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    bool MerchantMenu::ClickMenuButton(int buttonId, bool mayBuild, ContainerClickResult& result) {
        (void)mayBuild; (void)result;
        if (buttonId != BUTTON_REROLL_TRADES) return false;
        Merchant& trader = Trader();
        if (trader.IsClientSideMerchant() || !trader.RerollTrades()) return false;
        // The old list is gone: the result square re-picks from the new one,
        // starting at the first trade (the screen resets its selection too).
        // Payments stay where they are, as they do when a level-up changes
        // the list under an open screen.
        m_trade.SetSelectionHint(0);
        return true;
    }

    void MerchantMenu::SlotsChanged(ContainerClickResult& result) {
        // MC slotsChanged → tradeContainer.updateSellItem. Every Slot write
        // already re-evaluated through MerchantContainer::SetChanged; this
        // catches a payment square edited in place, without re-running (and
        // re-voicing the villager's yes/no) when nothing moved.
        if (m_trade.PaymentsChangedSinceUpdate()) m_trade.UpdateSellItem();
        MarkChanged(result, RESULT_SLOT);
    }

    void MerchantMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        // MC MerchantMenu.quickMoveStack. The trade itself (slot.onTake) and
        // the repeat are the base's QUICK_MOVE loop: it calls OnTake after
        // every iteration that changed the slot, and repeats while the result
        // refills — one shift-click trades until the payment runs out.
        if (!IsValidSlotIndex(slotIndex)) return;
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;

        if (slotIndex == RESULT_SLOT) {
            // Move straight out of the square without re-evaluating it: the
            // base must see the result gone (or shrunk) to run OnTake, which
            // is what pays for it and refills it.
            ItemStack& stack = m_trade.GetItem(RESULT_SLOT);
            if (!MoveItemStackTo(stack, INV_SLOT_START, USE_ROW_SLOT_END, true, result)) return;
            if (stack.count <= 0) stack.Clear();
            MarkChanged(result, RESULT_SLOT);
            // (MC playTradeSound runs only for a server-side merchant's
            // playLocalSound, which a server level ignores — silent.)
            return;
        }

        ItemStack& stack = slot.GetItemMut();
        const int before = stack.count;
        bool moved;
        if (slotIndex != PAYMENT1_SLOT && slotIndex != PAYMENT2_SLOT) {
            if (slotIndex >= INV_SLOT_START && slotIndex < INV_SLOT_END) {
                moved = MoveItemStackTo(stack, USE_ROW_SLOT_START, USE_ROW_SLOT_END, false, result);
            } else {
                moved = MoveItemStackTo(stack, INV_SLOT_START, INV_SLOT_END, false, result);
            }
        } else {
            moved = MoveItemStackTo(stack, INV_SLOT_START, USE_ROW_SLOT_END, false, result);
        }
        if (!moved) return;
        if (stack.count <= 0) {
            slot.SetByPlayer(ItemStack{});
        } else {
            slot.SetChanged();
        }
        if (stack.count != before) MarkChanged(result, slotIndex);
    }

    void MerchantMenu::Removed(ContainerClickResult& result) {
        // MC removed: the trader stops trading; the payments go back to the
        // player (placeItemBackInInventory), dropping what does not fit.
        Trader().ClearTradingPlayer();
        for (int i = PAYMENT1_SLOT; i <= PAYMENT2_SLOT; ++i) {
            ItemStack stack = m_trade.GetItem(i);
            m_trade.SetItemNoUpdate(i, ItemStack{});
            if (stack.IsEmpty()) continue;
            const int leftover = getInventory().AddStack(stack);
            if (leftover > 0) {
                ItemStack overflow = stack;
                overflow.count = leftover;
                result.extraDrops.push_back(overflow);
            }
            MarkChanged(result, i);
        }
        m_trade.SetItemNoUpdate(RESULT_SLOT, ItemStack{});
    }

    void MerchantMenu::TryMoveItems(int newTradeIndex, ContainerClickResult& result) {
        MerchantOffers& offers = GetOffers();
        if (newTradeIndex < 0 || newTradeIndex >= static_cast<int>(offers.size())) return;

        for (int paymentSlot = PAYMENT1_SLOT; paymentSlot <= PAYMENT2_SLOT; ++paymentSlot) {
            ItemStack old = m_trade.GetItem(paymentSlot);
            if (old.IsEmpty()) continue;
            if (!MoveItemStackTo(old, INV_SLOT_START, USE_ROW_SLOT_END, true, result)) return;
            m_trade.SetItem(paymentSlot, old.count > 0 ? old : ItemStack{});
            MarkChanged(result, paymentSlot);
        }

        if (m_trade.GetItem(PAYMENT1_SLOT).IsEmpty() && m_trade.GetItem(PAYMENT2_SLOT).IsEmpty()) {
            // Copy the costs: the moves below re-evaluate the offer, and the
            // list must not be read through a reference that could move.
            const MerchantOffer offer = offers[static_cast<size_t>(newTradeIndex)];
            MoveFromInventoryToPaymentSlot(PAYMENT1_SLOT, offer.GetItemCostA(), result);
            if (offer.GetItemCostB()) {
                MoveFromInventoryToPaymentSlot(PAYMENT2_SLOT, *offer.GetItemCostB(), result);
            }
        }
        MarkChanged(result, RESULT_SLOT);
    }

    void MerchantMenu::MoveFromInventoryToPaymentSlot(int paymentSlot, const ItemCost& cost,
                                                      ContainerClickResult& result) {
        for (int i = INV_SLOT_START; i < USE_ROW_SLOT_END; ++i) {
            ItemStack& inventoryItem = GetSlot(i).GetItemMut();
            if (inventoryItem.IsEmpty() || !cost.Test(inventoryItem)) continue;
            const ItemStack current = m_trade.GetItem(paymentSlot);
            if (!current.IsEmpty() && !IsSameItemSameComponents(inventoryItem, current)) continue;
            const int maxStackSize = ItemRegistry::Get(inventoryItem.itemId).maxStackSize;
            const int moveCount = std::min(maxStackSize - current.count, inventoryItem.count);
            ItemStack newPayment = inventoryItem;
            newPayment.count = current.count + moveCount;
            inventoryItem.count -= moveCount;
            if (inventoryItem.count <= 0) inventoryItem.Clear();
            GetSlot(i).SetChanged();
            MarkChanged(result, i);
            m_trade.SetItem(paymentSlot, newPayment);
            MarkChanged(result, paymentSlot);
            if (newPayment.count >= maxStackSize) break;
        }
    }

} // namespace Game
