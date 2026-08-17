// File: src/common/inventory/UtilityMenus.cpp
#include "UtilityMenus.hpp"
#include "common/data/DataComponents.hpp"
#include <algorithm>
#include <memory>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;

        // MC ResultSlot: cannot be placed into. Taking from it is what commits
        // the operation, which each menu handles in its own SlotsChanged.
        class CombinerResultSlot : public Slot {
        public:
            using Slot::Slot;
            bool MayPlace(const ItemStack& /*stack*/) const override { return false; }
        };
    }

    // ── ItemCombinerMenu ──────────────────────────────────────────────────
    ItemCombinerMenu::ItemCombinerMenu(Inventory* playerInventory, int inputCount, int playerTop)
        : AbstractContainerMenu(playerInventory),
          m_inputCount(inputCount), m_playerTop(playerTop),
          m_inputs(inputCount) {}

    void ItemCombinerMenu::AddInputSlot(int inputIndex, int x, int y) {
        AddSlot(std::make_unique<Slot>(&m_inputs, inputIndex, x, y));
    }

    void ItemCombinerMenu::AddResultSlot(int x, int y) {
        AddSlot(std::make_unique<CombinerResultSlot>(&m_result, 0, x, y));
    }

    void ItemCombinerMenu::FinishLayout(Inventory* playerInventory) {
        for (int i = 0; i < 27; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::MAIN_BEGIN + i,
                                           8 + (i % 9) * SLOT_STEP,
                                           m_playerTop + (i / 9) * SLOT_STEP));
        }
        for (int i = 0; i < 9; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::HOTBAR_BEGIN + i,
                                           8 + i * SLOT_STEP, m_playerTop + 58));
        }
    }

    int ItemCombinerMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        const int mainBegin = m_inputCount + 1;   // inputs + result
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return mainBegin + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return mainBegin + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    void ItemCombinerMenu::SlotsChanged(ContainerClickResult& result) {
        ComputeResult();
        MarkChanged(result, ResultSlotIndex());
    }

    void ItemCombinerMenu::Removed(ContainerClickResult& result) {
        // MC ItemCombinerMenu.removed → clearContainer: the inputs are the
        // player's, and this block stores nothing, so they go back. Without
        // this a closed anvil would eat whatever was sitting in it.
        for (int i = 0; i < m_inputCount; ++i) {
            ItemStack& stack = m_inputs.GetItem(i);
            if (stack.IsEmpty()) continue;
            const int leftover = getInventory().AddStack(stack);
            if (leftover > 0) {
                // Inventory was full. These are the player's own items, so they
                // go into the world rather than being destroyed — the session
                // spawns whatever lands in extraDrops.
                ItemStack overflow = stack;
                overflow.count = leftover;
                result.extraDrops.push_back(overflow);
            }
            stack.Clear();
            MarkChanged(result, i);
        }
        // The result square is derived, never owned — dropping it is correct.
        m_result.SetItem(0, ItemStack{});
    }

    void ItemCombinerMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;

        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const int resultIndex = ResultSlotIndex();
        const int mainBegin   = resultIndex + 1;
        const int playerEnd   = SlotCount();

        bool moved = false;
        if (slotIndex == resultIndex) {
            moved = MoveItemStackTo(stack, mainBegin, playerEnd, true, result);
        } else if (slotIndex < resultIndex) {
            moved = MoveItemStackTo(stack, mainBegin, playerEnd, false, result);
        } else {
            // From the player: try the inputs, then swap rows/hotbar.
            moved = MoveItemStackTo(stack, 0, resultIndex, false, result);
            if (!moved) {
                moved = (slotIndex < mainBegin + 27)
                    ? MoveItemStackTo(stack, mainBegin + 27, playerEnd, false, result)
                    : MoveItemStackTo(stack, mainBegin, mainBegin + 27, false, result);
            }
        }

        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
            ComputeResult();
            MarkChanged(result, resultIndex);
        }
    }

    // ── Stonecutter ───────────────────────────────────────────────────────
    StonecutterMenu::StonecutterMenu(Inventory* playerInventory)
        : ItemCombinerMenu(playerInventory, 1, 84) {
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
        PlaceInputSlots();
        FinishLayout(playerInventory);
    }

    void StonecutterMenu::PlaceInputSlots() {
        // MC StonecutterMenu: input at (20,33), result at (143,33).
        AddInputSlot(0, 20, 33);
        AddResultSlot(143, 33);
    }

    bool StonecutterMenu::SelectOption(int index) {
        if (index < 0 || index >= static_cast<int>(m_options.size())) return false;
        SetData(DATA_SELECTED, index);
        ComputeResult();
        return true;
    }

    void StonecutterMenu::ComputeResult() {
        m_options = RecipeManager::FindStonecutting(Input(0));
        if (m_options.empty()) {
            SetData(DATA_SELECTED, 0);
            SetResult(ItemStack{});
            return;
        }
        // MC resets the selection when the input changes to something with a
        // different option list; clamping covers that and a stale index.
        int selected = SelectedIndex();
        if (selected < 0 || selected >= static_cast<int>(m_options.size())) {
            selected = 0;
            SetData(DATA_SELECTED, 0);
        }
        const StonecuttingRecipe* recipe = m_options[static_cast<size_t>(selected)];
        SetResult(ItemStack(recipe->resultItem, recipe->resultCount));
    }

    // ── Grindstone ────────────────────────────────────────────────────────
    GrindstoneMenu::GrindstoneMenu(Inventory* playerInventory)
        : ItemCombinerMenu(playerInventory, 2, 84) {
        PlaceInputSlots();
        FinishLayout(playerInventory);
    }

    void GrindstoneMenu::PlaceInputSlots() {
        // MC GrindstoneMenu: (49,19), (49,40), result (129,34).
        AddInputSlot(0, 49, 19);
        AddInputSlot(1, 49, 40);
        AddResultSlot(129, 34);
    }

    void GrindstoneMenu::ComputeResult() {
        // MC GrindstoneMenu.createResult has two halves: strip enchantments,
        // and repair by combining durability. Durability does not exist on our
        // ItemStacks (no DAMAGE/MAX_DAMAGE component is registered — see
        // ItemBehaviors.cpp:85), so only the disenchant half is meaningful.
        const ItemStack& top    = Input(0);
        const ItemStack& bottom = Input(1);
        if (top.IsEmpty() && bottom.IsEmpty()) { SetResult(ItemStack{}); return; }
        // Exactly one input: hand back a copy with its enchantments removed.
        if (top.IsEmpty() != bottom.IsEmpty()) {
            ItemStack out = top.IsEmpty() ? bottom : top;
            out.components.remove(DataComponents::STORED_ENCHANTMENTS);
            SetResult(out);
            return;
        }
        // Two of the same item would repair in vanilla; without durability
        // there is nothing to combine, so decline rather than silently eat one.
        SetResult(ItemStack{});
    }

    // ── Cartography table ─────────────────────────────────────────────────
    CartographyTableMenu::CartographyTableMenu(Inventory* playerInventory)
        : ItemCombinerMenu(playerInventory, 2, 84) {
        PlaceInputSlots();
        FinishLayout(playerInventory);
    }

    void CartographyTableMenu::PlaceInputSlots() {
        // MC CartographyTableMenu: map (15,15), paper (15,52), result (145,39).
        AddInputSlot(0, 15, 15);
        AddInputSlot(1, 15, 52);
        AddResultSlot(145, 39);
    }

    void CartographyTableMenu::ComputeResult() {
        // Every cartography operation (zoom out, lock, clone) works on filled
        // map data, which needs the MapItemSavedData system — not present. The
        // menu is here so the block opens and behaves like a container that
        // hands your items back; it produces nothing until maps exist.
        SetResult(ItemStack{});
    }

    // ── Loom ──────────────────────────────────────────────────────────────
    LoomMenu::LoomMenu(Inventory* playerInventory)
        : ItemCombinerMenu(playerInventory, 3, 84) {
        PlaceInputSlots();
        FinishLayout(playerInventory);
    }

    void LoomMenu::PlaceInputSlots() {
        // MC LoomMenu: banner (13,26), dye (33,26), pattern (23,45), result (143,58).
        AddInputSlot(0, 13, 26);
        AddInputSlot(1, 33, 26);
        AddInputSlot(2, 23, 45);
        AddResultSlot(143, 58);
    }

    void LoomMenu::ComputeResult() {
        // Applying a pattern writes a BANNER_PATTERNS component onto the
        // result. That component is not registered, so the loom opens and
        // returns its inputs but cannot yet weave.
        SetResult(ItemStack{});
    }

    // ── Smithing table ────────────────────────────────────────────────────
    SmithingMenu::SmithingMenu(Inventory* playerInventory)
        : ItemCombinerMenu(playerInventory, 3, 84) {
        PlaceInputSlots();
        FinishLayout(playerInventory);
    }

    void SmithingMenu::PlaceInputSlots() {
        // MC SmithingMenu: template (8,48), base (26,48), addition (44,48),
        // result (98,48).
        AddInputSlot(0, 8, 48);
        AddInputSlot(1, 26, 48);
        AddInputSlot(2, 44, 48);
        AddResultSlot(98, 48);
    }

    void SmithingMenu::ComputeResult() {
        // smithing_transform / smithing_trim are the two recipe types
        // gen_recipes.py still skips — they need armour trim components to
        // carry the result. Nothing to compute until those are baked.
        SetResult(ItemStack{});
    }

    // ── Anvil ─────────────────────────────────────────────────────────────
    AnvilMenu::AnvilMenu(Inventory* playerInventory)
        : ItemCombinerMenu(playerInventory, 2, 84) {
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
        PlaceInputSlots();
        FinishLayout(playerInventory);
    }

    void AnvilMenu::PlaceInputSlots() {
        // MC AnvilMenu: (27,47), (76,47), result (134,47).
        AddInputSlot(0, 27, 47);
        AddInputSlot(1, 76, 47);
        AddResultSlot(134, 47);
    }

    void AnvilMenu::SetItemName(const std::string& name) {
        m_itemName = name;
        ComputeResult();
    }

    void AnvilMenu::ComputeResult() {
        const ItemStack& left = Input(0);
        if (left.IsEmpty()) {
            SetResult(ItemStack{});
            SetData(DATA_COST, 0);
            return;
        }

        // Rename is the one AnvilMenu.createResult branch that works without
        // durability or XP: copy the left input and stamp a CUSTOM_NAME.
        ItemStack out = left;
        int cost = 0;
        if (!m_itemName.empty()) {
            out.components.set(DataComponents::CUSTOM_NAME, m_itemName);
            cost = 1;   // MC's flat rename cost
        }

        // Repair and enchantment-combining are the other two branches; both
        // need durability (no DAMAGE component) and both charge XP levels (no
        // experience system on ServerPlayer). The cost is published for the
        // screen to display, but nothing is deducted — flagged rather than
        // faked so the gap is obvious when XP lands.
        SetData(DATA_COST, cost);
        SetResult(out);
    }

} // namespace Game
