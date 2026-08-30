// File: src/common/entity/ai/brain/HappyGhastAi.hpp
//
// MC net.minecraft.world.entity.animal.happyghast.HappyGhastAi.
//
// The split MC actually ships: the ADULT happy ghast is goal-driven (float,
// tempt, the ghast wander — the class keeps those), and the BRAIN runs only
// for the BABY ghastling — tempt-following, trailing the nearest player or
// any followable adult, the flying wander, and panic. The HappyGhast class
// swaps between the two setups at the age boundary exactly as MC's
// adultGhastSetup/babyGhastSetup do.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class HappyGhast;
    namespace HappyGhastAi {
        void InitBrain(HappyGhast& ghast, Brain& brain);
        void UpdateActivity(HappyGhast& ghast);
    }
}
