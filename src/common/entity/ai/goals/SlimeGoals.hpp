// File: src/common/entity/ai/goals/SlimeGoals.hpp
//
// MC Slime's four nested goals. A slime has no navigation — every goal here
// just feeds SlimeMoveControl a heading and arms its hop:
//
//   SlimeFloatGoal            bob and hop while in liquid (speed 1.2)
//   SlimeAttackGoal           face the target for up to 300 ticks; the
//                             aggressive flag triples the hop rate
//   SlimeRandomDirectionGoal  pick a random heading every 2-5 seconds
//   SlimeKeepOnJumpingGoal    the fallback that keeps the hop armed — a slime
//                             NEVER stands still
#pragma once

#include "common/entity/ai/Goal.hpp"

namespace Game {

    class Slime;

    class SlimeFloatGoal : public Goal {
    public:
        explicit SlimeFloatGoal(Slime* slime);
        bool CanUse() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        void Tick() override;
        const char* Name() const override { return "SlimeFloatGoal"; }
    private:
        Slime* m_slime;
    };

    class SlimeAttackGoal : public Goal {
    public:
        explicit SlimeAttackGoal(Slime* slime);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        void Tick() override;
        const char* Name() const override { return "SlimeAttackGoal"; }
    private:
        Slime* m_slime;
        int m_growTiredTimer = 0;
    };

    class SlimeRandomDirectionGoal : public Goal {
    public:
        explicit SlimeRandomDirectionGoal(Slime* slime);
        bool CanUse() override;
        void Tick() override;
        const char* Name() const override { return "SlimeRandomDirectionGoal"; }
    private:
        Slime* m_slime;
        float m_chosenDegrees = 0.0f;
        int m_nextRandomizeTime = 0;
    };

    class SlimeKeepOnJumpingGoal : public Goal {
    public:
        explicit SlimeKeepOnJumpingGoal(Slime* slime);
        bool CanUse() override;
        void Tick() override;
        const char* Name() const override { return "SlimeKeepOnJumpingGoal"; }
    private:
        Slime* m_slime;
    };

} // namespace Game
