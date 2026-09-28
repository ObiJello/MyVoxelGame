// File: src/common/inventory/SimpleContainerOps.hpp
//
// MC SimpleContainer.addItem / canAddItem / removeAllItems — the three
// operations an InventoryCarrier mob (the piglin, the pillager) runs on its
// pocket inventory. The same rules AbstractVillager::AddToInventory spells
// out: merge into matching stacks first, then the first empty slot, and the
// remainder comes back.
#pragma once

#include "common/entity/Item.hpp"
#include "common/inventory/SimpleContainer.hpp"

#include <algorithm>
#include <vector>

namespace Game::SimpleContainerOps {

    // MC SimpleContainer.addItem — returns what did not fit.
    inline ItemStack AddItem(SimpleContainer& container, const ItemStack& stack) {
        if (stack.IsEmpty()) return {};
        ItemStack rest = stack;
        const int maxStack = container.GetMaxStackSize(rest);
        for (int i = 0; i < container.GetContainerSize() && !rest.IsEmpty(); ++i) {
            ItemStack& slot = container.GetItem(i);
            if (slot.IsEmpty() || !IsSameItemSameComponents(slot, rest)) continue;
            const int moved = std::min(rest.count, maxStack - slot.count);
            if (moved <= 0) continue;
            slot.count += moved;
            rest.count -= moved;
        }
        for (int i = 0; i < container.GetContainerSize() && !rest.IsEmpty(); ++i) {
            if (!container.GetItem(i).IsEmpty()) continue;
            container.SetItem(i, rest);
            rest.Clear();
        }
        if (rest.count <= 0) rest.Clear();
        return rest;
    }

    // MC SimpleContainer.canAddItem — an empty slot, or a matching stack with
    // room.
    inline bool CanAddItem(const SimpleContainer& container, const ItemStack& stack) {
        for (int i = 0; i < container.GetContainerSize(); ++i) {
            const ItemStack& slot = container.GetItem(i);
            if (slot.IsEmpty()) return true;
            if (IsSameItemSameComponents(slot, stack) && slot.count < container.GetMaxStackSize(slot)) {
                return true;
            }
        }
        return false;
    }

    // MC SimpleContainer.removeAllItems — every non-empty stack, the
    // container left empty.
    inline std::vector<ItemStack> RemoveAllItems(SimpleContainer& container) {
        std::vector<ItemStack> out;
        for (int i = 0; i < container.GetContainerSize(); ++i) {
            if (!container.GetItem(i).IsEmpty()) out.push_back(container.GetItem(i));
            container.SetItem(i, ItemStack{});
        }
        return out;
    }

} // namespace Game::SimpleContainerOps
