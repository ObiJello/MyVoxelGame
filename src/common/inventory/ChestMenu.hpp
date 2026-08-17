// File: src/common/inventory/ChestMenu.hpp
//
// Mirrors net.minecraft.world.inventory.ChestMenu — the generic 9-wide storage
// menu behind chests, barrels, shulker boxes and ender chests.
//
// MC keys these on ROW COUNT, not on block: MenuType.GENERIC_9x3 backs a chest,
// a barrel and a shulker box alike, because the only thing that differs is how
// tall the container half is drawn. One class covers all of them here too, and
// the same shape (with a different row count) covers a double chest.
//
// Slot order — the wire's index space:
//   0 .. rows*9-1              the block's container
//   rows*9 .. rows*9+26        player main inventory (3 rows x 9)
//   rows*9+27 .. rows*9+35     player hotbar
// Armour and offhand are absent, matching MC's addStandardInventorySlots.
//
// The container is BORROWED, not owned: it is the block entity living in the
// chunk (BaseContainerBlockEntity), so writes go straight to the block and are
// saved and broadcast by the BE's own dirty bit. That is also why Removed()
// has nothing to hand back, unlike the crafting menus — nothing here is
// stranded when the screen closes, because none of it was ever the menu's.
#pragma once

#include "AbstractContainerMenu.hpp"
#include "SimpleContainer.hpp"
#include <memory>

namespace Game {

    class ChestMenu : public AbstractContainerMenu {
    public:
        // SERVER: borrow the block entity's container. It must outlive the menu
        // (PlayerSession closes the menu when the block goes away).
        //
        // `columns` defaults to 9, the chest family. MC splits the narrower
        // grids into their own classes — DispenserMenu (3x3) and HopperMenu
        // (5x1) — but the slot maths is character-for-character the same in all
        // three, differing only in the grid dimensions and the resulting panel
        // height, so one class carries them rather than three near-copies.
        ChestMenu(Inventory* playerInventory, IContainer* container,
                  int rows, int columns = COLUMNS);

        // CLIENT: no block entity exists on this side, so the menu owns a
        // scratch container of the right size and the server's slot sync fills
        // it. Slot writes land in it exactly as they would in a chest, which is
        // what makes the click prediction work identically on both sides.
        ChestMenu(Inventory* playerInventory, int rows, int columns = COLUMNS);

        // SERVER, double chest: the menu owns the CompoundContainer joining the
        // two block entities (the halves themselves stay owned by their chunk).
        // Owned because the compound view is created per-open and has nowhere
        // else to live.
        ChestMenu(Inventory* playerInventory, std::unique_ptr<IContainer> container,
                  int rows, int columns = COLUMNS);

        int Rows() const { return m_rows; }
        int Columns() const { return m_columns; }
        int ContainerSlotCount() const { return m_rows * m_columns; }

        // MC ChestMenu.quickMoveStack: container → player, player → container.
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;

        int MenuIndexForInventorySlot(int inventoryIndex) const override;

        static constexpr int COLUMNS = 9;

    private:
        void BuildSlots(Inventory* playerInventory, IContainer* container);

        int m_rows = 3;
        int m_columns = COLUMNS;
        // Set when the menu owns its container: a client-side scratch buffer,
        // or a server-side CompoundContainer for a double chest. Null when the
        // menu borrows a single block entity.
        std::unique_ptr<IContainer> m_ownedContainer;
    };

} // namespace Game
