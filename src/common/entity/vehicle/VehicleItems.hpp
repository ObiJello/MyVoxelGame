// File: src/common/entity/vehicle/VehicleItems.hpp
//
// The items that place vehicles — MC BoatItem (every boat, chest boat and
// raft) and MinecartItem (the seven carts) — and their dispenser behaviours
// (BoatDispenseItemBehavior, MinecartDispenseItemBehavior).
#pragma once

#include "common/entity/Item.hpp"

#include <unordered_map>

namespace Game {

    struct DispenseSource;

    // Is `item` a boat / chest boat / raft or a minecart item?
    bool IsVehicleItem(ItemID item);

    // Wire BoatItem.use and MinecartItem.useOn onto their items (called from
    // the item registry's behaviour pass).
    void ItemRegistry_RegisterVehicleItems(std::unordered_map<ItemID, Item>& pureItems);

    // The execute() half of the boat / minecart dispense behaviour: the
    // vehicle placed in front of the dispenser (the stack shrinks), or the
    // default behaviour's drop when there is nowhere to put it. The caller
    // plays the behaviour's own sound (levelEvent 1000) and smoke after.
    ItemStack DispenseVehicleItem(const DispenseSource& source, ItemStack dispensed);

} // namespace Game
