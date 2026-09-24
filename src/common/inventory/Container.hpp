// File: src/common/inventory/Container.hpp
//
// Mirrors net.minecraft.world.Container — the raw backing store behind a
// menu's slots. A Container knows how to read/write its own indices and
// nothing else: no GUI position, no placement policy, no click handling. All
// of that lives on Slot (Slot.hpp), which points AT a container index.
//
// This separation is what lets one menu span several containers. MC's
// ChestMenu, for example, builds slots over both the chest's Container and the
// player's Inventory; the click code never needs to know which is which.
// Today the only implementation is Game::Inventory (the player's 46 slots) —
// exactly as MC's `Inventory implements Container`.
#pragma once

#include "common/entity/Item.hpp"
#include "common/world/block/Direction.hpp"

#include <algorithm>
#include <vector>

namespace Game {

    class IContainer {
    public:
        virtual ~IContainer() = default;

        virtual int GetContainerSize() const = 0;

        // MC Container.isEmpty.
        virtual bool IsEmpty() const {
            const int n = GetContainerSize();
            for (int i = 0; i < n; ++i) if (!GetItem(i).IsEmpty()) return false;
            return true;
        }

        // MC Container.getMaxStackSize(ItemStack): the container's ceiling
        // capped by the item's own.
        int GetMaxStackSize(const ItemStack& stack) const {
            return std::min(GetMaxStackSize(), ItemRegistry::Get(stack.itemId).maxStackSize);
        }

        // MC Container.canPlaceItem(slot, stack) — may this go here at all?
        // A furnace refuses anything but fuel in its fuel slot and anything
        // in its output slot. Hoppers and droppers consult it.
        virtual bool CanPlaceItem(int /*slot*/, const ItemStack& /*stack*/) const { return true; }

        // MC Container.canTakeItem(target, slot, stack) — may a hopper pull
        // this out? True everywhere in vanilla but the brewing stand.
        virtual bool CanTakeItem(const IContainer& /*target*/, int /*slot*/,
                                 const ItemStack& /*stack*/) const { return true; }

        // MC Container.removeItem(slot, count): split `count` off the slot.
        ItemStack RemoveItem(int slot, int count) {
            ItemStack& current = GetItem(slot);
            if (current.IsEmpty() || count <= 0) return ItemStack{};
            ItemStack taken = current;
            taken.count = std::min(count, current.count);
            current.count -= taken.count;
            if (current.count <= 0) current.Clear();
            SetChanged();
            return taken;
        }

        virtual ItemStack&       GetItem(int index)       = 0;
        virtual const ItemStack& GetItem(int index) const = 0;
        virtual void             SetItem(int index, const ItemStack& stack) = 0;

        // MC Container.getMaxStackSize — the container's own ceiling, before
        // the per-item limit is applied. Slot::GetMaxStackSize(stack) takes the
        // min of the two.
        virtual int GetMaxStackSize() const { return 64; }

        // MC Container.setChanged — "something in here was mutated". The
        // player inventory has nothing to do here (the per-tick container diff
        // in PlayerSession finds changes on its own); block containers will use
        // it to mark their block entity dirty for saving.
        virtual void SetChanged() {}
    };

    // MC WorldlyContainer — a container whose slots are reachable only from
    // certain faces (the furnace: input from above, fuel from the sides,
    // output out of the bottom). Hoppers and droppers ask through this.
    class IWorldlyContainer {
    public:
        virtual ~IWorldlyContainer() = default;
        virtual void GetSlotsForFace(Direction face, std::vector<int>& out) const = 0;
        // `hasDirection` false is vanilla's null direction (a hopper's own
        // pull into itself).
        virtual bool CanPlaceItemThroughFace(int slot, const ItemStack& stack,
                                             Direction face, bool hasDirection) const = 0;
        virtual bool CanTakeItemThroughFace(int slot, const ItemStack& stack, Direction face) const = 0;
    };

} // namespace Game
