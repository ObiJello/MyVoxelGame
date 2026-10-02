// File: src/common/inventory/CraftingMenu.cpp
#include "CraftingMenu.hpp"
#include "common/world/block/entity/CraftingTableBlockEntity.hpp"
#include "common/world/map/MapItem.hpp"
#include <memory>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;
    }

    CraftingMenu::CraftingMenu(Inventory* playerInventory)
        : AbstractCraftingMenu(playerInventory, 3, 3) {
        BuildSlots(playerInventory, &m_craftSlots, &m_resultSlots);
    }

    CraftingMenu::CraftingMenu(Inventory* playerInventory, CraftingTableBlockEntity* table)
        : AbstractCraftingMenu(playerInventory, 3, 3), m_sharedTable(table) {
        // The menu's own m_craftSlots / m_resultSlots go unused: every viewer's
        // slots point at the table's one grid and output, so a write through
        // any of them is the write every other viewer's diff reads.
        if (table) BuildSlots(playerInventory, &table->Grid(), &table->Result());
        else       BuildSlots(playerInventory, &m_craftSlots, &m_resultSlots);
    }

    void CraftingMenu::BuildSlots(Inventory* playerInventory, IContainer* craftSlots,
                                  IContainer* resultSlots) {
        Configure(craftSlots, 0, resultSlots, 0,
                  /*gridMenuBegin=*/GRID_BEGIN, /*resultMenuIndex=*/RESULT_SLOT);

        // GUI coordinates are MC's, panel-relative to the 176x166
        // textures/gui/container/crafting_table.png.

        // 0 — result (CraftingMenu.java:43 → addResultSlot(player, 124, 35)).
        AddSlot(std::make_unique<CraftingResultSlot>(this, resultSlots, 0, 124, 35));

        // 1..9 — the 3x3 grid (CraftingMenu.java:44 → addCraftingGridSlots(30, 17)).
        for (int i = 0; i < GRID_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(craftSlots, i,
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

    void CraftingMenu::SlotsChanged(ContainerClickResult& result) {
        AbstractCraftingMenu::SlotsChanged(result);
        // Slot writes already mark the table through its container; this
        // catches the in-place edits (a stack's count changed through the
        // slot's reference) that reach no SetItem.
        if (m_sharedTable) m_sharedTable->MarkDirty();
    }

    void CraftingMenu::Removed(ContainerClickResult& result) {
        // MC CraftingMenu.removed → clearContainer(player, craftSlots), which
        // the shared table skips: what is in the grid is the table's, and
        // other players may still be crafting with it.
        if (m_sharedTable) return;
        AbstractCraftingMenu::Removed(result);
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
            // MC: itemStack.getItem().onCraftedBy(itemStack, player) first —
            // a crafted map is locked / scaled before it moves.
            MapItemBridge::OnCraftedPostProcess(stack);
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
        stack.count = Game::GetMaxStackSize(source);

        // Hotbar first, then the main rows — same intent as InventoryMenu's
        // override: a creative shift-click means "give me this in hand".
        MoveItemStackTo(stack, HOTBAR_BEGIN, SLOT_COUNT, false, result);
        if (!stack.IsEmpty()) {
            MoveItemStackTo(stack, MAIN_BEGIN, HOTBAR_BEGIN, false, result);
        }
        return result;
    }

} // namespace Game
