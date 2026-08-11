// File: src/common/inventory/AbstractCraftingMenu.hpp
//
// Mirrors net.minecraft.world.inventory.AbstractCraftingMenu — the part shared
// by every menu with a crafting grid and an output square:
//
//   • InventoryMenu — the player's own 2x2, in the survival inventory screen
//   • CraftingMenu  — a crafting table's 3x3
//
// It owns the recipe lookup (SlotsChanged), the "taking the output consumes
// the grid" rule (CraftingResultSlot::OnTake) and the "give the grid back when
// the menu closes" rule (Removed). All three run identically on the client and
// the server, which is what makes the result square appear the instant an
// ingredient lands rather than a round trip later.
//
// Where the grid LIVES is left to the subclass, because the two disagree:
// InventoryMenu's 2x2 is part of the player's own 46-slot inventory (indices
// 0..4, which is how it reaches the save file), while CraftingMenu owns a
// private SimpleContainer that exists only while the table is open. Both are
// described to this class through Configure().
#pragma once

#include "AbstractContainerMenu.hpp"
#include "common/world/crafting/RecipeManager.hpp"

namespace Game {

    class AbstractCraftingMenu;

    // MC ResultSlot. Refuses inserts; taking from it is a craft, which consumes
    // one of each ingredient and leaves behind any crafting remainders.
    class CraftingResultSlot : public Slot {
    public:
        CraftingResultSlot(AbstractCraftingMenu* menu, IContainer* container,
                           int containerSlot, int x, int y)
            : Slot(container, containerSlot, x, y), m_menu(menu) {}

        bool MayPlace(const ItemStack& /*stack*/) const override { return false; }
        void OnTake(const ItemStack& taken, ContainerClickResult& result) override;

    private:
        AbstractCraftingMenu* m_menu;
    };

    class AbstractCraftingMenu : public AbstractContainerMenu {
    public:
        // MC AbstractContainerMenu.slotsChanged → CraftingMenu.slotChangedCraftingGrid:
        // re-run the recipe lookup and rewrite the output square.
        void SlotsChanged(ContainerClickResult& result) override;

        // MC CraftingMenu.removed → clearContainer(player, craftSlots).
        void Removed(ContainerClickResult& result) override;

        // Consume one craft's worth of ingredients. Public because
        // CraftingResultSlot calls it; not meant for anyone else.
        void OnResultTaken(ContainerClickResult& result);

    protected:
        AbstractCraftingMenu(Inventory* playerInventory, int gridWidth, int gridHeight)
            : AbstractContainerMenu(playerInventory),
              m_gridWidth(gridWidth), m_gridHeight(gridHeight) {}

        // Tell the base where the grid and output actually live. Must be called
        // from the SUBCLASS constructor body — a subclass that owns its
        // containers as members cannot pass them through the base's initializer
        // list, since base construction happens first.
        //
        //   craftContainer/craftBase  — the grid's backing store and the index
        //                               of its first cell inside it
        //   resultContainer/resultIdx — the output square's backing store
        //   gridMenuBegin/resultMenuIndex — the same two, as MENU indices, so
        //                               changed slots can be reported
        void Configure(IContainer* craftContainer, int craftBase,
                       IContainer* resultContainer, int resultIdx,
                       int gridMenuBegin, int resultMenuIndex);

        int GridWidth()  const { return m_gridWidth; }
        int GridHeight() const { return m_gridHeight; }
        int GridSize()   const { return m_gridWidth * m_gridHeight; }

    private:
        // Read the grid out as a trimmed CraftingInput (see RecipeManager),
        // reporting where the trimmed box sits inside the full grid so
        // OnResultTaken knows which real cells to consume.
        CraftingInput BuildInput(int& outLeft, int& outTop) const;

        int m_gridWidth  = 0;
        int m_gridHeight = 0;

        IContainer* m_craftContainer  = nullptr;
        int         m_craftBase       = 0;
        IContainer* m_resultContainer = nullptr;
        int         m_resultIdx       = 0;
        int         m_gridMenuBegin   = 0;
        int         m_resultMenuIndex = 0;
    };

} // namespace Game
