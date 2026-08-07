// File: src/common/inventory/InventoryMenu.cpp
#include "InventoryMenu.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include <memory>

namespace Game {

    namespace {
        // GUI positions, panel-image-relative. These were hardcoded in
        // InventoryScreen::GetSlotImagePos; they live here now so a slot's
        // position and its behaviour can never disagree.
        constexpr int SLOT_STEP = 18;
    }

    InventoryMenu::InventoryMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory) {

        // Slot order must match MC's InventoryMenu exactly — the wire protocol,
        // the save format and PlayerSession's remote-slot diff all assume menu
        // index == Game::Inventory index.

        // 0 — crafting result. Not rendered (IsActive false).
        AddSlot(std::make_unique<NoPlaceSlot>(playerInventory, 0, -1, -1));

        // 1..4 — 2x2 crafting grid. Not rendered.
        for (int i = 0; i < Inventory::CRAFT_GRID_SIZE; ++i) {
            AddSlot(std::make_unique<NoPlaceSlot>(
                playerInventory, Inventory::CRAFT_GRID_BEGIN + i, -1, -1));
        }

        // 5..8 — armor (helmet, chest, legs, boots), laid out two columns of two.
        for (int i = 0; i < Inventory::ARMOR_SIZE; ++i) {
            const int col = i / 2;
            const int row = i % 2;
            AddSlot(std::make_unique<ArmorSlot>(
                playerInventory, Inventory::ARMOR_BEGIN + i,
                54 + col * 54, 6 + row * 27));
        }

        // 9..35 — main storage, 3 rows of 9.
        for (int i = 0; i < Inventory::MAIN_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(
                playerInventory, Inventory::MAIN_BEGIN + i,
                9 + (i % 9) * SLOT_STEP, 54 + (i / 9) * SLOT_STEP));
        }

        // 36..44 — hotbar.
        for (int i = 0; i < Inventory::HOTBAR_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(
                playerInventory, Inventory::HOTBAR_BEGIN + i,
                9 + i * SLOT_STEP, 112));
        }

        // 45 — offhand. MC: CreativeModeInventoryScreen.selectTab(INVENTORY)
        // lines 537-539 → (35, 20).
        AddSlot(std::make_unique<Slot>(playerInventory, Inventory::OFFHAND_BEGIN, 35, 20));
    }

    // Verbatim port of InventoryMenu.quickMoveStack (InventoryMenu.java:95-153).
    // The destination ranges are MC's: main and hotbar feed each other, and
    // every restricted source (result, craft grid, armor) targets 9..45 as ONE
    // contiguous range. The old hand-rolled version tried main, then hotbar only
    // if main had moved nothing at all, so a partial move into main could strand
    // the remainder.
    void InventoryMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;

        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;

        // MC's ranges, expressed against our identical slot layout.
        constexpr int MAIN_BEGIN   = Inventory::MAIN_BEGIN;                        // 9
        constexpr int HOTBAR_BEGIN = Inventory::HOTBAR_BEGIN;                      // 36
        constexpr int HOTBAR_END   = Inventory::HOTBAR_BEGIN + Inventory::HOTBAR_SIZE; // 45
        constexpr int STORAGE_END  = Inventory::TOTAL_SIZE;                        // 46 (through offhand)

        bool moved = false;

        // Equip priority — MC checks the item's equipment slot BEFORE the
        // generic main/hotbar routing, so shift-clicking a helmet in storage
        // puts it on your head when the armor slot is free.
        int equipTarget = -1;
        if (auto equippable = stack.get(DataComponents::EQUIPPABLE)) {
            const int idx = InventoryIndexFor(equippable->slot);
            if (idx >= 0 && IsValidSlotIndex(idx) && !GetSlot(idx).HasItem()) {
                equipTarget = idx;
            }
        }

        if (slotIndex == Inventory::CRAFT_RESULT_BEGIN
            || Inventory::IsCraftGridSlot(slotIndex)
            || Inventory::IsArmorSlot(slotIndex)) {
            // Restricted sources → the whole player storage range at once.
            moved = MoveItemStackTo(stack, MAIN_BEGIN, STORAGE_END, true, result);
        } else if (equipTarget >= 0) {
            moved = MoveItemStackTo(stack, equipTarget, equipTarget + 1, false, result);
        } else if (Inventory::IsMainSlot(slotIndex)) {
            moved = MoveItemStackTo(stack, HOTBAR_BEGIN, HOTBAR_END, false, result);
        } else if (Inventory::IsHotbarSlot(slotIndex)) {
            moved = MoveItemStackTo(stack, MAIN_BEGIN, HOTBAR_BEGIN, false, result);
        } else {
            // Offhand and anything else → main + hotbar.
            moved = MoveItemStackTo(stack, MAIN_BEGIN, HOTBAR_END, false, result);
        }

        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

    ContainerClickResult InventoryMenu::HandleCreativeQuickMove(const ItemStack& source) {
        ContainerClickResult result;
        if (source.itemId == Items::Air) return result;

        ItemStack stack = source;
        stack.count = ItemRegistry::Get(source.itemId).maxStackSize;

        // Hotbar first, then main — a creative shift-click is "give me this in
        // hand", so the hotbar is the useful destination.
        MoveItemStackTo(stack, Inventory::HOTBAR_BEGIN,
                        Inventory::HOTBAR_BEGIN + Inventory::HOTBAR_SIZE, false, result);
        if (!stack.IsEmpty()) {
            MoveItemStackTo(stack, Inventory::MAIN_BEGIN,
                            Inventory::MAIN_BEGIN + Inventory::MAIN_SIZE, false, result);
        }
        return result;
    }

} // namespace Game
