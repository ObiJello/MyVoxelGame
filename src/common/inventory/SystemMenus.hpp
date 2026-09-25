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
#include "common/world/enchantment/EnchantmentInstance.hpp"
#include <memory>
#include <vector>

namespace Game {

    class BrewingStandBlockEntity;

    // ── Enchanting table (MC EnchantmentMenu) ─────────────────────────────
    // Slots: 0 item (one at a time), 1 lapis. Data: three costs, the
    // enchantment seed, and three (enchantment id, level) clues — MC publishes
    // exactly these so the client can render the three offer rows without
    // knowing the roll.
    //
    // MC EnchantmentMenu line for line: the offers are rolled on the SERVER
    // only (MC's slotsChanged runs inside access.execute, a no-op on the
    // client — here the menu learns it is the server's copy from
    // SetBookshelfPower / SetEnchantmentSeed) from the player's enchantment
    // seed; costs from EnchantmentHelper.getEnchantmentCost, the clue from
    // the same selectEnchantment roll the button then applies
    // (#minecraft:in_enchanting_table, the item's ENCHANTABLE value).
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
        // Server only — also what marks this copy as the table's (see above).
        void SetBookshelfPower(int power);
        int  BookshelfPower() const { return m_power; }

        // MC `enchantmentSeed.set(player.getEnchantmentSeed())` — at open and
        // after every enchantment performed. Server only; re-rolls the offers.
        void SetEnchantmentSeed(int seed);
        int  EnchantmentSeed() const { return GetData(DATA_SEED); }

        // The clicking player's experience level and hasInfiniteMaterials
        // (creative), refreshed by the session before each button click.
        void SetPlayerState(int experienceLevel, bool infiniteMaterials) {
            m_playerLevel = experienceLevel;
            m_infiniteMaterials = infiniteMaterials;
        }

        // MC EnchantmentMenu.clickMenuButton(player, buttonId): take offer
        // `buttonId` (0..2) with the state SetPlayerState gave.
        bool ClickMenuButton(int buttonId, bool mayBuild, ContainerClickResult& result) override;

        // The body of clickMenuButton: checks lapis (buttonId + 1) and levels
        // (at least the row's cost and buttonId + 1) unless creative, enchants
        // the item (a book becomes an enchanted book), spends the lapis.
        // Returns the levels the player must be charged — buttonId + 1, MC
        // Player.onEnchantmentPerformed — or 0 when refused. The caller
        // charges them, rerolls the player's seed and hands it back through
        // SetEnchantmentSeed.
        int TakeOffer(int slot, int playerLevel, bool creative, ContainerClickResult& result);

        // The levels the last ClickMenuButton performed an enchantment for,
        // cleared by the read (0 = nothing happened).
        int ConsumePerformedCost() {
            const int cost = m_performedCost;
            m_performedCost = 0;
            return cost;
        }

        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;
        void SlotsChanged(ContainerClickResult& result) override;
        void Removed(ContainerClickResult& result) override;

    private:
        void RollOffers();
        // MC EnchantmentMenu.getEnchantmentList.
        std::vector<EnchantmentInstance> GetEnchantmentList(const ItemStack& item, int slot, int cost);

        SimpleContainer m_inputs{2};
        JavaRandom      m_random;
        int             m_power = 0;
        bool            m_serverSide = false;
        int             m_playerLevel = 0;
        bool            m_infiniteMaterials = false;
        int             m_performedCost = 0;
    };

    // ── Brewing stand (MC BrewingStandMenu) ───────────────────────────────
    // Slots: 0..2 bottles (PotionSlot — brewing potion inputs, max 1 each),
    // 3 ingredient (IngredientsSlot — a brewing reagent), 4 fuel (FuelSlot —
    // BREWING_FUEL, i.e. blaze powder). Data: the stand's four counters —
    // brew time, fuel uses, total brew time, total fuel uses — read straight
    // through from the BrewingStandBlockEntity, which does the brewing.
    class BrewingStandMenu : public AbstractContainerMenu {
    public:
        static constexpr int SLOT_BOTTLE_0  = 0;
        static constexpr int SLOT_INGREDIENT = 3;
        static constexpr int SLOT_FUEL      = 4;
        static constexpr int CONTAINER_END  = 5;
        static constexpr int MAIN_BEGIN     = 5;
        static constexpr int HOTBAR_BEGIN   = 32;
        static constexpr int SLOT_COUNT     = 41;

        static constexpr int DATA_BREW_TIME       = 0;
        static constexpr int DATA_FUEL            = 1;
        static constexpr int DATA_TOTAL_BREW_TIME = 2;
        static constexpr int DATA_TOTAL_FUEL      = 3;
        static constexpr int DATA_COUNT           = 4;

        BrewingStandMenu(Inventory* playerInventory, BrewingStandBlockEntity* stand);
        explicit BrewingStandMenu(Inventory* playerInventory);   // client

        void QuickMoveStack(int slotIndex, ContainerClickResult& result) override;
        int  MenuIndexForInventorySlot(int inventoryIndex) const override;

        // MC getFuel / getTotalFuel / getBrewingTicks / getTotalBrewingTicks —
        // what BrewingStandScreen.renderBg reads.
        int GetFuel() const;
        int GetTotalFuel() const;
        int GetBrewingTicks() const;
        int GetTotalBrewingTicks() const;

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
