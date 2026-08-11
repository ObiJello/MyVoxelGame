// File: src/common/inventory/CraftingMenu.hpp
//
// Mirrors net.minecraft.world.inventory.CraftingMenu — a crafting table's menu.
//
// Slot order is MC's exactly (it is what travels on the wire):
//   0        crafting result
//   1..9     3x3 crafting grid
//   10..36   player main inventory (3 rows x 9)
//   37..45   player hotbar
// Note what is NOT here: armour and offhand. MC's addStandardInventorySlots
// only adds main + hotbar, so those slots are unreachable while the table is
// open — same as vanilla.
//
// The grid and the result square are OWNED by this menu (a table's grid is not
// part of the player's inventory), which is the first place menu index and
// player-inventory index stop being the same number. Everything that speaks
// menu indices — the wire protocol, PlayerSession's remote-slot diff, the
// screen's hit test — goes through Slot, so nothing else has to care.
#pragma once

#include "AbstractCraftingMenu.hpp"
#include "SimpleContainer.hpp"

namespace Game {

    class CraftingMenu : public AbstractCraftingMenu {
    public:
        static constexpr int RESULT_SLOT   = 0;
        static constexpr int GRID_BEGIN    = 1;
        static constexpr int GRID_SIZE     = 9;
        static constexpr int MAIN_BEGIN    = 10;
        static constexpr int MAIN_SIZE     = 27;
        static constexpr int HOTBAR_BEGIN  = 37;
        static constexpr int HOTBAR_SIZE   = 9;
        static constexpr int SLOT_COUNT    = 46;

        explicit CraftingMenu(Inventory* playerInventory);

        // Verbatim port of CraftingMenu.quickMoveStack (CraftingMenu.java:101-140).
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;

        int MenuIndexForInventorySlot(int inventoryIndex) const override;

    protected:
        // A creative shift-click from the search grid should reach the player's
        // pockets, not the table's grid — the base's whole-menu scan would fill
        // the 3x3 first because it comes earlier in slot order.
        ContainerClickResult HandleCreativeQuickMove(const ItemStack& source) override;

    private:
        // Declared BEFORE anything that binds to them, and populated through
        // Configure() in the constructor body: base-class construction runs
        // before these exist, so they cannot be passed up the initializer list.
        SimpleContainer m_craftSlots{GRID_SIZE};
        SimpleContainer m_resultSlots{1};
    };

} // namespace Game
