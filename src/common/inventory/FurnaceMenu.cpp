// File: src/common/inventory/FurnaceMenu.cpp
#include "FurnaceMenu.hpp"
#include "common/entity/GeneratedItemList.hpp"   // Items::Bucket
#include "common/world/block/entity/FurnaceBlockEntity.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include <algorithm>
#include <memory>

namespace Game {

    namespace {
        constexpr int SLOT_STEP = 18;

        // MC FurnaceFuelSlot.mayPlace: fuel, or a bucket (so an empty bucket
        // can be swapped for the lava one that is sitting there).
        class FurnaceFuelSlot : public Slot {
        public:
            using Slot::Slot;
            bool MayPlace(const ItemStack& stack) const override {
                return RecipeManager::GetFuelBurnTime(stack) > 0 ||
                       stack.itemId == Items::Bucket;
            }
        };
    }

    // MC FurnaceResultSlot.onTake → AbstractFurnaceBlockEntity.awardUsedRecipes:
    // taking the output pays out the XP banked by every smelt that produced it.
    // Reported through the menu so the SERVER can credit the player — the menu
    // itself has no ServerPlayer, and the client must never award anything.
    float FurnaceMenu::TakeBankedExperience() {
        return m_furnace ? m_furnace->TakeStoredExperience() : 0.0f;
    }

    FurnaceMenu::FurnaceMenu(Inventory* playerInventory, FurnaceBlockEntity* furnace)
        : AbstractContainerMenu(playerInventory),
          m_kind(furnace ? furnace->Kind() : CookingKind::Smelting),
          m_furnace(furnace) {
        BuildSlots(playerInventory, furnace, furnace);

        // Publish the block entity's OWN counters — MC's anonymous
        // ContainerData in AbstractFurnaceBlockEntity. Reading through means
        // the menu can never show a stale flame.
        if (furnace) {
            SetOwnedData(std::make_unique<DelegatingContainerData>(
                std::vector<DelegatingContainerData::Entry>{
                    {[furnace] { return furnace->LitTime(); },
                     [furnace](int v) { furnace->SetLitTime(v); }},
                    {[furnace] { return furnace->LitDuration(); },
                     [furnace](int v) { furnace->SetLitDuration(v); }},
                    {[furnace] { return furnace->CookingTime(); },
                     [furnace](int v) { furnace->SetCookingTime(v); }},
                    {[furnace] { return furnace->CookingTotal(); },
                     [furnace](int v) { furnace->SetCookingTotal(v); }},
                }));
        }
    }

    FurnaceMenu::FurnaceMenu(Inventory* playerInventory, CookingKind kind)
        : AbstractContainerMenu(playerInventory), m_kind(kind),
          m_ownedContainer(std::make_unique<SimpleContainer>(FurnaceBlockEntity::SLOT_COUNT)) {
        BuildSlots(playerInventory, m_ownedContainer.get(), nullptr);
        SetOwnedData(std::make_unique<SimpleContainerData>(FurnaceBlockEntity::DATA_COUNT));
    }

    void FurnaceMenu::BuildSlots(Inventory* playerInventory, IContainer* container,
                                 FurnaceBlockEntity* furnace) {
        // Coordinates are MC AbstractFurnaceMenu's (line 60-63).
        AddSlot(std::make_unique<Slot>(container, FurnaceBlockEntity::SLOT_INPUT, 56, 17));
        AddSlot(std::make_unique<FurnaceFuelSlot>(container, FurnaceBlockEntity::SLOT_FUEL, 56, 53));
        AddSlot(std::make_unique<FurnaceResultSlot>(container, FurnaceBlockEntity::SLOT_RESULT,
                                                    116, 35, furnace));

        for (int i = 0; i < 27; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::MAIN_BEGIN + i,
                                           8 + (i % 9) * SLOT_STEP,
                                           84 + (i / 9) * SLOT_STEP));
        }
        for (int i = 0; i < 9; ++i) {
            AddSlot(std::make_unique<Slot>(playerInventory, Inventory::HOTBAR_BEGIN + i,
                                           8 + i * SLOT_STEP, 142));
        }
    }

    int FurnaceMenu::MenuIndexForInventorySlot(int inventoryIndex) const {
        if (Inventory::IsMainSlot(inventoryIndex)) {
            return MAIN_BEGIN + (inventoryIndex - Inventory::MAIN_BEGIN);
        }
        if (Inventory::IsHotbarSlot(inventoryIndex)) {
            return HOTBAR_BEGIN + (inventoryIndex - Inventory::HOTBAR_BEGIN);
        }
        return -1;
    }

    bool FurnaceMenu::IsLit() const {
        // MC AbstractFurnaceMenu.isLit — fuel remaining, not "cooking".
        return GetData(FurnaceBlockEntity::DATA_LIT_TIME) > 0;
    }

    float FurnaceMenu::LitProgress() const {
        // MC AbstractFurnaceMenu.getLitProgress: fall back to a 200-tick
        // duration so a furnace loaded mid-burn still draws a sane flame.
        int duration = GetData(FurnaceBlockEntity::DATA_LIT_DURATION);
        if (duration == 0) duration = 200;
        const float p = static_cast<float>(GetData(FurnaceBlockEntity::DATA_LIT_TIME)) /
                        static_cast<float>(duration);
        return std::clamp(p, 0.0f, 1.0f);
    }

    float FurnaceMenu::BurnProgress() const {
        // MC AbstractFurnaceMenu.getBurnProgress.
        const int total = GetData(FurnaceBlockEntity::DATA_COOKING_TOTAL);
        const int done  = GetData(FurnaceBlockEntity::DATA_COOKING_TIME);
        if (total == 0 || done == 0) return 0.0f;
        return std::clamp(static_cast<float>(done) / static_cast<float>(total),
                          0.0f, 1.0f);
    }

    void FurnaceMenu::QuickMoveStack(int slotIndex, ContainerClickResult& result) {
        Slot& slot = GetSlot(slotIndex);
        if (!slot.HasItem()) return;

        ItemStack& stack = slot.GetItemMut();
        const ItemStack original = stack;

        // AbstractFurnaceMenu.quickMoveStack (line 110-150).
        bool moved = false;
        if (slotIndex == SLOT_RESULT) {
            // Out of the output, into the player, filling from the back.
            moved = MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, true, result);
        } else if (slotIndex == SLOT_INPUT || slotIndex == SLOT_FUEL) {
            moved = MoveItemStackTo(stack, MAIN_BEGIN, SLOT_COUNT, false, result);
        } else {
            // From the player: smeltable → input, else fuel → fuel, else swap
            // between the main rows and the hotbar.
            if (RecipeManager::FindCooking(m_kind, stack)) {
                moved = MoveItemStackTo(stack, SLOT_INPUT, SLOT_INPUT + 1, false, result);
            } else if (RecipeManager::GetFuelBurnTime(stack) > 0) {
                moved = MoveItemStackTo(stack, SLOT_FUEL, SLOT_FUEL + 1, false, result);
            } else if (slotIndex < HOTBAR_BEGIN) {
                moved = MoveItemStackTo(stack, HOTBAR_BEGIN, SLOT_COUNT, false, result);
            } else {
                moved = MoveItemStackTo(stack, MAIN_BEGIN, HOTBAR_BEGIN, false, result);
            }
        }

        if (!moved) return;
        if (stack.count != original.count) {
            slot.SetChanged();
            MarkChanged(result, slotIndex);
        }
    }

} // namespace Game
