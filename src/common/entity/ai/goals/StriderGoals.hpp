// File: src/common/entity/ai/goals/StriderGoals.hpp
//
// MC Strider.java's nested StriderGoToLavaGoal. MC nests it inside
// Strider.java; it lives here because every goal class in this port does.
#pragma once

#include "common/entity/ai/goals/MoveToBlockGoal.hpp"

namespace Game {

    class Strider;

    // MC Strider.StriderGoToLavaGoal — MoveToBlockGoal(strider, speed, 8, 2)
    // hunting the nearest lava block with a pathable cell above it; only
    // while the strider is out of lava, repathing every 20 ticks.
    class StriderGoToLavaGoal : public MoveToBlockGoal {
    public:
        StriderGoToLavaGoal(Strider* strider, double speedModifier);

        bool CanUse() override;
        bool CanContinueToUse() override;
        bool ShouldRecalculatePath() const override { return m_tryTicks % 20 == 0; }
        const char* Name() const override { return "StriderGoToLavaGoal"; }

    protected:
        // MC getMoveToTarget returns blockPos itself — the strider walks ONTO
        // the lava rather than to the block above it.
        glm::ivec3 GetMoveToTarget() const override { return m_blockPos; }

        bool IsValidTarget(const IBlockAccess& blocks,
                           const glm::ivec3& pos) const override;

    private:
        Strider* m_strider;
    };

} // namespace Game
