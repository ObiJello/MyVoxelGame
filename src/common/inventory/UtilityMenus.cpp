// File: src/common/inventory/UtilityMenus.cpp
#include "UtilityMenus.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"   // Items::EnchantedBook, Items::Book
#include "common/world/enchantment/EnchantmentDefinitions.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/map/MapItem.hpp"
#include "common/world/banner/BannerPatterns.hpp"
#include "common/entity/DyeColorUtil.hpp"
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

        // CartographyTableMenu's two input slots: the map square takes only
        // a map with an id, the other paper, a blank map or a glass pane.
        class CartographyInputSlot : public Slot {
        public:
            CartographyInputSlot(IContainer* container, int index, int x, int y)
                : Slot(container, index, x, y) {}
            bool MayPlace(const ItemStack& stack) const override {
                if (containerSlot == CartographyTableMenu::MAP_SLOT) {
                    return stack.get(DataComponents::MAP_ID).has_value();
                }
                return stack.itemId == Items::Paper || stack.itemId == Items::Map ||
                       stack.itemId == ItemRegistry::FromBlock(BlockID::GlassPane);
            }
        };

        // The result square: taking it runs onCraftedBy on what is taken
        // (the LOCK / SCALE post-process), then the menu's onTake.
        class CartographyResultSlot : public Slot {
        public:
            CartographyResultSlot(ItemCombinerMenu& menu, IContainer* container, int x, int y)
                : Slot(container, 0, x, y), m_menu(menu) {}
            bool MayPlace(const ItemStack& /*stack*/) const override { return false; }
            ItemStack Remove(int amount) override {
                ItemStack taken = Slot::Remove(amount);
                MapItemBridge::OnCraftedPostProcess(taken);
                return taken;
            }
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
        AddSlot(std::make_unique<CartographyInputSlot>(&m_inputs, MAP_SLOT, 15, 15));
        AddSlot(std::make_unique<CartographyInputSlot>(&m_inputs, ADDITIONAL_SLOT, 15, 52));
        AddSlot(std::make_unique<CartographyResultSlot>(*this, &m_result, 145, 39));
    }

    void CartographyTableMenu::SlotsChanged(ContainerClickResult& result) {
        // MC slotsChanged: with a result showing and an input gone, the
        // result goes; with both inputs, setupResultSlot.
        const bool haveBoth = !Input(MAP_SLOT).IsEmpty() && !Input(ADDITIONAL_SLOT).IsEmpty();
        if (!Result().IsEmpty() && !haveBoth) {
            SetResult(ItemStack{});
        } else if (haveBoth) {
            ComputeResult();
        }
        MarkChanged(result, ResultSlotIndex());
    }

    void CartographyTableMenu::ComputeResult() {
        // MC setupResultSlot.
        const ItemStack& map = Input(MAP_SLOT);
        const ItemStack& additional = Input(ADDITIONAL_SLOT);
        if (map.IsEmpty() || additional.IsEmpty()) return;
        // MapItem.getSavedData(map, level): nothing changes without data.
        const std::optional<int> scale = MapItemBridge::MapScale(map);
        if (!scale) return;
        const bool locked = MapItemBridge::MapLocked(map);

        ItemStack out = map;
        out.count = 1;
        if (additional.itemId == Items::Paper && map.itemId == Items::FilledMap &&   // #extendable_maps
            !locked && *scale < 4) {
            out.components.set(DataComponents::MAP_POST_PROCESSING, Maps::MapPostProcessing::Scale);
        } else if (additional.itemId == ItemRegistry::FromBlock(BlockID::GlassPane) && !locked) {
            out.components.set(DataComponents::MAP_POST_PROCESSING, Maps::MapPostProcessing::Lock);
        } else if (additional.itemId == Items::Map) {
            out.count = 2;
        } else {
            SetResult(ItemStack{});
            return;
        }
        if (!ItemStacksMatch(out, Result())) SetResult(out);
    }

    void CartographyTableMenu::OnTakeResult(const ItemStack& /*taken*/, ContainerClickResult& result) {
        // The result slot's onTake: one of each input, then the take sound
        // (the session's, at the table).
        for (int i : {MAP_SLOT, ADDITIONAL_SLOT}) {
            ItemStack& input = Input(i);
            if (input.IsEmpty()) continue;
            if (--input.count <= 0) input.Clear();
            MarkChanged(result, i);
        }
        result.cartographyUsed = true;
        MarkChanged(result, ResultSlotIndex());
    }

    void CartographyTableMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const int mainBegin = RESULT_SLOT + 1;           // 3
        const int hotbarBegin = mainBegin + 27;          // 30
        const int playerEnd = SlotCount();               // 39

        bool moved = false;
        if (slotIndex == RESULT_SLOT) {
            // stack.getItem().onCraftedBy(stack, player) before the move.
            MapItemBridge::OnCraftedPostProcess(stack);
            moved = MoveItemStackTo(stack, mainBegin, playerEnd, true, result);
        } else if (slotIndex == MAP_SLOT || slotIndex == ADDITIONAL_SLOT) {
            moved = MoveItemStackTo(stack, mainBegin, playerEnd, false, result);
        } else if (stack.get(DataComponents::MAP_ID)) {
            moved = MoveItemStackTo(stack, MAP_SLOT, MAP_SLOT + 1, false, result);
        } else if (stack.itemId == Items::Paper || stack.itemId == Items::Map ||
                   stack.itemId == ItemRegistry::FromBlock(BlockID::GlassPane)) {
            moved = MoveItemStackTo(stack, ADDITIONAL_SLOT, ADDITIONAL_SLOT + 1, false, result);
        } else if (slotIndex >= mainBegin && slotIndex < hotbarBegin) {
            moved = MoveItemStackTo(stack, hotbarBegin, playerEnd, false, result);
        } else if (slotIndex >= hotbarBegin && slotIndex < playerEnd) {
            moved = MoveItemStackTo(stack, mainBegin, hotbarBegin, false, result);
        }

        if (!moved || stack.count == original.count) return;
        slot.SetChanged();
        MarkChanged(result, slotIndex);
        // As ItemCombinerMenu: the caller's onTake consumes the inputs before
        // the result square is recomputed.
        MarkChanged(result, ResultSlotIndex());
    }

    // ── Loom ──────────────────────────────────────────────────────────────
    namespace {
        class LoomInputSlot : public Slot {
        public:
            LoomInputSlot(IContainer* container, int index, int x, int y) : Slot(container, index, x, y) {}
            bool MayPlace(const ItemStack& stack) const override {
                switch (containerSlot) {
                    case LoomMenu::BANNER_SLOT:  return LoomMenu::IsBannerItem(stack);
                    case LoomMenu::DYE_SLOT:     return LoomMenu::IsDyeItem(stack);
                    case LoomMenu::PATTERN_SLOT: return LoomMenu::IsPatternItem(stack);
                    default:                     return false;
                }
            }
        };
    } // namespace

    LoomMenu::LoomMenu(Inventory* playerInventory)
        : ItemCombinerMenu(playerInventory, 3, 84) {
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
        SetData(DATA_SELECTED, -1);
        PlaceInputSlots();
        FinishLayout(playerInventory);
    }

    void LoomMenu::PlaceInputSlots() {
        // MC LoomMenu: banner (13,26), dye (33,26), pattern (23,45), result (143,57).
        AddSlot(std::make_unique<LoomInputSlot>(&m_inputs, BANNER_SLOT, 13, 26));
        AddSlot(std::make_unique<LoomInputSlot>(&m_inputs, DYE_SLOT, 33, 26));
        AddSlot(std::make_unique<LoomInputSlot>(&m_inputs, PATTERN_SLOT, 23, 45));
        AddResultSlot(143, 57);
    }

    bool LoomMenu::IsBannerItem(const ItemStack& stack) {
        // BannerItem: the sixteen standing banners' items.
        if (stack.IsEmpty() || !ItemRegistry::IsBlockItem(stack.itemId)) return false;
        const std::string_view slug = ItemRegistry::Slug(stack.itemId);
        constexpr std::string_view kSuffix = "_banner";
        return slug.size() > kSuffix.size() && slug.substr(slug.size() - kSuffix.size()) == kSuffix &&
               slug.find("_wall_banner") == std::string_view::npos;
    }

    bool LoomMenu::IsDyeItem(const ItemStack& stack) {
        // #loom_dyes with a DYE: the sixteen dyes.
        return !stack.IsEmpty() && DyeColorOfItem(stack.itemId) >= 0;
    }

    bool LoomMenu::IsPatternItem(const ItemStack& stack) {
        // #loom_patterns with PROVIDES_BANNER_PATTERNS.
        if (stack.IsEmpty()) return false;
        const std::string tag = BannerPatterns::ProvidedTagOf(std::string(ItemRegistry::Slug(stack.itemId)));
        return !tag.empty() && !BannerPatterns::Resolve(tag).empty();
    }

    std::vector<std::string> LoomMenu::SelectablePatternsFor(const ItemStack& patternStack) const {
        if (patternStack.IsEmpty()) return BannerPatterns::Resolve("#minecraft:no_item_required");
        const std::string tag = BannerPatterns::ProvidedTagOf(std::string(ItemRegistry::Slug(patternStack.itemId)));
        return tag.empty() ? std::vector<std::string>{} : BannerPatterns::Resolve(tag);
    }

    void LoomMenu::SetupResultSlot(const std::string& pattern) {
        const ItemStack& banner = Input(BANNER_SLOT);
        const ItemStack& dye = Input(DYE_SLOT);
        ItemStack result;
        if (!banner.IsEmpty() && !dye.IsEmpty()) {
            const int color = DyeColorOfItem(dye.itemId);
            if (color >= 0) {
                result = banner;
                result.count = 1;
                BannerPatternLayers layers = result.get(DataComponents::BANNER_PATTERNS).value_or(BannerPatternLayers{});
                layers.layers.push_back(BannerPatternLayer{pattern, static_cast<uint8_t>(color)});
                result.components.set(DataComponents::BANNER_PATTERNS, std::move(layers));
            }
        }
        if (!ItemStacksMatch(result, Result())) SetResult(result);
    }

    void LoomMenu::ComputeResult() {
        // MC LoomMenu.slotsChanged.
        const ItemStack& banner = Input(BANNER_SLOT);
        const ItemStack& dye = Input(DYE_SLOT);
        if (banner.IsEmpty() || dye.IsEmpty()) {
            SetResult(ItemStack{});
            m_selectablePatterns.clear();
            SetData(DATA_SELECTED, -1);
            return;
        }
        const int selected = GetData(DATA_SELECTED);
        const bool validIndex = selected >= 0 && selected < static_cast<int>(m_selectablePatterns.size());
        const std::vector<std::string> previous = m_selectablePatterns;
        m_selectablePatterns = SelectablePatternsFor(Input(PATTERN_SLOT));
        std::optional<std::string> toDisplay;
        if (m_selectablePatterns.size() == 1) {
            SetData(DATA_SELECTED, 0);
            toDisplay = m_selectablePatterns[0];
        } else if (!validIndex) {
            SetData(DATA_SELECTED, -1);
        } else {
            const std::string& value = previous[static_cast<size_t>(selected)];
            const auto it = std::find(m_selectablePatterns.begin(), m_selectablePatterns.end(), value);
            if (it != m_selectablePatterns.end()) {
                toDisplay = value;
                SetData(DATA_SELECTED, static_cast<int>(it - m_selectablePatterns.begin()));
            } else {
                SetData(DATA_SELECTED, -1);
            }
        }
        if (toDisplay) {
            const auto layers = banner.get(DataComponents::BANNER_PATTERNS);
            const bool hasMaxPatterns = layers && static_cast<int>(layers->layers.size()) >= MAX_PATTERNS;
            if (hasMaxPatterns) {
                SetData(DATA_SELECTED, -1);
                SetResult(ItemStack{});
            } else {
                SetupResultSlot(*toDisplay);
            }
        } else {
            SetResult(ItemStack{});
        }
    }

    bool LoomMenu::ClickMenuButton(int buttonId, bool /*mayBuild*/, ContainerClickResult& result) {
        if (buttonId < 0 || buttonId >= static_cast<int>(m_selectablePatterns.size())) return false;
        SetData(DATA_SELECTED, buttonId);
        SetupResultSlot(m_selectablePatterns[static_cast<size_t>(buttonId)]);
        MarkChanged(result, RESULT_SLOT);
        return true;
    }

    void LoomMenu::OnTakeResult(const ItemStack& /*taken*/, ContainerClickResult& result) {
        // The result slot's onTake: one banner and one dye used; the pattern
        // item stays. Without both the selection resets. The session plays
        // UI_LOOM_TAKE_RESULT at the loom, once per game tick.
        ItemStack& banner = Input(BANNER_SLOT);
        ItemStack& dye = Input(DYE_SLOT);
        if (!banner.IsEmpty() && --banner.count <= 0) banner.Clear();
        if (!dye.IsEmpty() && --dye.count <= 0) dye.Clear();
        if (banner.IsEmpty() || dye.IsEmpty()) SetData(DATA_SELECTED, -1);
        MarkChanged(result, BANNER_SLOT);
        MarkChanged(result, DYE_SLOT);
        MarkChanged(result, RESULT_SLOT);
        result.loomUsed = true;
    }

    void LoomMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        // MC LoomMenu.quickMoveStack.
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        constexpr int kInvStart = 4, kInvEnd = 31, kUseRowStart = 31, kUseRowEnd = 40;

        bool moved = false;
        if (slotIndex == RESULT_SLOT) {
            moved = MoveItemStackTo(stack, kInvStart, kUseRowEnd, true, result);
        } else if (slotIndex != DYE_SLOT && slotIndex != BANNER_SLOT && slotIndex != PATTERN_SLOT) {
            if (IsBannerItem(stack)) {
                moved = MoveItemStackTo(stack, BANNER_SLOT, BANNER_SLOT + 1, false, result);
            } else if (IsDyeItem(stack)) {
                moved = MoveItemStackTo(stack, DYE_SLOT, DYE_SLOT + 1, false, result);
            } else if (IsPatternItem(stack)) {
                moved = MoveItemStackTo(stack, PATTERN_SLOT, PATTERN_SLOT + 1, false, result);
            } else if (slotIndex >= kInvStart && slotIndex < kInvEnd) {
                moved = MoveItemStackTo(stack, kUseRowStart, kUseRowEnd, false, result);
            } else if (slotIndex >= kUseRowStart && slotIndex < kUseRowEnd) {
                moved = MoveItemStackTo(stack, kInvStart, kInvEnd, false, result);
            }
        } else {
            moved = MoveItemStackTo(stack, kInvStart, kUseRowEnd, false, result);
        }

        if (!moved || stack.count == original.count) return;
        slot.SetChanged();
        MarkChanged(result, slotIndex);
        MarkChanged(result, ResultSlotIndex());
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
