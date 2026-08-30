// File: src/common/entity/ai/goals/HappyGhastGoals.cpp
#include "common/entity/ai/goals/HappyGhastGoals.hpp"

#include "common/entity/mobs/AnimatedMobs.hpp"

namespace Game {

    HappyGhastFloatGoal::HappyGhastFloatGoal(HappyGhast* ghast)
        : FloatGoal(ghast) {
        // MC gates canUse on !isOnStillTimeout(); the timeout is riding-only
        // and permanently false here, so the base predicate is exact.
    }

} // namespace Game
