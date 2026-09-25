// File: src/common/inventory/UtilityMenus.cpp
#include "UtilityMenus.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"   // Items::EnchantedBook, Items::Book
#include "common/world/enchantment/EnchantmentDefinitions.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;

        // MC ItemCombinerMenu.createResultSlot: cannot be placed into, may be
        // taken when the menu says so, and taking it is what commits the
        // operation (the menu's OnTakeResult consumes the inputs).
        class CombinerResultSlot : public Slot {
        public:
            CombinerResultSlot(ItemCombinerMenu& menu, IContainer* container, int x, int y)
                : Slot(container, 0, x, y), m_menu(menu) {}
            bool MayPlace(const ItemStack& /*stack*/) const override { return false; }
            bool MayPickup() const override { return m_menu.MayPickupResult(); }
            void OnTake(const ItemStack& taken, ContainerClickResult& result) override {
                m_menu.OnTakeResult(taken, result);
            }
        private:
            ItemCombinerMenu& m_menu;
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
        AddSlot(std::make_unique<CombinerResultSlot>(*this, &m_result, x, y));
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
            // No ComputeResult here. MC's quickMoveStack leaves the result
            // square empty after moving it out: the caller's onTake consumes
            // the inputs FIRST and its slotsChanged recomputes after. Refilling
            // it here — before onTake — made the quick-move loop see an
            // unchanged slot and stop without ever consuming anything, which
            // was a free copy per shift-click.
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

    void StonecutterMenu::OnTakeResult(const ItemStack& /*taken*/, ContainerClickResult& result) {
        // MC StonecutterMenu's result slot onTake: inputSlot.remove(1), then
        // setupResultSlot — SlotsChanged recomputes from what is left.
        ItemStack& input = Input(0);
        if (input.IsEmpty()) return;
        if (--input.count <= 0) input.Clear();
        MarkChanged(result, 0);
        MarkChanged(result, ResultSlotIndex());
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
    namespace {
        // MC GrindstoneMenu's two input slots: `isDamageableItem() ||
        // EnchantmentHelper.hasAnyEnchantments(itemStack)`.
        class GrindstoneInputSlot : public Slot {
        public:
            using Slot::Slot;
            bool MayPlace(const ItemStack& stack) const override {
                return IsDamageableItem(stack) || EnchantmentHelper::HasAnyEnchantments(stack);
            }
        };

        // #minecraft:curse — what the grindstone leaves on an item.
        bool IsCurse(EnchantmentId id) {
            // #minecraft:curse, resolved once — the grindstone asks on every
            // recompute.
            static const std::vector<EnchantmentId> curses = EnchantmentDefinitions::ResolveTag("minecraft:curse");
            return std::find(curses.begin(), curses.end(), id) != curses.end();
        }

        // Set a component, or drop the stack's override when the value is
        // the item's own default (MC's patch map keeps no such entry).
        void SetIntComponentOrDefault(ItemStack& stack, const DataComponentType<int32_t>& type,
                                      int32_t value, int32_t absentDefault) {
            const int32_t prototype =
                ItemRegistry::Get(stack.itemId).defaultComponents.get(type).value_or(absentDefault);
            if (value == prototype) stack.components.remove(type);
            else                    stack.components.set(type, value);
        }

        // MC GrindstoneMenu.removeNonCursesFrom.
        ItemStack RemoveNonCursesFrom(ItemStack item) {
            EnchantmentHelper::UpdateEnchantments(item, [](ItemEnchantments& e) {
                std::vector<EnchantmentInstance> kept;
                for (const EnchantmentInstance& entry : e.entries) {
                    if (IsCurse(entry.id)) kept.push_back(entry);
                }
                e.entries = std::move(kept);   // still id-sorted: a subsequence
            });
            const ItemEnchantments remaining = EnchantmentHelper::GetEnchantmentsForCrafting(item);
            if (item.itemId == Items::EnchantedBook && remaining.IsEmpty()) {
                // item.transmuteCopy(BOOK): the book keeps the stack's patch
                // (a custom name, say).
                ItemStack book(Items::Book, item.count);
                book.components = item.components;
                item = std::move(book);
            }
            int repairCost = 0;
            for (size_t i = 0; i < remaining.size(); ++i) {
                repairCost = AnvilMenu::CalculateIncreasedRepairCost(repairCost);
            }
            SetIntComponentOrDefault(item, DataComponents::REPAIR_COST, repairCost, 0);
            return item;
        }

        // MC GrindstoneMenu.mergeEnchantsFrom: every enchantment of `source`
        // upgrades onto `target`, except a curse the target already has.
        void MergeEnchantsFrom(ItemStack& target, const ItemStack& source) {
            const ItemEnchantments from = EnchantmentHelper::GetEnchantmentsForCrafting(source);
            EnchantmentHelper::UpdateEnchantments(target, [&](ItemEnchantments& e) {
                for (const EnchantmentInstance& entry : from.entries) {
                    if (IsCurse(entry.id) && e.GetLevel(entry.id) != 0) continue;
                    e.Upgrade(entry.id, entry.level);
                }
            });
        }

        // MC GrindstoneMenu.mergeItems.
        ItemStack MergeItems(const ItemStack& input, const ItemStack& additional) {
            if (input.itemId != additional.itemId) return ItemStack{};
            const int durability = std::max(GetMaxDamage(input), GetMaxDamage(additional));
            const int remaining1 = GetMaxDamage(input) - GetDamageValue(input);
            const int remaining2 = GetMaxDamage(additional) - GetDamageValue(additional);
            const int remaining  = remaining1 + remaining2 + durability * 5 / 100;
            int count = 1;
            if (!IsDamageableItem(input)) {
                if (ItemRegistry::Get(input.itemId).maxStackSize < 2 || !ItemStacksMatch(input, additional)) {
                    return ItemStack{};
                }
                count = 2;
            }
            ItemStack newItem = input;
            newItem.count = count;
            if (IsDamageableItem(newItem)) {
                SetIntComponentOrDefault(newItem, DataComponents::MAX_DAMAGE, durability, 0);
                SetDamageValue(newItem, std::max(durability - remaining, 0));
            }
            MergeEnchantsFrom(newItem, additional);
            return RemoveNonCursesFrom(std::move(newItem));
        }
    } // namespace

    GrindstoneMenu::GrindstoneMenu(Inventory* playerInventory)
        : ItemCombinerMenu(playerInventory, 2, 84) {
        PlaceInputSlots();
        FinishLayout(playerInventory);
    }

    void GrindstoneMenu::PlaceInputSlots() {
        // MC GrindstoneMenu: (49,19), (49,40), result (129,34). The inputs
        // take only what the grindstone can work on.
        AddSlot(std::make_unique<GrindstoneInputSlot>(&m_inputs, 0, 49, 19));
        AddSlot(std::make_unique<GrindstoneInputSlot>(&m_inputs, 1, 49, 40));
        AddResultSlot(129, 34);
    }

    ItemStack GrindstoneMenu::ComputeGrindResult(const ItemStack& input, const ItemStack& additional) {
        // MC GrindstoneMenu.computeResult.
        if (input.IsEmpty() && additional.IsEmpty()) return ItemStack{};
        if (input.count > 1 || additional.count > 1) return ItemStack{};
        if (input.IsEmpty() || additional.IsEmpty()) {
            const ItemStack& item = !input.IsEmpty() ? input : additional;
            return EnchantmentHelper::HasAnyEnchantments(item) ? RemoveNonCursesFrom(item) : ItemStack{};
        }
        return MergeItems(input, additional);
    }

    void GrindstoneMenu::ComputeResult() {
        SetResult(ComputeGrindResult(Input(0), Input(1)));
    }

    int GrindstoneMenu::ExperienceFromItem(const ItemStack& item) {
        int amount = 0;
        for (const EnchantmentInstance& e : EnchantmentHelper::GetEnchantmentsForCrafting(item).entries) {
            if (!IsCurse(e.id)) amount += EnchantmentDefinitions::Get(e.id).minCost.Calculate(e.level);
        }
        return amount;
    }

    void GrindstoneMenu::OnTakeResult(const ItemStack& /*taken*/, ContainerClickResult& result) {
        // MC's result slot onTake: the experience (access.execute — the
        // session's), then both inputs are used up.
        result.grindstoneUsed = true;
        result.grindstoneXp   = ExperienceFromItem(Input(0)) + ExperienceFromItem(Input(1));
        Input(0).Clear();
        Input(1).Clear();
        MarkChanged(result, 0);
        MarkChanged(result, 1);
        MarkChanged(result, ResultSlotIndex());
    }

    void GrindstoneMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        // MC GrindstoneMenu.quickMoveStack: the result and the inputs go to
        // the player; from the player, into the inputs while one is free,
        // otherwise between the main rows and the hotbar.
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const int resultIndex = ResultSlotIndex();
        const int mainBegin   = resultIndex + 1;
        const int hotbarBegin = mainBegin + 27;
        const int playerEnd   = SlotCount();

        bool moved;
        if (slotIndex == resultIndex) {
            moved = MoveItemStackTo(stack, mainBegin, playerEnd, true, result);
        } else if (slotIndex < resultIndex) {
            moved = MoveItemStackTo(stack, mainBegin, playerEnd, false, result);
        } else if (!Input(0).IsEmpty() && !Input(1).IsEmpty()) {
            moved = slotIndex < hotbarBegin
                ? MoveItemStackTo(stack, hotbarBegin, playerEnd, false, result)
                : MoveItemStackTo(stack, mainBegin, hotbarBegin, false, result);
        } else {
            moved = MoveItemStackTo(stack, 0, resultIndex, false, result);
        }

        if (!moved || stack.count == original.count) return;
        slot.SetChanged();
        MarkChanged(result, slotIndex);
        // As ItemCombinerMenu: the result square is recomputed after the
        // caller's onTake has consumed the inputs, never before.
        MarkChanged(result, resultIndex);
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

    namespace {
        // MC StringUtil.isBlank: empty or whitespace only.
        bool IsBlank(const std::string& s) {
            return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
        }

        // MC StringUtil.filterText → isAllowedChatCharacter: drops the
        // section sign (formatting codes), control characters and DEL. UTF-8
        // aware so a multi-byte character is kept or dropped whole.
        std::string FilterText(const std::string& in) {
            std::string out;
            out.reserve(in.size());
            for (size_t i = 0; i < in.size();) {
                const unsigned char c = static_cast<unsigned char>(in[i]);
                const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
                const bool sectionSign = len == 2 && i + 1 < in.size() &&
                                         c == 0xC2 && static_cast<unsigned char>(in[i + 1]) == 0xA7;
                const bool control = len == 1 && (c < 0x20 || c == 0x7F);
                // A stray continuation byte or an invalid lead is not a
                // character at all — dropped, so the name stays valid UTF-8.
                bool malformed = len == 1 && c >= 0x80;
                for (size_t k = 1; !malformed && k < len && i + k < in.size(); ++k) {
                    malformed = (static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80;
                }
                if (malformed && len > 1) {
                    ++i;   // resync on the next byte
                    continue;
                }
                if (!sectionSign && !control && !malformed && i + len <= in.size()) out.append(in, i, len);
                i += len;
            }
            return out;
        }

        // Code points, not bytes — MC's String.length() for the 50 limit.
        size_t CodePointCount(const std::string& s) {
            size_t n = 0;
            for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
            return n;
        }
    }

    std::optional<std::string> AnvilMenu::ValidateName(const std::string& name) {
        std::string filtered = FilterText(name);
        if (CodePointCount(filtered) > static_cast<size_t>(MAX_NAME_LENGTH)) return std::nullopt;
        return filtered;
    }

    int AnvilMenu::CalculateIncreasedRepairCost(int baseCost) {
        return static_cast<int>(std::min<int64_t>(static_cast<int64_t>(baseCost) * 2 + 1,
                                                  std::numeric_limits<int32_t>::max()));
    }

    bool AnvilMenu::SetItemName(const std::string& name) {
        const std::optional<std::string> validated = ValidateName(name);
        if (!validated || (m_itemName && *validated == *m_itemName)) return false;
        m_itemName = *validated;
        // A result already showing takes the new name at once.
        ItemStack out = Result();
        if (!out.IsEmpty()) {
            if (IsBlank(*m_itemName)) out.components.remove(DataComponents::CUSTOM_NAME);
            else out.components.set(DataComponents::CUSTOM_NAME, *m_itemName);
            SetResult(out);
        }
        ComputeResult();
        return true;
    }

    bool AnvilMenu::MayPickupResult() const {
        // MC AnvilMenu.mayPickup: (infinite materials || level >= cost) && cost > 0.
        const int cost = GetCost();
        return (creative || m_playerLevel >= cost) && cost > 0;
    }

    void AnvilMenu::OnTakeResult(const ItemStack& /*taken*/, ContainerClickResult& result) {
        // MC AnvilMenu.onTake.
        if (!creative) result.levelsSpent += GetCost();

        if (m_repairItemCountCost > 0) {
            ItemStack& addition = Input(1);
            if (!addition.IsEmpty() && addition.count > m_repairItemCountCost) {
                addition.count -= m_repairItemCountCost;
            } else {
                addition.Clear();
            }
        } else if (!m_onlyRenaming) {
            Input(1).Clear();
        }
        SetData(DATA_COST, 0);
        Input(0).Clear();
        MarkChanged(result, 0);
        MarkChanged(result, 1);
        MarkChanged(result, ResultSlotIndex());
        // The wear roll and the use / break event: ContainerLevelAccess work,
        // the session's (ApplyMenuTakeCosts).
        result.anvilUsed = true;
    }

    void AnvilMenu::ComputeResult() {
        // MC AnvilMenu.createResult, statement for statement.
        const ItemStack& input = Input(0);
        m_onlyRenaming = false;
        SetData(DATA_COST, 1);
        int price = 0;
        int64_t tax = 0;
        int namingCost = 0;

        auto fail = [this]() {
            SetResult(ItemStack{});
            SetData(DATA_COST, 0);
        };

        if (input.IsEmpty() || !EnchantmentHelper::CanStoreEnchantments(input)) {
            fail();
            return;
        }

        ItemStack result = input;
        const ItemStack& addition = Input(1);
        ItemEnchantments enchantments = EnchantmentHelper::GetEnchantmentsForCrafting(result);
        tax += static_cast<int64_t>(input.get(DataComponents::REPAIR_COST).value_or(0)) +
               static_cast<int64_t>(addition.get(DataComponents::REPAIR_COST).value_or(0));
        m_repairItemCountCost = 0;

        if (!addition.IsEmpty()) {
            const bool usingBook = addition.get(DataComponents::STORED_ENCHANTMENTS).has_value();
            if (IsDamageableItem(result) && IsValidRepairItem(input, addition)) {
                // Repair with the item's material: each unit restores a
                // quarter of the durability, one level each.
                int repairAmount = std::min(GetDamageValue(result), GetMaxDamage(result) / 4);
                if (repairAmount <= 0) {
                    fail();
                    return;
                }
                int count = 0;
                for (; repairAmount > 0 && count < addition.count; ++count) {
                    SetDamageValue(result, GetDamageValue(result) - repairAmount);
                    ++price;
                    repairAmount = std::min(GetDamageValue(result), GetMaxDamage(result) / 4);
                }
                m_repairItemCountCost = count;
            } else {
                if (!usingBook && (result.itemId != addition.itemId || !IsDamageableItem(result))) {
                    fail();
                    return;
                }

                // Two of the same item: their remaining durability plus a 12%
                // bonus.
                if (IsDamageableItem(result) && !usingBook) {
                    const int remaining1 = GetMaxDamage(input) - GetDamageValue(input);
                    const int remaining2 = GetMaxDamage(addition) - GetDamageValue(addition);
                    const int additional = remaining2 + GetMaxDamage(result) * 12 / 100;
                    const int remaining = remaining1 + additional;
                    int resultDamage = GetMaxDamage(result) - remaining;
                    if (resultDamage < 0) resultDamage = 0;
                    if (resultDamage < GetDamageValue(result)) {
                        SetDamageValue(result, resultDamage);
                        price += 2;
                    }
                }

                const ItemEnchantments additional = EnchantmentHelper::GetEnchantmentsForCrafting(addition);
                bool anyCompatible = false;
                bool anyNotCompatible = false;
                for (const EnchantmentInstance& entry : additional.entries) {
                    const int current = enchantments.GetLevel(entry.id);
                    int level = entry.level;
                    level = current == level ? level + 1 : std::max(level, current);
                    const auto& definition = EnchantmentDefinitions::Get(entry.id);
                    bool compatible = EnchantmentDefinitions::IsSupportedItem(entry.id, input.itemId);
                    if (creative || input.itemId == Items::EnchantedBook) compatible = true;
                    for (const EnchantmentInstance& other : enchantments.entries) {
                        if (other.id != entry.id && !EnchantmentDefinitions::AreCompatible(entry.id, other.id)) {
                            compatible = false;
                            ++price;
                        }
                    }
                    if (!compatible) {
                        anyNotCompatible = true;
                    } else {
                        anyCompatible = true;
                        if (level > definition.maxLevel) level = definition.maxLevel;
                        enchantments.Set(entry.id, level);
                        int fee = definition.anvilCost;
                        if (usingBook) fee = std::max(1, fee / 2);
                        price += fee * level;
                        if (input.count > 1) price = 40;
                    }
                }
                if (anyNotCompatible && !anyCompatible) {
                    fail();
                    return;
                }
            }
        }

        if (m_itemName && !IsBlank(*m_itemName)) {
            if (*m_itemName != GetItemStackHoverName(input)) {
                namingCost = 1;
                price += namingCost;
                result.components.set(DataComponents::CUSTOM_NAME, *m_itemName);
            }
        } else if (input.components.has(DataComponents::CUSTOM_NAME)) {
            namingCost = 1;
            price += namingCost;
            result.components.remove(DataComponents::CUSTOM_NAME);
        }

        const int finalPrice = price <= 0
            ? 0
            : static_cast<int>(std::clamp<int64_t>(tax + price, 0, std::numeric_limits<int32_t>::max()));
        SetData(DATA_COST, finalPrice);
        if (price <= 0) result = ItemStack{};

        if (namingCost == price && namingCost > 0) {
            if (GetCost() >= TOO_EXPENSIVE_COST) SetData(DATA_COST, TOO_EXPENSIVE_COST - 1);
            m_onlyRenaming = true;
        }

        if (GetCost() >= TOO_EXPENSIVE_COST && !creative) result = ItemStack{};

        if (!result.IsEmpty()) {
            int baseCost = result.get(DataComponents::REPAIR_COST).value_or(0);
            baseCost = std::max(baseCost, addition.get(DataComponents::REPAIR_COST).value_or(0));
            if (namingCost != price || namingCost == 0) baseCost = CalculateIncreasedRepairCost(baseCost);
            // result.set(REPAIR_COST, baseCost): a value equal to the item's
            // default (0) is no patch, so a rename-only result keeps stacking.
            SetIntComponentOrDefault(result, DataComponents::REPAIR_COST, baseCost, 0);
            EnchantmentHelper::SetEnchantments(result, enchantments);
        }

        SetResult(result);
    }

} // namespace Game
