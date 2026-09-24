// File: src/common/inventory/Slot.cpp
#include "Slot.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include <algorithm>

namespace Game {

    int Slot::SafeInsert(ItemStack& input, int amount) {
        if (input.IsEmpty() || !MayPlace(input)) return 0;

        ItemStack& here = GetItemMut();
        // MC: min(min(amount, input.getCount()), getMaxStackSize(input) - slotStack.getCount())
        const int room = GetMaxStackSize(input) - here.count;
        const int moved = std::min({amount, input.count, room});
        if (moved <= 0) return 0;

        if (here.IsEmpty()) {
            ItemStack placed = input;      // component-preserving copy
            placed.count = moved;
            SetByPlayer(placed);
        } else if (IsSameItemSameComponents(here, input)) {
            here.count += moved;
            SetChanged();
        } else {
            // Occupied by something else — MC's safeInsert does nothing here.
            return 0;
        }

        input.count -= moved;
        if (input.count <= 0) input.Clear();
        return moved;
    }

    ItemStack Slot::SafeTake(int amount, int maxAmount) {
        if (!MayPickup()) return {};
        const ItemStack& here = GetItem();
        if (here.IsEmpty()) return {};
        // MC tryRemove: `!allowModification(player) && decrement < count`.
        if (!AllowModification() && maxAmount < here.count) return {};
        const int wanted = std::min(amount, maxAmount);
        if (wanted <= 0) return {};
        return Remove(wanted);
    }

    ItemStack Slot::Remove(int amount) {
        ItemStack& here = GetItemMut();
        const int taken = std::min(amount, here.count);
        if (here.IsEmpty() || taken <= 0) return {};
        ItemStack out = here;              // component-preserving copy
        out.count = taken;
        here.count -= taken;
        if (here.count <= 0) here.Clear();
        SetChanged();
        return out;
    }

    bool ArmorSlot::MayPlace(const ItemStack& stack) const {
        // MC ArmorSlot.mayPlace → the stack's EQUIPPABLE component must name
        // exactly this slot.
        auto equippable = stack.get(DataComponents::EQUIPPABLE);
        return equippable && InventoryIndexFor(equippable->slot) == containerSlot;
    }

} // namespace Game
