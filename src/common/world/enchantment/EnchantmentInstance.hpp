// File: src/common/world/enchantment/EnchantmentInstance.hpp
//
// Mirrors net/minecraft/world/item/enchantment/EnchantmentInstance.java — a
// (id, level) pair. MC's struct holds a Holder<Enchantment> + int level; we
// store the registry id directly since our registry is a flat vector.
#pragma once

#include "Enchantment.hpp"

namespace Game {

    struct EnchantmentInstance {
        EnchantmentId id;
        int           level;
    };

} // namespace Game
