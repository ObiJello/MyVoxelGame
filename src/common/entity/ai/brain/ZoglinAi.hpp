// File: src/common/entity/ai/brain/ZoglinAi.hpp
//
// MC net.minecraft.world.entity.monster.Zoglin — the brain half. MC keeps the
// zoglin's activities as private statics on the entity class rather than in a
// separate <Mob>Ai file; they are split out here so the port's one-Ai-file-
// per-brain-mob shape holds.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Zoglin;
    namespace ZoglinAi {
        void InitBrain(Zoglin& zoglin, Brain& brain);
        void UpdateActivity(Zoglin& zoglin);

        // MC Zoglin.setAttackTarget — the hurtServer path: erase the
        // can't-reach clock and remember the attacker for 200 ticks.
        void SetAttackTarget(Zoglin& zoglin, LivingEntity& target);
    }
}
