// File: src/common/entity/ai/brain/CreakingAi.hpp
//
// MC net.minecraft.world.entity.monster.creaking.CreakingAi.
//
// The smallest fight brain in the game: IDLE strolls at 0.3 and starts
// attacking whatever the freeze gate activated on; FIGHT walks at 1.0 and
// swings every 40 ticks — but ONLY while canMove is true. The freeze itself
// is not a behaviour: Creaking.checkCanMove runs in aiStep and the activity
// switch here simply falls back to IDLE while frozen, exactly as in MC.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Creaking;
    namespace CreakingAi {
        void InitBrain(Creaking& creaking, Brain& brain);
        void UpdateActivity(Creaking& creaking);
    }
}
