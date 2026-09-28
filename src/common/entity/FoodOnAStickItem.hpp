// File: src/common/entity/FoodOnAStickItem.hpp
//
// MC net.minecraft.world.item.FoodOnAStickItem — carrot on a stick (boosts a
// ridden pig, 7 durability a use) and warped fungus on a stick (a ridden
// strider, 1 a use). Using the rod while it steers the matching mount starts
// the mount's ItemBasedSteering burst; a rod worn out by it turns into a
// fishing rod carrying its components.
#pragma once

#include "common/entity/Item.hpp"

#include <unordered_map>

namespace Game::FoodOnAStickItem {

    // Wires Item::use on both rods (called from ItemRegistry_RegisterBehaviors).
    void RegisterBehaviors(std::unordered_map<ItemID, Item>& pureItems);

} // namespace Game::FoodOnAStickItem
