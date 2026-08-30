// File: src/common/entity/ai/brain/BreezeAi.hpp
//
// MC net.minecraft.world.entity.monster.breeze.BreezeAi, plus the five
// behaviours that live beside it (Shoot, Slide, LongJump, ShootWhenStuck,
// SlideToTargetSink) — all package-private in MC, all in the .cpp here.
//
// The breeze's whole fight is a memory-driven cycle: Slide picks a point in
// one of three rings around the target and walks there (pose SLIDING via
// SlideToTargetSink); arriving arms BREEZE_SHOOT; LongJump inhales for 10
// ticks (pose INHALING) then ballistic-jumps to a point behind the target
// (pose LONG_JUMPING), and landing arms BREEZE_SHOOT too; Shoot charges for
// 15 ticks (pose SHOOTING), fires the wind charge, recovers for 4, cools for
// 10. Every clip the client plays is derived from those poses.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Breeze;
    namespace BreezeAi {
        void InitBrain(Breeze& breeze, Brain& brain);
        void UpdateActivity(Breeze& breeze);
    }
}
