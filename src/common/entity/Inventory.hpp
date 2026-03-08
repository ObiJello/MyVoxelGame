// File: src/common/entity/Inventory.hpp
#pragma once

#include "../world/block/Blocks.hpp"
#include <array>
#include <optional>

namespace Game {

    // Represents a single inventory slot
    struct InventorySlot {
        BlockID blockId = BlockID::Air;
        int count = 0;

        bool IsEmpty() const { return count <= 0 || blockId == BlockID::Air; }
        void Clear() { blockId = BlockID::Air; count = 0; }
    };

    class Inventory {
    public:
        static constexpr int HOTBAR_SIZE = 9;
        static constexpr int MAX_STACK_SIZE = 64;

        Inventory();

        // Initialize with default blocks for testing
        void InitializeDefaults();

        // Get the currently selected slot index (0-8)
        int GetSelectedSlot() const { return selectedSlot; }

        // Set the selected slot (clamps to valid range)
        void SetSelectedSlot(int slot);

        // Get the currently selected block type
        BlockID GetSelectedBlock() const;

        // Get slot at index
        const InventorySlot& GetSlot(int index) const;

        // Try to consume one block from the selected slot
        // Returns true if successful
        bool ConsumeSelectedBlock();

        // Try to add blocks to inventory
        // Returns number of blocks that couldn't be added (0 if all added)
        int AddBlocks(BlockID blockId, int count);

        // Find first slot containing the specified block
        std::optional<int> FindBlock(BlockID blockId) const;

        // Check if inventory has at least one of the specified block
        bool HasBlock(BlockID blockId) const;

        // Get total count of a specific block type across all slots
        int GetBlockCount(BlockID blockId) const;

        // Set a slot from server data (server-authoritative sync)
        void SetSlot(int index, BlockID blockId, int count = 64);

        // Clear all slots
        void Clear();

        // Move selected slot left/right (with wrapping)
        void SelectPreviousSlot();
        void SelectNextSlot();

    private:
        std::array<InventorySlot, HOTBAR_SIZE> slots;
        int selectedSlot;

        // Try to add blocks to a specific slot
        // Returns number of blocks added
        int AddToSlot(int slotIndex, BlockID blockId, int count);
    };

} // namespace Game