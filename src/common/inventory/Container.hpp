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

namespace Game {

    class IContainer {
    public:
        virtual ~IContainer() = default;

        virtual int GetContainerSize() const = 0;

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

} // namespace Game
