// File: src/common/entity/ai/brain/GoatAi.hpp
//
// MC net.minecraft.world.entity.animal.goat.GoatAi.
#pragma once

#include "common/entity/ai/brain/Brain.hpp"

namespace Game {
    class Goat;
    namespace GoatAi {
        // MC TIME_BETWEEN_LONG_JUMPS = UniformInt.of(600, 1200) — goats jump
        // far less often than frogs, and much further (5x5 instead of 4x2).
        inline constexpr int kTimeBetweenLongJumpsMin = 600;
        inline constexpr int kTimeBetweenLongJumpsMax = 1200;

        void InitBrain(Goat& goat, Brain& brain);
        void InitMemories(Goat& goat);
        void UpdateActivity(Goat& goat);
    }
}
