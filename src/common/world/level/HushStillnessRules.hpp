// File: src/common/world/level/HushStillnessRules.hpp
//
// What the Hush's "stillness" does to a mob (docs/the-hush.md, Atmosphere).
//
// The event itself is server-owned — Server::HushStillness runs the timer
// and raises EntityLevel::IsStilled() on the Hush level's bridge while one
// is on. The mob side is ONE early-out at the top of Mob::ServerAiStep:
// while the level is stilled, a mob this file does not exempt skips its whole
// AI step — sensing, target and goal selectors, navigation, brain,
// CustomServerAiStep (so a creeper's fuse and a wraith's charge pause too)
// and the move/look/jump controls — and HoldStill zeroes what would still
// move it. Nothing is cleared: the path, the goals and the target are all
// where they were, so the mob picks up exactly where it stopped when the
// stillness lifts.
//
// What still runs: LivingEntity's own tick (hurt clocks, fire, effects,
// death), gravity for a mob that has it, and being pushed or knocked back.
// A frozen mob can be hit and killed; it simply does not answer.
#pragma once

#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/LivingEntity.hpp"

namespace Game::HushStillnessRules {

    // The bosses keep moving: a stillness must never hand a free kill on the
    // Silent Warden (the Hush's own boss) or the two vanilla ones a player
    // might bring through the frame.
    inline bool IsImmune(EntityTypeId type) {
        return type == EntityTypeId::SilentWarden
            || type == EntityTypeId::TheUnsung       // Aurelith's boss
            || type == EntityTypeId::EnderDragon
            || type == EntityTypeId::Wither;
    }

    // "Freeze in place": no steering input, no jump, and no carried
    // momentum. Vertical speed is kept for a mob under gravity (it settles to
    // the ground rather than hanging mid-hop) and zeroed for one without (a
    // flying wraith or a bat stops dead in the air).
    inline void HoldStill(LivingEntity& mob) {
        mob.xxa = 0.0f;
        mob.yya = 0.0f;
        mob.zza = 0.0f;
        mob.jumping = false;
        mob.velocity.x = 0.0;
        mob.velocity.z = 0.0;
        if (mob.IsNoGravity()) mob.velocity.y = 0.0;
    }

} // namespace Game::HushStillnessRules
