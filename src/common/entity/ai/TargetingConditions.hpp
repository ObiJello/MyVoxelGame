// File: src/common/entity/ai/TargetingConditions.hpp
//
// MC net.minecraft.world.entity.ai.targeting.TargetingConditions — the shared
// "may this entity be considered right now" filter.
//
// MC has two presets and they differ in more than name:
//   forCombat()    — requires line of sight and respects invisibility
//   forNonCombat() — neither, because a mob turning its head to look at you
//                    should not need to see you first
// Getting that backwards makes mobs stare through walls, or refuse to notice a
// player standing in the open.
#pragma once

namespace Game {

    class LivingEntity;

    struct TargetingConditions {
        double range = -1.0;              // < 0 means unlimited
        bool   requiresLineOfSight = true;
        bool   testInvisible = true;

        static TargetingConditions ForCombat() {
            return TargetingConditions{ -1.0, true, true };
        }
        static TargetingConditions ForNonCombat() {
            return TargetingConditions{ -1.0, false, false };
        }

        TargetingConditions& Range(double r) { range = r; return *this; }
        TargetingConditions& IgnoreLineOfSight() { requiresLineOfSight = false; return *this; }
        TargetingConditions& IgnoreInvisibility() { testInvisible = false; return *this; }

        // `attacker` may be null for a pure range test.
        bool Test(LivingEntity* attacker, const LivingEntity& target) const;
    };

} // namespace Game
