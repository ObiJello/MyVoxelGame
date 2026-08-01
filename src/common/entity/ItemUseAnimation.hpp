// File: src/common/entity/ItemUseAnimation.hpp
//
// Mirrors net/minecraft/world/item/ItemUseAnimation.java — the first-person
// arm pose used while an item is being "used" (held right-click). Ids match
// ItemUseAnimation.java:12-23 exactly (they cross the network via
// DataComponents' Consumable codec, so they must stay stable).
#pragma once

#include <cstdint>

namespace Game {

    enum class ItemUseAnimation : uint8_t {
        NONE      = 0,
        EAT       = 1,
        DRINK     = 2,
        BLOCK     = 3,
        BOW       = 4,
        TRIDENT   = 5,
        CROSSBOW  = 6,
        SPYGLASS  = 7,
        TOOT_HORN = 8,
        BRUSH     = 9,
        BUNDLE    = 10,
        SPEAR     = 11,
    };

    // Mirrors ItemUseAnimation.hasCustomArmTransform() (:30, :51-53) — true
    // for the poses that replace the normal arm transform entirely
    // (EAT / DRINK / SPEAR).
    inline bool HasCustomArmTransform(ItemUseAnimation anim) {
        return anim == ItemUseAnimation::EAT
            || anim == ItemUseAnimation::DRINK
            || anim == ItemUseAnimation::SPEAR;
    }

} // namespace Game
