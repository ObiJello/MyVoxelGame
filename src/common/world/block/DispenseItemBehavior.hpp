// File: src/common/world/block/DispenseItemBehavior.hpp
//
// MC net.minecraft.core.dispenser — the per-item behaviours a dispenser runs
// (DispenseItemBehavior.bootStrap) and the default "spit it out" one.
//
// Ported: the default drop, projectiles (arrows, snowballs, eggs), filled and
// empty buckets, flint and steel, bone meal, TNT, and spawn eggs. Not
// ported, and falling back to the default drop: boats and minecarts (no
// such entities), armour equipping, shears, shulker box placement, honeycomb
// waxing, bottles, the wither-skull and pumpkin golem builders.
#pragma once

#include "common/entity/Item.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;
    class DispenserBlockEntity;

    // MC BlockSource.
    struct DispenseSource {
        ILevelWrite&          level;
        glm::ivec3            pos;
        BlockState            state;
        DispenserBlockEntity& blockEntity;
        glm::dvec3 Center() const { return glm::dvec3(pos) + glm::dvec3(0.5); }
    };

    // MC DispenserBlock.getDispenseMethod(level, stack).dispense(source, stack):
    // runs the item's behaviour and returns what stays in the slot.
    ItemStack DispenseItem(const DispenseSource& source, ItemStack stack);

    // MC DefaultDispenseItemBehavior.dispense — the dropper's only behaviour.
    ItemStack DispenseDefault(const DispenseSource& source, ItemStack stack);

} // namespace Game
