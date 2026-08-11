// File: src/common/inventory/CraftingMenu.cpp
#include "CraftingMenu.hpp"
#include <memory>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;
    }

    CraftingMenu::CraftingMenu(Inventory* playerInventory)
        : AbstractCraftingMenu(playerInventory, 3, 3) {

        Configure(&m_craftSlots, 0, &m_resultSlots, 0,
                  /*gridMenuBegin=*/GRID_BEGIN, /*resultMenuIndex=*/RESULT_SLOT);

        // GUI coordinates are MC's, panel-relative to the 176x166
        // textures/gui/container/crafting_table.png.

        // 0 — result (CraftingMenu.java:43 → addResultSlot(player, 124, 35)).
        AddSlot(std::make_unique<CraftingResultSlot>(this, &m_resultSlots, 0, 124, 35));

        // 1..9 — the 3x3 grid (CraftingMenu.java:44 → addCraftingGridSlots(30, 17)).
        for (int i = 0; i < GRID_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(&m_craftSlots, i,
                                           30 + (i % 3) * SLOT_STEP,
                                           17 + (i / 3) * SLOT_STEP));
        }

        // 10..36 / 37..45 — the player's main rows and hotbar
        // (CraftingMenu.java:45 → addStandardInventorySlots(inventory, 8, 84)).
        for (int i = 0; i < MAIN_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::MAIN_BEGIN + i,
                                           8 + (i % 9) * SLOT_STEP,
                                           84 + (i / 9) * SLOT_STEP));
        }
        for (int i = 0; i < HOTBAR_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::HOTBAR_BEGIN + i,
                                           8 + i * SLOT_STEP, 84 + 58));
        }
    }

    int CraftingMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return HOTBAR_BEGIN + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        // Armour, offhand and the player's own 2x2 are not part of this menu.
        return -1;
    }

    void CraftingMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;

        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        constexpr int PLAYER_END = SLOT_COUNT;   // 46, exclusive

        bool moved = false;
        if (slotIndex == RESULT_SLOT) {
            // A crafted stack fills from the BACK — hotbar first (MC line 106).
            moved = MoveItemStackTo(stack, MAIN_BEGIN, PLAYER_END, true, result);
        } else if (slotIndex >= MAIN_BEGIN && slotIndex < PLAYER_END) {
            // From the player's pockets, MC tries the crafting GRID first
            // (line 113) — shift-clicking ingredients loads the table — and
            // only then swaps between the main rows and the hotbar.
            moved = MoveItemStackTo(stack, GRID_BEGIN, GRID_BEGIN + GRID_SIZE, false, result);
            if (!moved) {
                moved = (slotIndex < HOTBAR_BEGIN)
                    ? MoveItemStackTo(stack, HOTBAR_BEGIN, PLAYER_END, false, result)
                    : MoveItemStackTo(stack, MAIN_BEGIN, HOTBAR_BEGIN, false, result);
            }
        } else {
            // Out of the grid, back into the player (line 123).
            moved = MoveItemStackTo(stack, MAIN_BEGIN, PLAYER_END, false, result);
        }

        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

    ContainerClickResult CraftingMenu::HandleCreativeQuickMove(const ItemStack& source) {
        ContainerClickResult result;
        if (source.itemId == Items::Air) return result;

        ItemStack stack = source;
        stack.count = ItemRegistry::Get(source.itemId).maxStackSize;

        // Hotbar first, then the main rows — same intent as InventoryMenu's
        // override: a creative shift-click means "give me this in hand".
        MoveItemStackTo(stack, HOTBAR_BEGIN, SLOT_COUNT, false, result);
        if (!stack.IsEmpty()) {
            MoveItemStackTo(stack, MAIN_BEGIN, HOTBAR_BEGIN, false, result);
        }
        return result;
    }

} // namespace Game
