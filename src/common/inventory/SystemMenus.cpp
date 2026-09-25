// File: src/common/inventory/SystemMenus.cpp
#include "SystemMenus.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/alchemy/PotionBrewing.hpp"
#include "common/world/block/entity/BrewingStandBlockEntity.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/EnchantmentDefinitions.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/enchantment/ItemEnchantments.hpp"
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;

        // Lay out the player's 27 + 9 at `top`. Every menu here does it the
        // same way (MC addStandardInventorySlots).
        void AddPlayerSlots(AbstractContainerMenu& menu, Inventory* inv, int top,
                            const std::function<Slot&(std::unique_ptr<Slot>)>& add) {
            (void)menu;
            for (int i = 0; i < 27; ++i) {
                add(std::make_unique<Slot>(inv, Inventory::MAIN_BEGIN + i,
                                           8 + (i % 9) * SLOT_STEP,
                                           top + (i / 9) * SLOT_STEP));
            }
            for (int i = 0; i < 9; ++i) {
                add(std::make_unique<Slot>(inv, Inventory::HOTBAR_BEGIN + i,
                                           8 + i * SLOT_STEP, top + 58));
            }
        }
    }

    // ══════════════════ Enchanting table ══════════════════════════════════
    namespace {
        // MC EnchantmentMenu's item slot: `getMaxStackSize() { return 1; }`.
        class EnchantItemSlot : public Slot {
        public:
            using Slot::Slot;
            int GetMaxStackSize() const override { return 1; }
        };

        // MC EnchantmentMenu's lapis slot: lapis only, with the empty-slot
        // silhouette.
        class LapisSlot : public Slot {
        public:
            LapisSlot(IContainer* container, int containerSlot, int x, int y)
                : Slot(container, containerSlot, x, y) {
                noItemIcon = "container/slot/lapis_lazuli";
            }
            bool MayPlace(const ItemStack& stack) const override {
                return stack.itemId == Items::LapisLazuli;
            }
        };
    }

    EnchantmentMenu::EnchantmentMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory),
          m_random(static_cast<int64_t>(
              std::chrono::steady_clock::now().time_since_epoch().count())) {
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));

        // MC EnchantmentMenu: item at (15,47), lapis at (35,47).
        AddSlot(std::make_unique<EnchantItemSlot>(&m_inputs, SLOT_ITEM, 15, 47));
        AddSlot(std::make_unique<LapisSlot>(&m_inputs, SLOT_LAPIS, 35, 47));
        auto add = [this](std::unique_ptr<Slot> s) -> Slot& { return AddSlot(std::move(s)); };
        AddPlayerSlots(*this, playerInventory, 84, add);

        // enchantClue / levelClue start at -1 (costs at 0).
        for (int i = 0; i < 3; ++i) {
            SetData(DATA_CLUE_ID_0 + i, -1);
            SetData(DATA_CLUE_LVL_0 + i, -1);
        }
    }

    int EnchantmentMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return MAIN_BEGIN + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    void EnchantmentMenu::SetBookshelfPower(int power) {
        m_power = std::clamp(power, 0, 15);   // getEnchantmentCost caps bookcases at 15
        m_serverSide = true;
        RollOffers();
    }

    void EnchantmentMenu::SetEnchantmentSeed(int seed) {
        SetData(DATA_SEED, seed);
        m_serverSide = true;
        RollOffers();
    }

    void EnchantmentMenu::SlotsChanged(ContainerClickResult& result) {
        RollOffers();
        MarkChanged(result, SLOT_ITEM);
    }

    std::vector<EnchantmentInstance> EnchantmentMenu::GetEnchantmentList(const ItemStack& item,
                                                                        int slot, int cost) {
        // MC getEnchantmentList: reseed with seed + slot, roll over
        // #minecraft:in_enchanting_table, and a plain book loses one of
        // several results at random.
        // Java's int addition wraps before the widening; signed overflow in
        // C++ would be UB, so add as unsigned.
        m_random.SetSeed(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(GetData(DATA_SEED)) + static_cast<uint32_t>(slot))));
        const std::vector<EnchantmentId> table =
            EnchantmentDefinitions::ResolveTagOrdered("minecraft:in_enchanting_table");
        if (table.empty()) return {};
        std::vector<EnchantmentInstance> list =
            EnchantmentHelper::SelectEnchantment(m_random, item, cost, table);
        if (item.itemId == Items::Book && list.size() > 1) {
            list.erase(list.begin() + m_random.NextInt(static_cast<int>(list.size())));
        }
        return list;
    }

    void EnchantmentMenu::RollOffers() {
        // MC EnchantmentMenu.slotsChanged. The client's copy never rolls — its
        // three rows are the server's data slots.
        if (!m_serverSide) return;
        const ItemStack& item = m_inputs.GetItem(SLOT_ITEM);
        if (item.IsEmpty() || !IsEnchantable(item)) {
            for (int i = 0; i < 3; ++i) {
                SetData(DATA_COST_0 + i, 0);
                SetData(DATA_CLUE_ID_0 + i, -1);
                SetData(DATA_CLUE_LVL_0 + i, -1);
            }
            return;
        }

        m_random.SetSeed(static_cast<int64_t>(GetData(DATA_SEED)));
        int costs[3];
        for (int i = 0; i < 3; ++i) {
            // EnchantmentHelper.getEnchantmentCost, verbatim (the item always
            // has ENCHANTABLE here — isEnchantable checked it).
            const int selected = m_random.NextInt(8) + 1 + (m_power >> 1) + m_random.NextInt(m_power + 1);
            if (i == 0)      costs[i] = std::max(selected / 3, 1);
            else if (i == 1) costs[i] = selected * 2 / 3 + 1;
            else             costs[i] = std::max(selected, m_power * 2);
            if (costs[i] < i + 1) costs[i] = 0;
            SetData(DATA_CLUE_ID_0 + i, -1);
            SetData(DATA_CLUE_LVL_0 + i, -1);
        }
        for (int i = 0; i < 3; ++i) {
            SetData(DATA_COST_0 + i, costs[i]);
            if (costs[i] <= 0) continue;
            const std::vector<EnchantmentInstance> list = GetEnchantmentList(item, i, costs[i]);
            if (list.empty()) continue;
            const EnchantmentInstance& clue =
                list[static_cast<size_t>(m_random.NextInt(static_cast<int>(list.size())))];
            SetData(DATA_CLUE_ID_0 + i, static_cast<int>(clue.id));
            SetData(DATA_CLUE_LVL_0 + i, clue.level);
        }
    }

    bool EnchantmentMenu::ClickMenuButton(int buttonId, bool /*mayBuild*/, ContainerClickResult& result) {
        // MC: an out-of-range id is logged and refused.
        if (buttonId < 0 || buttonId >= 3) return false;
        m_performedCost = 0;
        const ItemStack& item  = m_inputs.GetItem(SLOT_ITEM);
        const ItemStack& lapis = m_inputs.GetItem(SLOT_LAPIS);
        const int needed = buttonId + 1;
        // The two refusals clickMenuButton answers false for.
        if ((lapis.IsEmpty() || lapis.count < needed) && !m_infiniteMaterials) return false;
        if (GetData(DATA_COST_0 + buttonId) <= 0 || item.IsEmpty() ||
            ((m_playerLevel < needed || m_playerLevel < GetData(DATA_COST_0 + buttonId)) &&
             !m_infiniteMaterials)) {
            return false;
        }
        // access.execute is a no-op on the client: its copy only answers
        // whether the press is worth sending (EnchantmentScreen.mouseClicked).
        if (!m_serverSide) return true;
        m_performedCost = TakeOffer(buttonId, m_playerLevel, m_infiniteMaterials, result);
        return true;
    }

    int EnchantmentMenu::TakeOffer(int slot, int playerLevel, bool creative,
                                   ContainerClickResult& result) {
        if (slot < 0 || slot > 2) return 0;
        const int cost = GetData(DATA_COST_0 + slot);
        const int enchantmentCost = slot + 1;
        ItemStack& item  = m_inputs.GetItem(SLOT_ITEM);
        ItemStack& lapis = m_inputs.GetItem(SLOT_LAPIS);
        if (cost <= 0 || item.IsEmpty()) return 0;
        if (!creative) {
            if (lapis.IsEmpty() || lapis.itemId != Items::LapisLazuli || lapis.count < enchantmentCost) return 0;
            if (playerLevel < enchantmentCost || playerLevel < cost) return 0;
        }

        // The access.execute body: the same roll the clue came from.
        const std::vector<EnchantmentInstance> enchantments = GetEnchantmentList(item, slot, cost);
        if (enchantments.empty()) return 0;

        // (player.onEnchantmentPerformed is the caller's — see the header.)
        if (item.itemId == Items::Book) {
            // itemStack.transmuteCopy(ENCHANTED_BOOK): the book's components
            // carried over.
            ItemStack book(Items::EnchantedBook, item.count);
            book.components = item.components;
            item = book;
        }
        for (const EnchantmentInstance& e : enchantments) {
            EnchantmentHelper::Enchant(item, e.id, e.level);
        }

        // currency.consume(enchantmentCost, player): nothing in creative.
        if (!creative && !lapis.IsEmpty()) {
            lapis.count -= enchantmentCost;
            if (lapis.count <= 0) lapis.Clear();
        }
        // (awardStat ENCHANT_ITEM / ENCHANTED_ITEM trigger: no stats.)

        MarkChanged(result, SLOT_ITEM);
        MarkChanged(result, SLOT_LAPIS);
        return enchantmentCost;
    }

    void EnchantmentMenu::Removed(ContainerClickResult& result) {
        // MC removed → clearContainer: both slots go back to the player, and
        // what does not fit is dropped rather than lost.
        for (int i = 0; i < 2; ++i) {
            ItemStack& stack = m_inputs.GetItem(i);
            if (stack.IsEmpty()) continue;
            const int leftover = getInventory().AddStack(stack);
            if (leftover > 0) {
                ItemStack overflow = stack;
                overflow.count = leftover;
                result.extraDrops.push_back(overflow);
            }
            stack.Clear();
            MarkChanged(result, i);
        }
    }

    void EnchantmentMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        // MC EnchantmentMenu.quickMoveStack.
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;

        if (slotIndex == SLOT_ITEM || slotIndex == SLOT_LAPIS) {
            if (!MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, true, result)) return;
        } else if (stack.itemId == Items::LapisLazuli) {
            if (!MoveItemStackTo(stack, SLOT_LAPIS, SLOT_LAPIS + 1, true, result)) return;
        } else {
            // One item into the (empty) item slot.
            Slot& itemSlot = GetSlot(SLOT_ITEM);
            if (itemSlot.HasItem() || !itemSlot.MayPlace(stack)) return;
            ItemStack single = stack;
            single.count = 1;
            stack.count -= 1;
            if (stack.count <= 0) stack.Clear();
            itemSlot.SetByPlayer(single);
            MarkChanged(result, SLOT_ITEM);
        }
        if (stack.count == original.count) return;
        slot.SetChanged();
        MarkChanged(result, slotIndex);
    }

    // ══════════════════ Brewing stand ═════════════════════════════════════
    namespace {
        // MC BrewingStandMenu.PotionSlot: a brewing potion input, one per slot.
        class BrewingPotionSlot : public Slot {
        public:
            using Slot::Slot;
            bool MayPlace(const ItemStack& stack) const override { return IsBrewingPotionInput(stack); }
            int  GetMaxStackSize() const override { return 1; }
        };
        // MC BrewingStandMenu.IngredientsSlot: BREWING_REAGENTS.
        class BrewingIngredientSlot : public Slot {
        public:
            using Slot::Slot;
            bool MayPlace(const ItemStack& stack) const override { return IsBrewingReagent(stack); }
        };
        // MC BrewingStandMenu.FuelSlot: anything with BREWING_FUEL.
        class BrewingFuelSlot : public Slot {
        public:
            using Slot::Slot;
            bool MayPlace(const ItemStack& stack) const override { return GetBrewingFuelUses(stack) > 0; }
        };
    }

    BrewingStandMenu::BrewingStandMenu(Inventory* playerInventory, BrewingStandBlockEntity* stand)
        : AbstractContainerMenu(playerInventory) {
        BuildSlots(playerInventory, stand);
        // The block entity's own counters (MC's anonymous dataAccess).
        if (stand) {
            SetOwnedData(std::make_unique<DelegatingContainerData>(
                std::vector<DelegatingContainerData::Entry>{
                    {[stand] { return stand->BrewTime(); },
                     [stand](int v) { stand->SetBrewTime(v); }},
                    {[stand] { return stand->Fuel(); },
                     [stand](int v) { stand->SetFuel(v); }},
                    {[stand] { return stand->TotalBrewTime(); },
                     [stand](int v) { stand->SetTotalBrewTime(v); }},
                    {[stand] { return stand->TotalFuel(); },
                     [stand](int v) { stand->SetTotalFuel(v); }},
                }));
        } else {
            SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
        }
    }

    BrewingStandMenu::BrewingStandMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory),
          m_ownedContainer(std::make_unique<SimpleContainer>(CONTAINER_END)) {
        BuildSlots(playerInventory, m_ownedContainer.get());
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
    }

    void BrewingStandMenu::BuildSlots(Inventory* playerInventory, IContainer* container) {
        // MC BrewingStandMenu: bottles at (56,51),(79,58),(102,51),
        // ingredient (79,17), fuel (17,17).
        AddSlot(std::make_unique<BrewingPotionSlot>(container, 0, 56, 51));
        AddSlot(std::make_unique<BrewingPotionSlot>(container, 1, 79, 58));
        AddSlot(std::make_unique<BrewingPotionSlot>(container, 2, 102, 51));
        AddSlot(std::make_unique<BrewingIngredientSlot>(container, SLOT_INGREDIENT, 79, 17));
        AddSlot(std::make_unique<BrewingFuelSlot>(container, SLOT_FUEL, 17, 17));
        auto add = [this](std::unique_ptr<Slot> s) -> Slot& { return AddSlot(std::move(s)); };
        AddPlayerSlots(*this, playerInventory, 84, add);
    }

    int BrewingStandMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return MAIN_BEGIN + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    int BrewingStandMenu::GetFuel() const              { return GetData(DATA_FUEL); }
    int BrewingStandMenu::GetTotalFuel() const         { return GetData(DATA_TOTAL_FUEL); }
    int BrewingStandMenu::GetBrewingTicks() const      { return GetData(DATA_BREW_TIME); }
    int BrewingStandMenu::GetTotalBrewingTicks() const { return GetData(DATA_TOTAL_BREW_TIME); }

    void BrewingStandMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        // MC BrewingStandMenu.quickMoveStack. MC's early `return EMPTY`s only
        // end the shift-click loop; whatever already moved stays moved, so
        // here every branch falls through to one "did the source change"
        // record at the end.
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;

        const bool fromStand = (slotIndex >= 0 && slotIndex <= 2) ||
                               slotIndex == SLOT_INGREDIENT || slotIndex == SLOT_FUEL;
        if (!fromStand) {
            Slot& ingredientSlot = GetSlot(SLOT_INGREDIENT);
            if (GetBrewingFuelUses(original) > 0) {
                // Fuel first. Blaze powder is also a reagent (strength), so
                // when the fuel slot is full it goes on to the ingredient.
                if (!MoveItemStackTo(stack, SLOT_FUEL, SLOT_FUEL + 1, false, result) &&
                    ingredientSlot.MayPlace(stack)) {
                    MoveItemStackTo(stack, SLOT_INGREDIENT, SLOT_INGREDIENT + 1, false, result);
                }
            } else if (ingredientSlot.MayPlace(stack)) {
                MoveItemStackTo(stack, SLOT_INGREDIENT, SLOT_INGREDIENT + 1, false, result);
            } else if (IsBrewingPotionInput(original)) {
                MoveItemStackTo(stack, 0, 3, false, result);
            } else if (slotIndex >= MAIN_BEGIN && slotIndex < HOTBAR_BEGIN) {
                MoveItemStackTo(stack, HOTBAR_BEGIN, SLOT_COUNT, false, result);
            } else if (slotIndex >= HOTBAR_BEGIN && slotIndex < SLOT_COUNT) {
                MoveItemStackTo(stack, MAIN_BEGIN, HOTBAR_BEGIN, false, result);
            } else {
                MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, false, result);
            }
        } else {
            MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, true, result);
        }

        if (stack.count != original.count) {
            if (stack.count <= 0) stack.Clear();
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

    // ══════════════════ Beacon ════════════════════════════════════════════
    BeaconMenu::BeaconMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory) {
        SetOwnedData(std::make_unique<SimpleContainerData>(DATA_COUNT));
        // MC BeaconMenu: the payment slot sits at (136,110).
        AddSlot(std::make_unique<Slot>(&m_payment, 0, 136, 110));
        auto add = [this](std::unique_ptr<Slot> s) -> Slot& { return AddSlot(std::move(s)); };
        AddPlayerSlots(*this, playerInventory, 137, add);
    }

    int BeaconMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return MAIN_BEGIN + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    void BeaconMenu::Removed(ContainerClickResult& result) {
        ItemStack& stack = m_payment.GetItem(0);
        if (stack.IsEmpty()) return;
        (void)getInventory().AddStack(stack);
        stack.Clear();
        MarkChanged(result, SLOT_PAYMENT);
    }

    void BeaconMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const bool moved = (slotIndex == SLOT_PAYMENT)
            ? MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, true, result)
            : MoveItemStackTo(stack, SLOT_PAYMENT, SLOT_PAYMENT + 1, false, result);
        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

    // ══════════════════ Crafter ═══════════════════════════════════════════
    CrafterMenu::CrafterMenu(Inventory* playerInventory, IContainer* container)
        : AbstractContainerMenu(playerInventory) {
        BuildSlots(playerInventory, container);
    }

    CrafterMenu::CrafterMenu(Inventory* playerInventory)
        : AbstractContainerMenu(playerInventory),
          m_ownedContainer(std::make_unique<SimpleContainer>(GRID_SIZE)) {
        BuildSlots(playerInventory, m_ownedContainer.get());
    }

    void CrafterMenu::BuildSlots(Inventory* playerInventory, IContainer* container) {
        // MC CrafterMenu: the 3x3 starts at (30,17), like a crafting table's.
        for (int i = 0; i < GRID_SIZE; ++i) {
            AddSlot(std::make_unique<Slot>(container, i,
                                           30 + (i % 3) * SLOT_STEP,
                                           17 + (i / 3) * SLOT_STEP));
        }
        auto add = [this](std::unique_ptr<Slot> s) -> Slot& { return AddSlot(std::move(s)); };
        AddPlayerSlots(*this, playerInventory, 84, add);
    }

    int CrafterMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return MAIN_BEGIN + 27 + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    void CrafterMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;
        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;
        const bool moved = (slotIndex < GRID_SIZE)
            ? MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, true, result)
            : MoveItemStackTo(stack, 0, GRID_SIZE, false, result);
        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

} // namespace Game
