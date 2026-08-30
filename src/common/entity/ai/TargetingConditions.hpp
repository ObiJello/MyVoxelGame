// File: src/common/entity/ai/TargetingConditions.hpp
//
// MC net.minecraft.world.entity.ai.targeting.TargetingConditions — the shared
// "may this entity be considered right now" filter.
//
// MC has two presets. Both keep line of sight AND invisibility checks ON
// (the defaults) — the proof is BreedGoal/TemptGoal calling
// .ignoreLineOfSight() explicitly on a forNonCombat() instance, which would
// be a no-op otherwise. What forNonCombat() actually flips is the COMBAT
// half: the attackability / Peaceful-difficulty gates that decide whether the
// target may be fought, which a mob merely turning its head does not need.
#pragma once

namespace Game {

    class LivingEntity;

    struct TargetingConditions {
        double range = -1.0;              // < 0 means unlimited
        bool   isCombat = true;           // MC forCombat vs forNonCombat
        bool   requiresLineOfSight = true;
        bool   testInvisible = true;

        static TargetingConditions ForCombat() {
            return TargetingConditions{ -1.0, true, true, true };
        }
        static TargetingConditions ForNonCombat() {
            return TargetingConditions{ -1.0, false, true, true };
        }

        TargetingConditions& Range(double r) { range = r; return *this; }
        TargetingConditions& IgnoreLineOfSight() { requiresLineOfSight = false; return *this; }
        TargetingConditions& IgnoreInvisibility() { testInvisible = false; return *this; }

        // `attacker` may be null for a pure range test.
        bool Test(LivingEntity* attacker, const LivingEntity& target) const;
    };

} // namespace Game
