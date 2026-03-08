// File: src/common/entity/Inventory.cpp
#include "Inventory.hpp"
#include "../core/Log.hpp"
#include <algorithm>

namespace Game {

    Inventory::Inventory() : selectedSlot(0) {
        // Initialize all slots as empty
        for (auto& slot : slots) {
            slot.Clear();
        }
    }

    void Inventory::InitializeDefaults() {
        Clear();
        // Server sends authoritative hotbar contents via HotbarSyncS2C on join
        Log::Info("Inventory cleared, awaiting server hotbar sync");
    }

    void Inventory::SetSelectedSlot(int slot) {
        selectedSlot = std::clamp(slot, 0, HOTBAR_SIZE - 1);
    }

    BlockID Inventory::GetSelectedBlock() const {
        const auto& slot = slots[selectedSlot];
        return slot.IsEmpty() ? BlockID::Air : slot.blockId;
    }

    const InventorySlot& Inventory::GetSlot(int index) const {
        static const InventorySlot emptySlot{};
        if (index < 0 || index >= HOTBAR_SIZE) {
            return emptySlot;
        }
        return slots[index];
    }

    bool Inventory::ConsumeSelectedBlock() {
        auto& slot = slots[selectedSlot];

        if (slot.IsEmpty()) {
            return false;
        }

        slot.count--;

        if (slot.count <= 0) {
            slot.Clear();
        }

        return true;
    }

    int Inventory::AddBlocks(BlockID blockId, int count) {
        if (blockId == BlockID::Air || count <= 0) {
            return count;
        }

        int remaining = count;

        // First, try to add to existing stacks of the same block
        for (int i = 0; i < HOTBAR_SIZE && remaining > 0; ++i) {
            auto& slot = slots[i];

            if (slot.blockId == blockId && slot.count < MAX_STACK_SIZE) {
                int toAdd = std::min(remaining, MAX_STACK_SIZE - slot.count);
                slot.count += toAdd;
                remaining -= toAdd;
            }
        }

        // Then, try to add to empty slots
        for (int i = 0; i < HOTBAR_SIZE && remaining > 0; ++i) {
            auto& slot = slots[i];

            if (slot.IsEmpty()) {
                int toAdd = std::min(remaining, MAX_STACK_SIZE);
                slot.blockId = blockId;
                slot.count = toAdd;
                remaining -= toAdd;
            }
        }

        return remaining; // Return any blocks that couldn't be added
    }

    std::optional<int> Inventory::FindBlock(BlockID blockId) const {
        for (int i = 0; i < HOTBAR_SIZE; ++i) {
            if (slots[i].blockId == blockId && !slots[i].IsEmpty()) {
                return i;
            }
        }
        return std::nullopt;
    }

    bool Inventory::HasBlock(BlockID blockId) const {
        return FindBlock(blockId).has_value();
    }

    int Inventory::GetBlockCount(BlockID blockId) const {
        int total = 0;
        for (const auto& slot : slots) {
            if (slot.blockId == blockId) {
                total += slot.count;
            }
        }
        return total;
    }

    void Inventory::SetSlot(int index, BlockID blockId, int count) {
        if (index >= 0 && index < HOTBAR_SIZE) {
            slots[index] = {blockId, count};
        }
    }

    void Inventory::Clear() {
        for (auto& slot : slots) {
            slot.Clear();
        }
        selectedSlot = 0;
    }

    void Inventory::SelectPreviousSlot() {
        selectedSlot = (selectedSlot - 1 + HOTBAR_SIZE) % HOTBAR_SIZE;
    }

    void Inventory::SelectNextSlot() {
        selectedSlot = (selectedSlot + 1) % HOTBAR_SIZE;
    }

    int Inventory::AddToSlot(int slotIndex, BlockID blockId, int count) {
        if (slotIndex < 0 || slotIndex >= HOTBAR_SIZE || count <= 0) {
            return 0;
        }

        auto& slot = slots[slotIndex];

        if (slot.IsEmpty()) {
            slot.blockId = blockId;
            slot.count = std::min(count, MAX_STACK_SIZE);
            return slot.count;
        }

        if (slot.blockId == blockId) {
            int available = MAX_STACK_SIZE - slot.count;
            int toAdd = std::min(count, available);
            slot.count += toAdd;
            return toAdd;
        }

        return 0;
    }

} // namespace Game