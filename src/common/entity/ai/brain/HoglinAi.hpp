// File: src/common/entity/ai/brain/HoglinAi.hpp
//
// MC net.minecraft.world.entity.monster.hoglin.HoglinAi.
//
// The first HOSTILE brain mob in this port, and the one that exercises the
// combat behaviours: FIGHT is an activity gated on ATTACK_TARGET, and the
// activity erases that memory when it stops, so the mob cannot be left angry at
// something it has forgotten about.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Hoglin;
    namespace HoglinAi {
        void InitBrain(Hoglin& hoglin, Brain& brain);
        void UpdateActivity(Hoglin& hoglin);
    }
}
