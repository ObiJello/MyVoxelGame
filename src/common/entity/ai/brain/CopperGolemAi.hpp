// File: src/common/entity/ai/brain/CopperGolemAi.hpp
//
// MC net.minecraft.world.entity.animal.golem.CopperGolemAi, plus the one
// behaviour it exists for: ai/behavior/TransportItemsBetweenContainers. MC
// keeps that behaviour public, but the copper golem is its only constructor,
// so it lives in the .cpp's anonymous namespace the way each <Mob>Ai keeps
// its private behaviours.
//
// The loop that makes a copper golem a copper golem: with an empty hand it
// walks to the nearest unvisited chest, faces it for a 60-tick interaction,
// and takes up to 16 items of the first stack; holding something it carries
// the stack to a chest with room and puts it down. Chests it has handled (or
// failed to reach) are remembered for 6000 ticks; running out of candidates
// costs a 140-tick cooldown, during which it strolls and head-spins.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class CopperGolem;
    namespace CopperGolemAi {
        void InitBrain(CopperGolem& golem, Brain& brain);
        void UpdateActivity(CopperGolem& golem);
    }
}
