// File: src/common/entity/Inventory.hpp
#pragma once

#include "Item.hpp"
#include "../world/block/Blocks.hpp"
#include <array>
#include <optional>

namespace Game {

    // The inventory's slot contents IS an ItemStack (matches MC's design where every
    // slot holds an ItemStack). Old code that still references `InventorySlot` and its
    // members continues to compile via this type alias.
    using InventorySlot = ItemStack;

    // MC-compatible 46-slot inventory.
    // Indices match Minecraft's InventoryMenu.java exactly:
    //   0       crafting result
    //   1..4    2x2 crafting grid
    //   5..8    armor (helmet, chest, legs, boots)
    //   9..35   main inventory (3 rows × 9)
    //   36..44  hotbar (9 slots)
    //   45      offhand (reserved)
    class Inventory {
    public:
        // Region constants (MC-compatible)
        static constexpr int CRAFT_RESULT_BEGIN = 0;
        static constexpr int CRAFT_GRID_BEGIN   = 1;
        static constexpr int CRAFT_GRID_SIZE    = 4;
        static constexpr int ARMOR_BEGIN        = 5;
        static constexpr int ARMOR_SIZE         = 4;
        static constexpr int MAIN_BEGIN         = 9;
        static constexpr int MAIN_SIZE          = 27;
        static constexpr int HOTBAR_BEGIN       = 36;
        static constexpr int HOTBAR_SIZE        = 9;
        static constexpr int OFFHAND_BEGIN      = 45;
        static constexpr int TOTAL_SIZE         = 46;
        static constexpr int MAX_STACK_SIZE     = 64;

        Inventory();

        // Initialize with default blocks for testing
        void InitializeDefaults();

        // Get the currently selected slot index (0-8, hotbar-relative)
        int GetSelectedSlot() const { return selectedSlot; }

        // Set the selected slot (clamps to valid range)
        void SetSelectedSlot(int slot);

        // Get the currently selected item.
        ItemID  GetSelectedItem()  const;
        BlockID GetSelectedBlock() const; // returns Air for non-block items

        // Get slot at unified index (0..TOTAL_SIZE-1)
        const ItemStack& GetSlot(int index) const;

        // Mutable slot access (used by server-side click handler)
        ItemStack& MutableSlot(int index);

        // Try to consume one item from the selected slot
        bool ConsumeSelectedBlock();  // legacy name kept; works on any item

        // Try to add items to inventory (player region: main + hotbar).
        // Returns number of items that couldn't be added.
        int AddItems(ItemID id, int count);
        // Convenience overload — converts BlockID to BlockItem.
        int AddBlocks(BlockID blockId, int count) {
            return AddItems(ItemRegistry::FromBlock(blockId), count);
        }

        // Find first slot containing the specified item (player region)
        std::optional<int> FindItem(ItemID id) const;
        std::optional<int> FindBlock(BlockID b) const {
            return FindItem(ItemRegistry::FromBlock(b));
        }

        bool HasItem(ItemID id)  const { return FindItem(id).has_value(); }
        bool HasBlock(BlockID b) const { return FindBlock(b).has_value(); }
        int  GetItemCount(ItemID id) const;
        int  GetBlockCount(BlockID b) const { return GetItemCount(ItemRegistry::FromBlock(b)); }

        // Set a slot from server data (server-authoritative sync).
        // Accepts the full unified index range.
        void SetSlot(int index, ItemID id, int count = 64);
        // Convenience overload for legacy block-only callers.
        void SetSlot(int index, BlockID b, int count = 64) {
            SetSlot(index, ItemRegistry::FromBlock(b), count);
        }

        // Whole-stack overload (used by network sync)
        void SetSlotFull(int index, const ItemStack& s);

        void Clear();

        void SelectPreviousSlot();
        void SelectNextSlot();

        // Region predicates
        static bool IsCraftResultSlot(int i) { return i == CRAFT_RESULT_BEGIN; }
        static bool IsCraftGridSlot(int i)   { return i >= CRAFT_GRID_BEGIN && i < CRAFT_GRID_BEGIN + CRAFT_GRID_SIZE; }
        static bool IsArmorSlot(int i)       { return i >= ARMOR_BEGIN && i < ARMOR_BEGIN + ARMOR_SIZE; }
        static bool IsMainSlot(int i)        { return i >= MAIN_BEGIN && i < MAIN_BEGIN + MAIN_SIZE; }
        static bool IsHotbarSlot(int i)      { return i >= HOTBAR_BEGIN && i < HOTBAR_BEGIN + HOTBAR_SIZE; }
        static bool IsPlayerSlot(int i)      { return IsMainSlot(i) || IsHotbarSlot(i); }
        static bool IsOffhandSlot(int i)     { return i == OFFHAND_BEGIN; }

        // Hotbar 0..8 ↔ unified index 36..44
        static int HotbarToIndex(int hotbarSlot) { return HOTBAR_BEGIN + hotbarSlot; }
        static int IndexToHotbar(int unifiedIndex) { return unifiedIndex - HOTBAR_BEGIN; }

    private:
        std::array<ItemStack, TOTAL_SIZE> slots;
        int selectedSlot;

        // Try to add to a specific slot (returns items added)
        int AddToSlot(int slotIndex, ItemID id, int count);
    };

} // namespace Game
