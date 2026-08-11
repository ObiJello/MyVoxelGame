// File: src/common/inventory/InventoryMenu.cpp
#include "InventoryMenu.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include <memory>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;
    }

    InventoryMenu::InventoryMenu(Inventory* playerInventory)
        : AbstractCraftingMenu(playerInventory, 2, 2) {

        // The 2x2 grid and its output live in the player's OWN inventory at
        // indices 0..4 — unlike MC, where they are a transient container. That
        // is what keeps menu index == inventory index for this menu, which the
        // whole slot-sync path is built around.
        Configure(playerInventory, Inventory::CRAFT_GRID_BEGIN,
                  playerInventory, Inventory::CRAFT_RESULT_BEGIN,
                  /*gridMenuBegin=*/Inventory::CRAFT_GRID_BEGIN,
                  /*resultMenuIndex=*/Inventory::CRAFT_RESULT_BEGIN);

        // Slot order must match MC's InventoryMenu exactly — the wire protocol,
        // the save format and PlayerSession's remote-slot diff all assume menu
        // index == Game::Inventory index.
        //
        // The GUI coordinates below are MC's, panel-relative to the 176x166
        // textures/gui/container/inventory.png that Render::InventoryScreen
        // draws. They used to be the CREATIVE tab's coordinates instead, from
        // back when the creative picker was the only screen we had. MC keeps
        // the survival numbers here and lets CreativeModeInventoryScreen
        // re-position the same slots for its own panel
        // (CreativeModeInventoryScreen.selectTab, the SlotWrapper loop) — so
        // that is where our creative overrides live too.

        // 0 — crafting result (InventoryMenu.java:46 → addResultSlot(154, 28)).
        AddSlot(std::make_unique<CraftingResultSlot>(
            this, playerInventory, Inventory::CRAFT_RESULT_BEGIN, 154, 28));

        // 1..4 — 2x2 crafting grid (InventoryMenu.java:47 →
        // addCraftingGridSlots(98, 18), which walks row-major over a 2x2).
        for (int i = 0; i < Inventory::CRAFT_GRID_SIZE; ++i) {
            const int col = i % 2;
            const int row = i / 2;
            AddSlot(std::make_unique<Slot>(
                playerInventory, Inventory::CRAFT_GRID_BEGIN + i,
                98 + col * SLOT_STEP, 18 + row * SLOT_STEP));
        }

        // 5..8 — armor (helmet, chest, legs, boots) in one column
        // (InventoryMenu.java:52 → (8, 8 + i * 18)), each carrying the
        // empty-slot silhouette from InventoryMenu.EMPTY_ARMOR_SLOT_*.
        static const char* kArmorIcons[Inventory::ARMOR_SIZE] = {
            "container/slot/helmet",
            "container/slot/chestplate",
            "container/slot/leggings",
            "container/slot/boots",
        };
        for (int i = 0; i < Inventory::ARMOR_SIZE; ++i) {
            AddSlot(std::make_unique<ArmorSlot>(
                playerInventory, Inventory::ARMOR_BEGIN + i, 8, 8 + i * SLOT_STEP))
                .noItemIcon = kArmorIcons[i];
        }

        // 9..35 — main storage, 3 rows of 9 (InventoryMenu.java:55 →
        // addStandardInventorySlots(8, 84)).
        for (int i = 0; i < Inventory::MAIN_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(
                playerInventory, Inventory::MAIN_BEGIN + i,
                8 + (i % 9) * SLOT_STEP, 84 + (i / 9) * SLOT_STEP));
        }

        // 36..44 — hotbar. Same helper, offset 58px below the main rows
        // (AbstractContainerMenu.addStandardInventorySlots: top + 58).
        for (int i = 0; i < Inventory::HOTBAR_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(
                playerInventory, Inventory::HOTBAR_BEGIN + i, 8 + i * SLOT_STEP, 84 + 58));
        }

        // 45 — offhand (InventoryMenu.java:56 → (77, 62)), with
        // EMPTY_ARMOR_SLOT_SHIELD as its placeholder.
        AddSlot(std::make_unique<Slot>(playerInventory, Inventory::OFFHAND_BEGIN, 77, 62))
            .noItemIcon = "container/slot/shield";
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

        if (slotIndex == Inventory::CRAFT_RESULT_BEGIN) {
            // A crafted stack fills from the BACK (hotbar first) — MC line 104.
            moved = MoveItemStackTo(stack, MAIN_BEGIN, HOTBAR_END, true, result);
        } else if (Inventory::IsCraftGridSlot(slotIndex)
                || Inventory::IsArmorSlot(slotIndex)) {
            // Restricted sources → main + hotbar, front to back (MC lines 110/114).
            moved = MoveItemStackTo(stack, MAIN_BEGIN, HOTBAR_END, false, result);
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
