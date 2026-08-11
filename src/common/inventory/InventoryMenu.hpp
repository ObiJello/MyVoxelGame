// File: src/common/inventory/InventoryMenu.hpp
//
// Mirrors net.minecraft.world.inventory.InventoryMenu — the player's own
// 46-slot menu, always open behind whatever container menu is on top.
//
// Slot order matches MC's exactly, which is also Game::Inventory's storage
// order (Inventory.hpp), so menu index == container index for this menu:
//   0       crafting result
//   1..4    2x2 crafting grid
//   5..8    armor (helmet, chest, legs, boots)
//   9..35   main inventory (3 rows x 9)
//   36..44  hotbar
//   45      offhand
//
// Slot::x / Slot::y carry MC's SURVIVAL panel coordinates (the 176x166
// textures/gui/container/inventory.png). Screens read those rather than keeping
// their own table, so layout and click behaviour can't drift apart.
// CreativeModeInventoryScreen overrides them for its own 195x136 panel, exactly
// as MC's CreativeModeInventoryScreen.selectTab re-wraps these slots.
#pragma once

#include "AbstractCraftingMenu.hpp"

namespace Game {

    class InventoryMenu : public AbstractCraftingMenu {
    public:
        explicit InventoryMenu(Inventory* playerInventory);

        // Verbatim port of InventoryMenu.quickMoveStack (InventoryMenu.java:95-153).
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;

    protected:
        // Creative shift-click from the search grid fills the HOTBAR first,
        // then main storage — the base class's generic whole-menu scan would
        // fill main first (slot order), which is not what a creative player
        // wants from a shift-click.
        ContainerClickResult HandleCreativeQuickMove(const ItemStack& source) override;
    };

} // namespace Game
