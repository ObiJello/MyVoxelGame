// File: src/common/entity/ai/brain/FrogAi.hpp
//
// MC net.minecraft.world.entity.animal.frog.FrogAi.
//
// The frog runs on the brain and NOTHING ELSE — it has no goals, exactly as in
// MC, where Frog never overrides registerGoals. That matters beyond tidiness:
// MC's Croak is gated on WALK_TARGET being absent, and "absent" only means "not
// walking" if every movement the frog makes goes through a walk target. With a
// goal-driven stroll running alongside, it did not, and the frog croaked while
// walking.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {

    class Frog;

    namespace FrogAi {

        // MC TIME_BETWEEN_LONG_JUMPS = UniformInt.of(100, 140).
        inline constexpr int kTimeBetweenLongJumpsMin = 100;
        inline constexpr int kTimeBetweenLongJumpsMax = 140;

        void InitBrain(Frog& frog, Brain& brain);

        // MC FrogAi.initMemories — the frog starts on a long-jump cooldown so a
        // freshly spawned one does not leap the instant it appears.
        void InitMemories(Frog& frog);

        // MC FrogAi.updateActivity. The ORDER is the behaviour: TONGUE beats
        // LONG_JUMP beats SWIM beats IDLE.
        void UpdateActivity(Frog& frog);

    } // namespace FrogAi

} // namespace Game
