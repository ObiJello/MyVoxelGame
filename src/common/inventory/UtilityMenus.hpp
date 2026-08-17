// File: src/common/inventory/UtilityMenus.hpp
//
// The block menus that compute an output from their inputs and store nothing:
// stonecutter, grindstone, cartography table, loom, smithing table and anvil.
//
// MC calls these ItemCombinerMenu (anvil / grindstone / smithing) and gives the
// stonecutter and loom their own classes. What they all share is the thing that
// matters here: no block entity, no persistence. Close the screen and the
// inputs come back to you — MC's ItemCombinerMenu.removed does exactly that,
// which is why none of these blocks can be used as storage.
//
// Slot order per menu is MC's, because it is the wire's.
#pragma once

#include "AbstractContainerMenu.hpp"
#include "SimpleContainer.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include <memory>
#include <string>
#include <vector>

namespace Game {

    // Shared scaffolding: owns an input container and a result container, hands
    // the inputs back on close, and lays out the player's rows.
    class ItemCombinerMenu : public AbstractContainerMenu {
    public:
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;
        void SlotsChanged(ContainerClickResult& result) override;
        void Removed(ContainerClickResult& result) override;

        int InputCount() const { return m_inputCount; }
        int ResultSlotIndex() const { return m_inputCount; }

    protected:
        ItemCombinerMenu(Inventory* playerInventory, int inputCount, int playerTop);

        // Recompute the result square from the inputs. Called by SlotsChanged.
        virtual void ComputeResult() = 0;

        // Place each input slot; called once from the constructor so subclasses
        // can use MC's coordinates.
        virtual void PlaceInputSlots() = 0;

        void AddInputSlot(int inputIndex, int x, int y);
        void AddResultSlot(int x, int y);
        void FinishLayout(Inventory* playerInventory);

        ItemStack&       Input(int i)       { return m_inputs.GetItem(i); }
        const ItemStack& Input(int i) const { return m_inputs.GetItem(i); }
        void SetResult(const ItemStack& stack) { m_result.SetItem(0, stack); }
        const ItemStack& Result() const { return m_result.GetItem(0); }

        int m_inputCount = 2;
        int m_playerTop  = 84;
        SimpleContainer m_inputs;
        SimpleContainer m_result{1};
    };

    // ── Stonecutter (MC StonecutterMenu) ──────────────────────────────────
    // Input → many possible results; the player picks one from a grid. The
    // selection is a data slot so both sides agree on which is showing.
    class StonecutterMenu : public ItemCombinerMenu {
    public:
        static constexpr int DATA_SELECTED = 0;
        static constexpr int DATA_COUNT    = 1;

        explicit StonecutterMenu(Inventory* playerInventory);

        // The results the current input can become, in table order.
        const std::vector<const StonecuttingRecipe*>& Options() const { return m_options; }
        int  SelectedIndex() const { return GetData(DATA_SELECTED); }
        // MC StonecutterMenu.clickMenuButton — picking an entry in the grid.
        bool SelectOption(int index);

    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;

    private:
        std::vector<const StonecuttingRecipe*> m_options;
    };

    // ── Grindstone (MC GrindstoneMenu) ────────────────────────────────────
    // Two inputs → one output with its enchantments stripped. Repair maths
    // needs item durability, which the item system does not carry yet, so this
    // does the disenchant half only — see ComputeResult.
    class GrindstoneMenu : public ItemCombinerMenu {
    public:
        explicit GrindstoneMenu(Inventory* playerInventory);
    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;
    };

    // ── Cartography table (MC CartographyTableMenu) ───────────────────────
    class CartographyTableMenu : public ItemCombinerMenu {
    public:
        explicit CartographyTableMenu(Inventory* playerInventory);
    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;
    };

    // ── Loom (MC LoomMenu) ────────────────────────────────────────────────
    class LoomMenu : public ItemCombinerMenu {
    public:
        explicit LoomMenu(Inventory* playerInventory);
    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;
    };

    // ── Smithing table (MC SmithingMenu) ──────────────────────────────────
    class SmithingMenu : public ItemCombinerMenu {
    public:
        explicit SmithingMenu(Inventory* playerInventory);
    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;
    };

    // ── Anvil (MC AnvilMenu) ──────────────────────────────────────────────
    // Rename works; repair and enchantment-combining need durability and an XP
    // system, neither of which exists. The level cost is computed and published
    // so the screen can show it, but nothing is charged — see ComputeResult.
    class AnvilMenu : public ItemCombinerMenu {
    public:
        static constexpr int DATA_COST  = 0;
        static constexpr int DATA_COUNT = 1;

        explicit AnvilMenu(Inventory* playerInventory);

        void SetItemName(const std::string& name);
        const std::string& ItemName() const { return m_itemName; }

    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;

    private:
        std::string m_itemName;
    };

} // namespace Game
