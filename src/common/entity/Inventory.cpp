// File: src/common/entity/Inventory.cpp
#include "Inventory.hpp"
#include "../core/Log.hpp"
#include <algorithm>

namespace Game {

    Inventory::Inventory() : selectedSlot(0) {
        for (auto& slot : slots) {
            slot.Clear();
        }
    }

    void Inventory::InitializeDefaults() {
        Clear();
        Log::Info("Inventory cleared, awaiting server inventory sync");
    }

    void Inventory::SetSelectedSlot(int slot) {
        selectedSlot = std::clamp(slot, 0, HOTBAR_SIZE - 1);
    }

    ItemID Inventory::GetSelectedItem() const {
        const auto& slot = slots[HotbarToIndex(selectedSlot)];
        return slot.IsEmpty() ? Items::Air : slot.itemId;
    }

    BlockID Inventory::GetSelectedBlock() const {
        return slots[HotbarToIndex(selectedSlot)].AsBlockID();
    }

    const ItemStack& Inventory::GetSlot(int index) const {
        static const ItemStack emptySlot{};
        if (index < 0 || index >= TOTAL_SIZE) {
            return emptySlot;
        }
        return slots[index];
    }

    ItemStack& Inventory::MutableSlot(int index) {
        static ItemStack dummy{};
        if (index < 0 || index >= TOTAL_SIZE) {
            dummy.Clear();
            return dummy;
        }
        return slots[index];
    }

    bool Inventory::ConsumeSelectedBlock() {
        auto& slot = slots[HotbarToIndex(selectedSlot)];
        if (slot.IsEmpty()) return false;

        slot.count--;
        if (slot.count <= 0) slot.Clear();
        return true;
    }

    int Inventory::AddItems(ItemID id, int count) {
        if (id == Items::Air || count <= 0) return count;

        int remaining = count;
        const int maxStack = ItemRegistry::Get(id).maxStackSize;

        // MC's Inventory.add() priority order (Inventory.java add() →
        // getSlotWithRemainingSpace() → getFreeSlot()). MC's items list is laid out
        // hotbar(0..8) + main(9..35) + armor(36..39) + offhand(40), so iterating from
        // index 0 finds the hotbar first. Our unified index has main(9..35) before
        // hotbar(36..44), so we walk these regions explicitly to preserve MC's
        // selected-hotbar > offhand > hotbar > main priority.
        const int sel = HOTBAR_BEGIN + selectedSlot;

        auto tryMerge = [&](int slotIdx) {
            if (remaining <= 0) return;
            auto& slot = slots[slotIdx];
            if (slot.itemId == id && slot.count > 0 && slot.count < maxStack) {
                int toAdd = std::min(remaining, maxStack - slot.count);
                slot.count += toAdd;
                remaining -= toAdd;
            }
        };
        auto tryFill = [&](int slotIdx) {
            if (remaining <= 0) return;
            auto& slot = slots[slotIdx];
            if (slot.IsEmpty()) {
                int toAdd = std::min(remaining, maxStack);
                slot.itemId = id;
                slot.count  = toAdd;
                remaining -= toAdd;
            }
        };

        // Pass 1 (merge into existing stacks): selected → offhand → hotbar → main
        tryMerge(sel);
        tryMerge(OFFHAND_BEGIN);
        for (int i = HOTBAR_BEGIN; i < HOTBAR_BEGIN + HOTBAR_SIZE; ++i) tryMerge(i);
        for (int i = MAIN_BEGIN;   i < MAIN_BEGIN   + MAIN_SIZE;   ++i) tryMerge(i);

        // Pass 2 (place in empty slot): hotbar (left to right) → main (top-left to
        // bottom-right). MC's getFreeSlot iterates items 0..size and returns the first
        // empty index, which under MC's layout is hotbar-first.
        for (int i = HOTBAR_BEGIN; i < HOTBAR_BEGIN + HOTBAR_SIZE; ++i) tryFill(i);
        for (int i = MAIN_BEGIN;   i < MAIN_BEGIN   + MAIN_SIZE;   ++i) tryFill(i);

        return remaining;
    }

    std::optional<int> Inventory::FindItem(ItemID id) const {
        const int playerBegin = MAIN_BEGIN;
        const int playerEnd   = HOTBAR_BEGIN + HOTBAR_SIZE;
        for (int i = playerBegin; i < playerEnd; ++i) {
            if (slots[i].itemId == id && !slots[i].IsEmpty()) {
                return i;
            }
        }
        return std::nullopt;
    }

    int Inventory::GetItemCount(ItemID id) const {
        int total = 0;
        const int playerBegin = MAIN_BEGIN;
        const int playerEnd   = HOTBAR_BEGIN + HOTBAR_SIZE;
        for (int i = playerBegin; i < playerEnd; ++i) {
            if (slots[i].itemId == id) total += slots[i].count;
        }
        return total;
    }

    void Inventory::SetSlot(int index, ItemID id, int count) {
        if (index >= 0 && index < TOTAL_SIZE) {
            slots[index] = {id, count};
        }
    }

    void Inventory::SetSlotFull(int index, const ItemStack& s) {
        if (index >= 0 && index < TOTAL_SIZE) {
            slots[index] = s;
        }
    }

    void Inventory::Clear() {
        for (auto& slot : slots) slot.Clear();
        selectedSlot = 0;
    }

    void Inventory::SelectPreviousSlot() {
        selectedSlot = (selectedSlot - 1 + HOTBAR_SIZE) % HOTBAR_SIZE;
    }

    void Inventory::SelectNextSlot() {
        selectedSlot = (selectedSlot + 1) % HOTBAR_SIZE;
    }

    int Inventory::AddToSlot(int slotIndex, ItemID id, int count) {
        if (slotIndex < 0 || slotIndex >= TOTAL_SIZE || count <= 0) return 0;
        const int maxStack = ItemRegistry::Get(id).maxStackSize;
        auto& slot = slots[slotIndex];
        if (slot.IsEmpty()) {
            slot.itemId = id;
            slot.count = std::min(count, maxStack);
            return slot.count;
        }
        if (slot.itemId == id) {
            int available = maxStack - slot.count;
            int toAdd = std::min(count, available);
            slot.count += toAdd;
            return toAdd;
        }
        return 0;
    }

} // namespace Game
