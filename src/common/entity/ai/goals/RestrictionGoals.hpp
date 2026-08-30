// File: src/common/entity/ai/goals/RestrictionGoals.hpp
//
// MC net.minecraft.world.entity.ai.goal.MoveTowardsRestrictionGoal — the goal
// that walks a mob back inside its home radius (Mob::SetHomeTo). Registered by
// Guardian (5), ElderGuardian (via Guardian) and Blaze (5) in MC; for a mob
// with no home set the canUse gate never opens (IsWithinHome is true
// everywhere), so it sits dormant exactly as it does in vanilla until
// something calls SetHomeTo — today that is the elder guardian's aura tick.
#pragma once

#include "common/entity/ai/Goal.hpp"

namespace Game {

    class PathfinderMob;

    class MoveTowardsRestrictionGoal : public Goal {
    public:
        MoveTowardsRestrictionGoal(PathfinderMob* mob, double speedModifier);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        const char* Name() const override { return "MoveTowardsRestrictionGoal"; }

    private:
        PathfinderMob* m_mob;
        double m_wantedX = 0.0;
        double m_wantedY = 0.0;
        double m_wantedZ = 0.0;
        double m_speedModifier;
    };

} // namespace Game
