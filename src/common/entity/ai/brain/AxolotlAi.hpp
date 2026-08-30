// File: src/common/entity/ai/brain/AxolotlAi.hpp
//
// MC net.minecraft.world.entity.animal.axolotl.AxolotlAi, plus the two
// behaviours MC keeps beside it in the axolotl package (PlayDead,
// ValidatePlayDead — both local classes in the .cpp here).
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Axolotl;
    namespace AxolotlAi {
        void InitBrain(Axolotl& axolotl, Brain& brain);

        // MC AxolotlAi.updateActivity — PLAY_DEAD is sticky: once active, only
        // ValidatePlayDead's useDefaultActivity leaves it. Leaving FIGHT any
        // other way arms the 2400-tick hunting cooldown.
        void UpdateActivity(Axolotl& axolotl);
    }
}
