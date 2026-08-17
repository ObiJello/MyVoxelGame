// File: src/common/inventory/SystemMenus.hpp
//
// The four menus that sit on top of a gameplay system rather than on storage:
// enchanting table, brewing stand, beacon and crafter.
//
// Each is documented at its class with exactly which parts are live and which
// are waiting on a system that does not exist yet, so the gaps are visible in
// the code rather than discovered in play. Nothing here fakes a result: a menu
// whose system is missing produces no output and hands your items back.
#pragma once

#include "AbstractContainerMenu.hpp"
#include "SimpleContainer.hpp"
#include "common/core/JavaRandom.hpp"
#include <memory>
#include <vector>

namespace Game {

    class BrewingStandBlockEntity;

    // ── Enchanting table (MC EnchantmentMenu) ─────────────────────────────
    // Slots: 0 item, 1 lapis. Data: three costs, the enchantment seed, and
    // three (enchantment id, level) clues — MC publishes exactly these so the
    // client can render the three offer rows without knowing the roll.
    //
    // LIVE: bookshelf power, the three level costs, the level/lapis checks,
    //       and the roll itself (weighted pick over the enchantment registry).
    // The cost curve and power scan are MC's verbatim; see the .cpp.
    class EnchantmentMenu : public AbstractContainerMenu {
    public:
        static constexpr int SLOT_ITEM  = 0;
        static constexpr int SLOT_LAPIS = 1;
        static constexpr int RESULT_END = 2;
        static constexpr int MAIN_BEGIN = 2;
        static constexpr int SLOT_COUNT = 38;

        // MC EnchantmentMenu.dataSlots — 3 costs, seed, 3 clue ids, 3 clue levels.
        static constexpr int DATA_COST_0     = 0;
        static constexpr int DATA_SEED       = 3;
        static constexpr int DATA_CLUE_ID_0  = 4;
        static constexpr int DATA_CLUE_LVL_0 = 7;
        static constexpr int DATA_COUNT      = 10;

        explicit EnchantmentMenu(Inventory* playerInventory);

        // Bookshelves around the table, 0..15 (MC counts them in slotsChanged).
        void SetBookshelfPower(int power);
        int  BookshelfPower() const { return m_power; }

        // MC EnchantmentMenu.clickMenuButton — take offer `slot` (0..2).
        // `playerLevel` is checked and the cost returned so the caller can
        // charge it; 0 means the offer was refused.
        int TakeOffer(int slot, int playerLevel, bool creative,
                      ContainerClickResult& result);

        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;
        void SlotsChanged(ContainerClickResult& result) override;
        void Removed(ContainerClickResult& result) override;

    private:
        void RollOffers();

        SimpleContainer m_inputs{2};
        JavaRandom      m_random;
        int             m_power = 0;
        int             m_seed  = 0;
    };

    // ── Brewing stand (MC BrewingStandMenu) ───────────────────────────────
    // Slots: 0..2 bottles, 3 ingredient, 4 fuel (blaze powder).
    //
    // The brewing itself needs potion recipes, which MC keeps in CODE
    // (PotionBrewing.java builds its mixes at bootstrap) rather than in data/,
    // and a POTION_CONTENTS component to carry the result. Neither exists here,
    // so the stand opens, holds its five items, tracks fuel, and brews nothing.
    class BrewingStandMenu : public AbstractContainerMenu {
    public:
        static constexpr int SLOT_BOTTLE_0  = 0;
        static constexpr int SLOT_INGREDIENT = 3;
        static constexpr int SLOT_FUEL      = 4;
        static constexpr int CONTAINER_END  = 5;
        static constexpr int MAIN_BEGIN     = 5;
        static constexpr int SLOT_COUNT     = 41;

        static constexpr int DATA_BREW_TIME = 0;
        static constexpr int DATA_FUEL      = 1;
        static constexpr int DATA_COUNT     = 2;

        BrewingStandMenu(Inventory* playerInventory, IContainer* container);
        explicit BrewingStandMenu(Inventory* playerInventory);   // client

        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;

    private:
        void BuildSlots(Inventory* playerInventory, IContainer* container);
        std::unique_ptr<SimpleContainer> m_ownedContainer;
    };

    // ── Beacon (MC BeaconMenu) ────────────────────────────────────────────
    // One payment slot. Data: pyramid level, and the two chosen effects.
    //
    // LIVE: the payment slot and the pyramid level, which the server computes
    // from the blocks under the beacon. Applying an effect needs a MobEffect
    // system (no status effects exist on ServerPlayer at all), so the chosen
    // effect is recorded and does nothing.
    class BeaconMenu : public AbstractContainerMenu {
    public:
        static constexpr int SLOT_PAYMENT = 0;
        static constexpr int MAIN_BEGIN   = 1;
        static constexpr int SLOT_COUNT   = 37;

        static constexpr int DATA_LEVEL     = 0;
        static constexpr int DATA_PRIMARY   = 1;
        static constexpr int DATA_SECONDARY = 2;
        static constexpr int DATA_COUNT     = 3;

        explicit BeaconMenu(Inventory* playerInventory);

        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;
        void Removed(ContainerClickResult& result) override;

    private:
        SimpleContainer m_payment{1};
    };

    // ── Crafter (MC CrafterMenu) ──────────────────────────────────────────
    // A 3x3 container that crafts on a redstone pulse. The GRID and its
    // recipe lookup are live — it is a ChestMenu-shaped container plus the
    // crafting matcher. Redstone does not exist, so nothing pulses it; the
    // block is usable as a 9-slot container that knows what it would make.
    class CrafterMenu : public AbstractContainerMenu {
    public:
        static constexpr int GRID_SIZE   = 9;
        static constexpr int MAIN_BEGIN  = 9;
        static constexpr int SLOT_COUNT  = 45;

        CrafterMenu(Inventory* playerInventory, IContainer* container);
        explicit CrafterMenu(Inventory* playerInventory);        // client

        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;

    private:
        void BuildSlots(Inventory* playerInventory, IContainer* container);
        std::unique_ptr<SimpleContainer> m_ownedContainer;
    };

} // namespace Game
