// File: src/common/inventory/SystemMenus.cpp
#include "SystemMenus.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;

        // Lay out the player's 27 + 9 at `top`. Every menu here does it the
        // same way (MC addStandardInventorySlots).
        void AddPlayerSlots(AbstractContainerMenu& menu, Inventory* inv, int top,
                            const std::function<Slot&(std::unique_ptr<Slot>)>& add) {
            (void)menu;
            for (int i = 0; i < 27; ++i) {
                add(std::make_unique<Slot>(inv, Inventory::MAIN_BEGIN + i,
                                           8 + (i % 9) * SLOT_STEP,
                                           top + (i / 9) * SLOT_STEP));
            }
            for (int i = 0; i < 9; ++i) {
                add(std::make_unique<Slot>(inv, Inventory::HOTBAR_BEGIN + i,
                                           8 + i * SLOT_STEP, top + 58));
            }
        }
    }

    // ══════════════════ Enchanting table ══════════════════════════════════
    EnchantmentMenu::EnchantmentMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory),
          m_random(static_cast<int64_t>(
              std::chrono::steady_clock::now().time_since_epoch().count())) {
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));

        // MC EnchantmentMenu: item at (15,47), lapis at (35,47).
        AddSlot(std::make_unique<Slot>(&m_inputs, SLOT_ITEM,  15, 47));
        AddSlot(std::make_unique<Slot>(&m_inputs, SLOT_LAPIS, 35, 47));
        auto add = [this](std::unique_ptr<Slot> s) -> Slot& { return AddSlot(std::move(s)); };
        AddPlayerSlots(*this, playerInventory, 84, add);

        m_seed = m_random.NextInt(0x7FFFFFFF);
        SetData(DATA_SEED, m_seed);
    }

    int EnchantmentMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return MAIN_BEGIN + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    void EnchantmentMenu::SetBookshelfPower(int power) {
        m_power = std::clamp(power, 0, 15);   // MC caps bookcases at 15
        RollOffers();
    }

    void EnchantmentMenu::SlotsChanged(ContainerClickResult& result) {
        RollOffers();
        for (int i = 0; i < 3; ++i) MarkChanged(result, SLOT_ITEM);
    }

    void EnchantmentMenu::RollOffers() {
        const ItemStack& item = m_inputs.GetItem(SLOT_ITEM);
        auto clearOffers = [this] {
            for (int i = 0; i < 3; ++i) {
                SetData(DATA_COST_0 + i, 0);
                SetData(DATA_CLUE_ID_0 + i, -1);
                SetData(DATA_CLUE_LVL_0 + i, -1);
            }
        };
        if (item.IsEmpty()) { clearOffers(); return; }

        // MC EnchantmentMenu.slotsChanged: reseed from the stored enchantment
        // seed so the same item + same table always shows the same three
        // offers until one is taken. Re-rolling per frame would make the rows
        // flicker.
        m_random.SetSeed(m_seed);

        for (int i = 0; i < 3; ++i) {
            // EnchantmentHelper.getEnchantmentCost (line 451-465), verbatim.
            int selected = m_random.NextInt(8) + 1 + (m_power >> 1)
                         + m_random.NextInt(m_power + 1);
            int cost;
            if (i == 0)      cost = std::max(selected / 3, 1);
            else if (i == 1) cost = selected * 2 / 3 + 1;
            else             cost = std::max(selected, m_power * 2);
            // MC blanks a row whose cost can't cover its slot index.
            if (cost < i + 1) cost = 0;
            SetData(DATA_COST_0 + i, cost);

            // The clue is one enchantment the roll would grant — MC shows it
            // greyed in the row. Picked from the registry with the same RNG so
            // client and server agree.
            const auto& all = EnchantmentRegistry::All();
            if (cost > 0 && !all.empty()) {
                const int idx = m_random.NextInt(static_cast<int>(all.size()));
                SetData(DATA_CLUE_ID_0 + i, idx);
                SetData(DATA_CLUE_LVL_0 + i,
                        std::max(1, std::min(all[static_cast<size_t>(idx)].maxLevel,
                                             1 + cost / 10)));
            } else {
                SetData(DATA_CLUE_ID_0 + i, -1);
                SetData(DATA_CLUE_LVL_0 + i, -1);
            }
        }
    }

    int EnchantmentMenu::TakeOffer(int slot, int playerLevel, bool creative,
                                   ContainerClickResult& result) {
        if (slot < 0 || slot > 2) return 0;
        const int cost = GetData(DATA_COST_0 + slot);
        if (cost <= 0) return 0;

        ItemStack& item  = m_inputs.GetItem(SLOT_ITEM);
        ItemStack& lapis = m_inputs.GetItem(SLOT_LAPIS);
        if (item.IsEmpty()) return 0;

        // MC EnchantmentMenu.clickMenuButton: creative skips both checks;
        // otherwise you need the LEVELS and (slot+1) lapis.
        const int lapisNeeded = slot + 1;
        if (!creative) {
            if (playerLevel < cost || playerLevel < lapisNeeded) return 0;
            if (lapis.itemId != Items::LapisLazuli || lapis.count < lapisNeeded) return 0;
        }

        // Apply the clue as the granted enchantment. MC rolls a whole weighted
        // set here (EnchantmentHelper.selectEnchantment); we grant the single
        // clue enchantment, which is the same shape with one entry — enough for
        // the table to work, and honest about not modelling weights yet.
        const int clueId  = GetData(DATA_CLUE_ID_0 + slot);
        const int clueLvl = GetData(DATA_CLUE_LVL_0 + slot);
        if (clueId >= 0 && clueLvl > 0) {
            ItemEnchantments enchants;
            auto existing = item.get(DataComponents::STORED_ENCHANTMENTS);
            if (existing.has_value()) enchants = *existing;
            enchants.entries.push_back(
                EnchantmentInstance{static_cast<EnchantmentId>(clueId), clueLvl});
            item.components.set(DataComponents::STORED_ENCHANTMENTS, enchants);
        }

        if (!creative && !lapis.IsEmpty()) {
            lapis.count -= lapisNeeded;
            if (lapis.count <= 0) lapis.Clear();
        }

        // A fresh seed means fresh offers next time, exactly as MC does in
        // onEnchantmentPerformed.
        m_seed = m_random.NextInt(0x7FFFFFFF);
        SetData(DATA_SEED, m_seed);
        RollOffers();

        MarkChanged(result, SLOT_ITEM);
        MarkChanged(result, SLOT_LAPIS);
        return cost;
    }

    void EnchantmentMenu::Removed(ContainerClickResult& result) {
        for (int i = 0; i < 2; ++i) {
            ItemStack& stack = m_inputs.GetItem(i);
            if (stack.IsEmpty()) continue;
            (void)getInventory().AddStack(stack);
            stack.Clear();
            MarkChanged(result, i);
        }
    }

    void EnchantmentMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;

        bool moved;
        if (slotIndex < RESULT_END) {
            moved = MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, false, result);
        } else if (stack.itemId == Items::LapisLazuli) {
            moved = MoveItemStackTo(stack, SLOT_LAPIS, SLOT_LAPIS + 1, false, result);
        } else {
            moved = MoveItemStackTo(stack, SLOT_ITEM, SLOT_ITEM + 1, false, result);
        }
        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

    // ══════════════════ Brewing stand ═════════════════════════════════════
    BrewingStandMenu::BrewingStandMenu(Inventory* playerInventory, IContainer* container)
        : AbstractContainerMenu(playerInventory) {
        BuildSlots(playerInventory, container);
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
    }

    BrewingStandMenu::BrewingStandMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory),
          m_ownedContainer(std::make_unique<SimpleContainer>(CONTAINER_END)) {
        BuildSlots(playerInventory, m_ownedContainer.get());
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
    }

    void BrewingStandMenu::BuildSlots(Inventory* playerInventory, IContainer* container) {
        // MC BrewingStandMenu: bottles at (56,51),(79,58),(102,51),
        // ingredient (79,17), fuel (17,17).
        AddSlot(std::make_unique<Slot>(container, 0, 56, 51));
        AddSlot(std::make_unique<Slot>(container, 1, 79, 58));
        AddSlot(std::make_unique<Slot>(container, 2, 102, 51));
        AddSlot(std::make_unique<Slot>(container, SLOT_INGREDIENT, 79, 17));
        AddSlot(std::make_unique<Slot>(container, SLOT_FUEL, 17, 17));
        auto add = [this](std::unique_ptr<Slot> s) -> Slot& { return AddSlot(std::move(s)); };
        AddPlayerSlots(*this, playerInventory, 84, add);
    }

    int BrewingStandMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return MAIN_BEGIN + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    void BrewingStandMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const bool moved = (slotIndex < CONTAINER_END)
            ? MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, true, result)
            : MoveItemStackTo(stack, 0, CONTAINER_END, false, result);
        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

    // ══════════════════ Beacon ════════════════════════════════════════════
    BeaconMenu::BeaconMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory) {
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
        // MC BeaconMenu: the payment slot sits at (136,110).
        AddSlot(std::make_unique<Slot>(&m_payment, 0, 136, 110));
        auto add = [this](std::unique_ptr<Slot> s) -> Slot& { return AddSlot(std::move(s)); };
        AddPlayerSlots(*this, playerInventory, 137, add);
    }

    int BeaconMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return MAIN_BEGIN + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    void BeaconMenu::Removed(ContainerClickResult& result) {
        ItemStack& stack = m_payment.GetItem(0);
        if (stack.IsEmpty()) return;
        (void)getInventory().AddStack(stack);
        stack.Clear();
        MarkChanged(result, SLOT_PAYMENT);
    }

    void BeaconMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const bool moved = (slotIndex == SLOT_PAYMENT)
            ? MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, true, result)
            : MoveItemStackTo(stack, SLOT_PAYMENT, SLOT_PAYMENT + 1, false, result);
        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

    // ══════════════════ Crafter ═══════════════════════════════════════════
    CrafterMenu::CrafterMenu(Inventory* playerInventory, IContainer* container)
        : AbstractContainerMenu(playerInventory) {
        BuildSlots(playerInventory, container);
    }

    CrafterMenu::CrafterMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory),
          m_ownedContainer(std::make_unique<SimpleContainer>(GRID_SIZE)) {
        BuildSlots(playerInventory, m_ownedContainer.get());
    }

    void CrafterMenu::BuildSlots(Inventory* playerInventory, IContainer* container) {
        // MC CrafterMenu: the 3x3 starts at (30,17), like a crafting table's.
        for (int i = 0; i < GRID_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(container, i,
                                           30 + (i % 3) * SLOT_STEP,
                                           17 + (i / 3) * SLOT_STEP));
        }
        auto add = [this](std::unique_ptr<Slot> s) -> Slot& { return AddSlot(std::move(s)); };
        AddPlayerSlots(*this, playerInventory, 84, add);
    }

    int CrafterMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return MAIN_BEGIN + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    void CrafterMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const bool moved = (slotIndex < GRID_SIZE)
            ? MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, true, result)
            : MoveItemStackTo(stack, 0, GRID_SIZE, false, result);
        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

} // namespace Game
