// File: src/common/entity/ai/brain/ZombieNautilusAi.hpp
//
// MC net.minecraft.world.entity.animal.nautilus.ZombieNautilusAi — the
// nautilus brain minus breeding and panic: it drifts, follows temptation
// (slower), and runs the same charge attack off NautilusAi's target finder,
// at 0.5 speed.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class ZombieNautilus;
    namespace ZombieNautilusAi {
        void InitBrain(ZombieNautilus& nautilus, Brain& brain);
        void UpdateActivity(ZombieNautilus& nautilus);
    }
}
