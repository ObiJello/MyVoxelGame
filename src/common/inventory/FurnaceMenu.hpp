// File: src/common/inventory/FurnaceMenu.hpp
//
// Mirrors net.minecraft.world.inventory.AbstractFurnaceMenu — the furnace,
// blast furnace and smoker share one menu shape, differing only in which
// RecipeType decides whether an item is a valid input.
//
// Slot order (MC's, and therefore the wire's):
//   0        input
//   1        fuel
//   2        result
//   3..29    player main inventory
//   30..38   player hotbar
//
// Data slots 0..3 are the burn/cook counters — see FurnaceBlockEntity. On the
// server they read THROUGH to the block entity so a furnace burning with the
// screen shut still reports the truth; on the client they are a plain array the
// server's ContainerSetData deltas write into.
#pragma once

#include "AbstractContainerMenu.hpp"
#include "SimpleContainer.hpp"
#include "common/world/crafting/GeneratedRecipeList.hpp"
#include <memory>

namespace Game {

    class FurnaceBlockEntity;

    // MC FurnaceResultSlot: the output square. Can't be placed into, and taking
    // from it is what pays out the banked smelting XP.
    class FurnaceResultSlot : public Slot {
    public:
        FurnaceResultSlot(IContainer* container, int containerSlot, int x, int y,
                          FurnaceBlockEntity* furnace)
            : Slot(container, containerSlot, x, y), m_furnace(furnace) {}

        bool MayPlace(const ItemStack& /*stack*/) const override { return false; }

    private:
        FurnaceBlockEntity* m_furnace = nullptr;   // null on the client
    };

    class FurnaceMenu : public AbstractContainerMenu {
    public:
        static constexpr int SLOT_INPUT    = 0;
        static constexpr int SLOT_FUEL     = 1;
        static constexpr int SLOT_RESULT   = 2;
        static constexpr int CONTAINER_END = 3;
        static constexpr int MAIN_BEGIN    = 3;
        static constexpr int HOTBAR_BEGIN  = 30;
        static constexpr int SLOT_COUNT    = 39;

        // SERVER: borrow the block entity, and publish its live counters.
        FurnaceMenu(Inventory* playerInventory, FurnaceBlockEntity* furnace);
        // CLIENT: own a scratch 3-slot container and a plain data array.
        FurnaceMenu(Inventory* playerInventory, CookingKind kind);

        CookingKind Kind() const { return m_kind; }

        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;

        // Screen helpers — MC AbstractFurnaceMenu's three accessors, which is
        // all AbstractFurnaceScreen.renderBg reads. The ratios live here and
        // the pixel math lives in the screen, exactly as MC splits it, so both
        // halves stay line-comparable with the source.
        bool  IsLit() const;
        float LitProgress() const;    // 0..1, flame height
        float BurnProgress() const;   // 0..1, arrow width

        // Drain the XP the block entity banked. Server-side only — returns 0
        // on the client, which has no block entity and must award nothing.
        float TakeBankedExperience();

    private:
        void BuildSlots(Inventory* playerInventory, IContainer* container,
                        FurnaceBlockEntity* furnace);

        CookingKind m_kind = CookingKind::Smelting;
        FurnaceBlockEntity* m_furnace = nullptr;             // null on the client
        std::unique_ptr<SimpleContainer> m_ownedContainer;   // client only
    };

} // namespace Game
