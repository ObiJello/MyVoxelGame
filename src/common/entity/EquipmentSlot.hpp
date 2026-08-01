// File: src/common/entity/EquipmentSlot.hpp
//
// Mirrors net/minecraft/world/entity/EquipmentSlot.java — where a stack can
// be worn/held. Ordinals match MC's declaration order (they cross the wire
// inside the EQUIPPABLE component codec).
#pragma once

#include <cstdint>

namespace Game {

    enum class EquipmentSlot : uint8_t {
        MAINHAND = 0,
        OFFHAND  = 1,
        FEET     = 2,
        LEGS     = 3,
        CHEST    = 4,
        HEAD     = 5,
        BODY     = 6,   // animal armor (llama carpet, wolf armor) — data-only
        SADDLE   = 7,   // data-only
    };

    // Unified 46-slot inventory index for a wearable slot (Inventory.hpp
    // layout: armor 5-8 = helmet, chest, legs, boots; offhand 45).
    // MAINHAND and the entity-only slots return -1 (no fixed index — the
    // main hand is 36 + selected hotbar slot).
    inline int InventoryIndexFor(EquipmentSlot slot) {
        switch (slot) {
            case EquipmentSlot::HEAD:    return 5;
            case EquipmentSlot::CHEST:   return 6;
            case EquipmentSlot::LEGS:    return 7;
            case EquipmentSlot::FEET:    return 8;
            case EquipmentSlot::OFFHAND: return 45;
            default:                     return -1;
        }
    }

} // namespace Game
