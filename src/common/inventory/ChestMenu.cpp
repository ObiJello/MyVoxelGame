// File: src/common/inventory/ChestMenu.cpp
#include "ChestMenu.hpp"
#include <memory>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;
    }

    ChestMenu::ChestMenu(Inventory* playerInventory, IContainer* container,
                         int rows, int columns)
        : AbstractContainerMenu(playerInventory), m_rows(rows), m_columns(columns) {
        BuildSlots(playerInventory, container);
    }

    ChestMenu::ChestMenu(Inventory* playerInventory, int rows, int columns)
        : AbstractContainerMenu(playerInventory), m_rows(rows), m_columns(columns),
          m_ownedContainer(std::make_unique<SimpleContainer>(rows * columns)) {
        BuildSlots(playerInventory, m_ownedContainer.get());
    }

    ChestMenu::ChestMenu(Inventory* playerInventory, std::unique_ptr<IContainer> container,
                         int rows, int columns)
        : AbstractContainerMenu(playerInventory), m_rows(rows), m_columns(columns),
          m_ownedContainer(std::move(container)) {
        BuildSlots(playerInventory, m_ownedContainer.get());
    }

    void ChestMenu::BuildSlots(Inventory* playerInventory, IContainer* container) {
        // GUI coordinates are MC's ChestMenu (ChestMenu.java:70-84), panel-
        // relative. The player half sits `18*rows` lower than the 3-row case,
        // which is exactly how MC derives `i` there:
        //     int i = (this.containerRows - 4) * 18;
        // ...for a 3-row chest that is -18, i.e. the player block starts at 85.
        //
        // The narrow grids place their block centred instead, matching MC:
        // DispenserMenu puts its 3x3 at (62,17) and its player block at 84;
        // HopperMenu puts its 5 slots at (44,20) with the player block at 51.
        int playerTop;
        int gridX, gridY;
        if (m_columns == COLUMNS) {
            playerTop = 103 + (m_rows - 4) * SLOT_STEP;
            gridX = 8;
            gridY = 18;
        } else if (m_rows == 1) {                 // hopper
            playerTop = 51;
            gridX = 8 + ((COLUMNS - m_columns) * SLOT_STEP) / 2;
            gridY = 20;
        } else {                                  // dispenser / dropper 3x3
            playerTop = 84;
            gridX = 8 + ((COLUMNS - m_columns) * SLOT_STEP) / 2;
            gridY = 17;
        }

        // 0 .. rows*columns-1 — the block's own container.
        for (int r = 0; r < m_rows; ++r) {
            for (int c = 0; c < m_columns; ++c) {
                AddSlot(std::make_unique<Slot>(container, r * m_columns + c,
                                               gridX + c * SLOT_STEP,
                                               gridY + r * SLOT_STEP));
            }
        }

        // Then the player's main rows and hotbar (addStandardInventorySlots).
        for (int i = 0; i < 27; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::MAIN_BEGIN + i,
                                           8 + (i % 9) * SLOT_STEP,
                                           playerTop + (i / 9) * SLOT_STEP));
        }
        for (int i = 0; i < 9; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::HOTBAR_BEGIN + i,
                                           8 + i * SLOT_STEP, playerTop + 58));
        }
    }

    int ChestMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        const int mainBegin = ContainerSlotCount();
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return mainBegin + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return mainBegin + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        // Armour, offhand and the player's own 2x2 are not part of this menu.
        return -1;
    }

    void ChestMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;

        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const int containerEnd = ContainerSlotCount();
        const int playerEnd    = SlotCount();

        // ChestMenu.quickMoveStack (ChestMenu.java:118-132) is the simplest of
        // the family: the two halves just swap into each other.
        bool moved;
        if (slotIndex < containerEnd) {
            moved = MoveItemStackTo(stack, containerEnd, playerEnd, true, result);
        } else {
            moved = MoveItemStackTo(stack, 0, containerEnd, false, result);
        }

        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

} // namespace Game
