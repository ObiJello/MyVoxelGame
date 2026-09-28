// File: src/common/entity/ai/goals/HappyGhastGoals.cpp
#include "common/entity/ai/goals/HappyGhastGoals.hpp"

#include "common/entity/mobs/AnimatedMobs.hpp"

namespace Game {

    HappyGhastFloatGoal::HappyGhastFloatGoal(HappyGhast* ghast)
        : FloatGoal(ghast), m_ghast(ghast) {}

    bool HappyGhastFloatGoal::CanUse() {
        // MC: `!isOnStillTimeout() && super.canUse()`.
        return !m_ghast->IsOnStillTimeout() && FloatGoal::CanUse();
    }

} // namespace Game
