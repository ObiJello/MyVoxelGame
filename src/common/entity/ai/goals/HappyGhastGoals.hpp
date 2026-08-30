// File: src/common/entity/ai/goals/HappyGhastGoals.hpp
//
// MC animal/ghast/HappyGhast.java's nested HappyGhastFloatGoal. It lives
// here because every goal class in this port does.
#pragma once

#include "common/entity/ai/goals/BasicGoals.hpp"

namespace Game {

    class HappyGhast;

    // MC HappyGhast.HappyGhastFloatGoal — FloatGoal gated off during the
    // still timeout (the harness/rider "parked" state). The still timeout is
    // only ever set by the riding system, which this port does not model, so
    // the gate always passes and the base float behaviour is exact.
    class HappyGhastFloatGoal : public FloatGoal {
    public:
        explicit HappyGhastFloatGoal(HappyGhast* ghast);
        const char* Name() const override { return "HappyGhastFloatGoal"; }
    };

} // namespace Game
