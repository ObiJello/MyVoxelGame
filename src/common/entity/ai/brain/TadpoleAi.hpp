// File: src/common/entity/ai/brain/TadpoleAi.hpp
//
// MC net.minecraft.world.entity.animal.frog.TadpoleAi. The smallest brain in
// the game — every behaviour it uses is shared, so this is what a mob costs
// once the common set exists.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Tadpole;
    namespace TadpoleAi {
        void InitBrain(Tadpole& tadpole, Brain& brain);
        void UpdateActivity(Tadpole& tadpole);
    }
}
