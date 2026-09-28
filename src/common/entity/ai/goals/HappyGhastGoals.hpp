// File: src/common/entity/ai/goals/HappyGhastGoals.hpp
//
// MC animal/ghast/HappyGhast.java's nested HappyGhastFloatGoal. It lives
// here because every goal class in this port does.
#pragma once

#include "common/entity/ai/goals/BasicGoals.hpp"

namespace Game {

    class HappyGhast;

    // MC HappyGhast.HappyGhastFloatGoal — FloatGoal gated off during the
    // still timeout (a player standing on it or boarding it: a parked
    // platform does not bob up out of water).
    class HappyGhastFloatGoal : public FloatGoal {
    public:
        explicit HappyGhastFloatGoal(HappyGhast* ghast);
        bool CanUse() override;
        const char* Name() const override { return "HappyGhastFloatGoal"; }

    private:
        HappyGhast* m_ghast;
    };

} // namespace Game
