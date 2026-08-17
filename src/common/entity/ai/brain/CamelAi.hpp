// File: src/common/entity/ai/brain/CamelAi.hpp
//
// MC net.minecraft.world.entity.animal.camel.CamelAi.
//
// The camel is the mob whose dead animations the brain actually revives. Its
// sit, sit-pose and stand-up clips are driven by the SITTING pose, and MC sets
// that pose from RandomSitting — an IDLE behaviour that lies a camel down of
// its own accord every twenty seconds or so. Nothing else in the game does it
// except a player right-clicking, which is why these clips looked like they
// needed riding and did not.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {

    class Camel;

    namespace CamelAi {
        void InitBrain(Camel& camel, Brain& brain);
        void UpdateActivity(Camel& camel);
    }

} // namespace Game
