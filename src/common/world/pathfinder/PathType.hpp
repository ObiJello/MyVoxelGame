// File: src/common/world/pathfinder/PathType.hpp
//
// MC net.minecraft.world.level.pathfinder.PathType — what a block position
// means to a walking mob, and what it costs to path through.
//
// The malus column is the whole tuning of MC's navigation:
//   * NEGATIVE means impassable. The A* never expands into it.
//   * ZERO means free.
//   * POSITIVE is an additive penalty on g, so a mob will take a detour up to
//     that many blocks long to avoid it. Water at 8 is why land mobs walk
//     around a pond rather than through it, and DAMAGE_FIRE at 16 is why they
//     give lava a wide berth without it being forbidden outright.
//
// Individual mobs override entries via Mob::SetPathfindingMalus — a chicken
// sets WATER to 0 (it floats), an Animal sets DAMAGE_FIRE to -1 (never).
#pragma once

#include <cstdint>

namespace Game {

    enum class PathType : uint8_t {
        Blocked = 0,
        Open,
        Walkable,
        WalkableDoor,
        Trapdoor,
        PowderSnow,
        DangerPowderSnow,
        Fence,
        Lava,
        Water,
        WaterBorder,
        Rail,
        UnpassableRail,
        DangerFire,
        DamageFire,
        DangerOther,
        DamageOther,
        DoorOpen,
        DoorWoodClosed,
        DoorIronClosed,
        Breach,
        Leaves,
        StickyHoney,
        Cocoa,
        DamageCautious,
        DangerTrapdoor,
        Count
    };

    // PathType.java, in enum order.
    inline constexpr float kPathTypeMalus[] = {
        /* Blocked          */ -1.0f,
        /* Open             */  0.0f,
        /* Walkable         */  0.0f,
        /* WalkableDoor     */  0.0f,
        /* Trapdoor         */  0.0f,
        /* PowderSnow       */ -1.0f,
        /* DangerPowderSnow */  0.0f,
        /* Fence            */ -1.0f,
        /* Lava             */ -1.0f,
        /* Water            */  8.0f,
        /* WaterBorder      */  8.0f,
        /* Rail             */  0.0f,
        /* UnpassableRail   */ -1.0f,
        /* DangerFire       */  8.0f,
        /* DamageFire       */ 16.0f,
        /* DangerOther      */  8.0f,
        /* DamageOther      */ -1.0f,
        /* DoorOpen         */  0.0f,
        /* DoorWoodClosed   */ -1.0f,
        /* DoorIronClosed   */ -1.0f,
        /* Breach           */  4.0f,
        /* Leaves           */ -1.0f,
        /* StickyHoney      */  8.0f,
        /* Cocoa            */  0.0f,
        /* DamageCautious   */  0.0f,
        /* DangerTrapdoor   */  0.0f,
    };

    static_assert(sizeof(kPathTypeMalus) / sizeof(kPathTypeMalus[0]) ==
                      static_cast<size_t>(PathType::Count),
                  "kPathTypeMalus must stay in sync with PathType");

    inline float GetDefaultPathMalus(PathType t) {
        return kPathTypeMalus[static_cast<size_t>(t)];
    }

} // namespace Game
