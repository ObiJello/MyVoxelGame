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
#include <optional>
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
        // MC ItemCombinerMenu.canTakeItemForPickAll: never the result square.
        bool CanTakeItemForPickAll(int slotIndex) const override { return slotIndex != ResultSlotIndex(); }

        int InputCount() const { return m_inputCount; }
        int ResultSlotIndex() const { return m_inputCount; }

        // MC ItemCombinerMenu.mayPickup / onTake, reached through the result
        // slot. Taking the result is what commits the operation — consuming
        // the inputs — so a menu whose result is ever non-empty must override
        // OnTakeResult, or every pickup is a free copy.
        virtual bool MayPickupResult() const { return true; }
        virtual void OnTakeResult(const ItemStack& taken, ContainerClickResult& result) {
            (void)taken; (void)result;
        }

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

        // MC StonecutterMenu's result slot onTake: one input is used up.
        void OnTakeResult(const ItemStack& taken, ContainerClickResult& result) override;

    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;

    private:
        std::vector<const StonecuttingRecipe*> m_options;
    };

    // ── Grindstone (MC GrindstoneMenu) ────────────────────────────────────
    // Two inputs → one output: a single enchanted item with every non-curse
    // enchantment removed, or two of the same item combined — remaining
    // durability summed plus a 5% bonus, enchantments merged — then stripped
    // the same way. The result's REPAIR_COST is rebuilt from the curses left.
    // Taking it clears both inputs and pays experience for what was removed
    // (ContainerClickResult::grindstoneXp — the session awards it).
    class GrindstoneMenu : public ItemCombinerMenu {
    public:
        explicit GrindstoneMenu(Inventory* playerInventory);

        // MC GrindstoneMenu's result slot onTake: both inputs are used up,
        // the experience goes to the session.
        void OnTakeResult(const ItemStack& taken, ContainerClickResult& result) override;
        // MC GrindstoneMenu.quickMoveStack.
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;

        // MC GrindstoneMenu.computeResult / mergeItems / removeNonCursesFrom.
        static ItemStack ComputeGrindResult(const ItemStack& input, const ItemStack& additional);
        // The result slot's getExperienceFromItem: the minimum enchanting
        // cost of every non-curse enchantment on the item.
        static int ExperienceFromItem(const ItemStack& item);

    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;
    };

    // ── Cartography table (MC CartographyTableMenu) ───────────────────────
    // A filled map (slot 0) and paper / a blank map / a glass pane (slot 1):
    // zoom out (paper, an unlocked extendable map below scale 4), lock
    // (glass pane), or clone (blank map → two). The zoom and lock results
    // carry MAP_POST_PROCESSING; the result slot applies it as the map is
    // taken (MapItem.onCraftedPostProcess — a new map id). Taking consumes
    // one of each input and the session plays the take sound at the table.
    class CartographyTableMenu : public ItemCombinerMenu {
    public:
        static constexpr int MAP_SLOT        = 0;
        static constexpr int ADDITIONAL_SLOT = 1;
        static constexpr int RESULT_SLOT     = 2;

        explicit CartographyTableMenu(Inventory* playerInventory);

        void OnTakeResult(const ItemStack& taken, ContainerClickResult& result) override;
        // MC CartographyTableMenu.quickMoveStack.
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        // MC slotsChanged: a result with an input gone is cleared first.
        void SlotsChanged(ContainerClickResult& result) override;

    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;
    };

    // ── Loom (MC LoomMenu) ────────────────────────────────────────────────
    // A banner (slot 0), a dye (1) and optionally a banner pattern item (2):
    // the patterns on offer are the pattern item's PROVIDES_BANNER_PATTERNS
    // tag, or #no_item_required without one; clicking one (ClickMenuButton
    // with its index) previews the banner with that layer added in the
    // dye's colour — none once it already has 6 layers. Taking the result
    // uses one banner and one dye (the pattern item stays) and asks the
    // session for the take sound. The selection is data slot 0 (-1 = none),
    // shared by both sides; the list is recomputed on each side from the
    // same slots.
    class LoomMenu : public ItemCombinerMenu {
    public:
        static constexpr int BANNER_SLOT  = 0;
        static constexpr int DYE_SLOT     = 1;
        static constexpr int PATTERN_SLOT = 2;
        static constexpr int RESULT_SLOT  = 3;
        static constexpr int DATA_SELECTED = 0;
        static constexpr int DATA_COUNT    = 1;
        // BannerPatternLayers' loom limit (LoomMenu: hasMaxPatterns >= 6).
        static constexpr int MAX_PATTERNS = 6;

        explicit LoomMenu(Inventory* playerInventory);

        // LoomMenu.isPatternItem / isDyeItem / the banner slot's mayPlace.
        static bool IsBannerItem(const ItemStack& stack);
        static bool IsDyeItem(const ItemStack& stack);
        static bool IsPatternItem(const ItemStack& stack);

        const std::vector<std::string>& SelectablePatterns() const { return m_selectablePatterns; }
        int  SelectedPatternIndex() const { return GetData(DATA_SELECTED); }
        const ItemStack& BannerInput() const { return Input(BANNER_SLOT); }
        const ItemStack& DyeInput() const { return Input(DYE_SLOT); }
        const ItemStack& PatternInput() const { return Input(PATTERN_SLOT); }
        const ItemStack& ResultItem() const { return Result(); }

        // MC LoomMenu.clickMenuButton.
        bool ClickMenuButton(int buttonId, bool mayBuild, ContainerClickResult& result) override;
        void OnTakeResult(const ItemStack& taken, ContainerClickResult& result) override;
        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        // The list the screen shows, from the slots as they are now (the
        // client's copy is fed slot by slot and never recomputes itself):
        // empty without both a banner and a dye.
        std::vector<std::string> PatternsOnOffer() const {
            if (Input(BANNER_SLOT).IsEmpty() || Input(DYE_SLOT).IsEmpty()) return {};
            return SelectablePatternsFor(Input(PATTERN_SLOT));
        }

    protected:
        void ComputeResult() override;   // MC slotsChanged
        void PlaceInputSlots() override;

    private:
        std::vector<std::string> SelectablePatternsFor(const ItemStack& patternStack) const;
        void SetupResultSlot(const std::string& pattern);

        std::vector<std::string> m_selectablePatterns;
    };

    // ── Smithing table (MC SmithingMenu) ──────────────────────────────────
    class SmithingMenu : public ItemCombinerMenu {
    public:
        explicit SmithingMenu(Inventory* playerInventory);
        void OnTakeResult(const ItemStack& taken, ContainerClickResult& result) override;
    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;
    };

    // ── Anvil (MC AnvilMenu) ──────────────────────────────────────────────
    // Rename, repair with the item's material, combine two of the same item,
    // and apply / merge enchanted books — AnvilMenu.createResult line for line.
    // The level cost is data slot DATA_COST (the screen's "Enchantment Cost"
    // label); taking the result charges it through ContainerClickResult::
    // levelsSpent and flags anvilUsed, which the session turns into the
    // anvil's wear roll and use sound.
    //
    // The same code runs on both sides: the client predicts the result as the
    // player types a name (SetItemName, then RenameItemC2S carries the name to
    // the server's copy). Both sides need the player's level for mayPickup —
    // SetPlayerLevel, refreshed before every click.
    class AnvilMenu : public ItemCombinerMenu {
    public:
        static constexpr int DATA_COST  = 0;
        static constexpr int DATA_COUNT = 1;
        // MC AnvilMenu.MAX_NAME_LENGTH.
        static constexpr int MAX_NAME_LENGTH = 50;
        // At or above this cost the anvil refuses outside creative ("Too
        // Expensive!"); a rename alone is capped just below it.
        static constexpr int TOO_EXPENSIVE_COST = 40;

        explicit AnvilMenu(Inventory* playerInventory);

        // MC AnvilMenu.setItemName: validates (MC StringUtil.filterText, then
        // at most 50 characters), stamps / clears the name on a result already
        // showing, recomputes. False when the name was rejected or unchanged
        // — the client only sends a RenameItemC2S when this returns true.
        bool SetItemName(const std::string& name);
        std::string ItemName() const { return m_itemName.value_or(std::string{}); }
        int  GetCost() const { return GetData(DATA_COST); }

        void SetPlayerLevel(int level) { m_playerLevel = level; }

        bool MayPickupResult() const override;
        void OnTakeResult(const ItemStack& taken, ContainerClickResult& result) override;

        // MC AnvilMenu.calculateIncreasedRepairCost: prior-work penalty
        // doubling, 0 → 1 → 3 → 7 → 15 …
        static int CalculateIncreasedRepairCost(int baseCost);
        // MC StringUtil.filterText + the length check; nullopt when too long.
        static std::optional<std::string> ValidateName(const std::string& name);

    protected:
        void ComputeResult() override;
        void PlaceInputSlots() override;

    private:
        // MC AnvilMenu.itemName: null until the player first types — the
        // difference between "never named" and "cleared" matters to
        // createResult, which removes a CUSTOM_NAME only in the second case.
        std::optional<std::string> m_itemName;
        int  m_repairItemCountCost = 0;   // MC repairItemCountCost
        bool m_onlyRenaming = false;      // MC onlyRenaming
        int  m_playerLevel = 0;
    };

} // namespace Game
