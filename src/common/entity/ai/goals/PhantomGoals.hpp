// File: src/common/entity/ai/goals/PhantomGoals.hpp
//
// MC Phantom.java's nested AI: the three custom controls and the four goals
// that run the CIRCLE/SWOOP state machine. MC nests all of them inside
// Phantom.java; they live here because every goal class in this port does,
// and the controls come along because Monsters.cpp needs to construct them.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/TargetingConditions.hpp"

namespace Game {

    class Phantom;

    // MC Phantom.PhantomMoveControl — no navigation: every tick it banks the
    // yaw toward moveTargetPoint at 4 degrees, accelerates toward 1.8 while
    // lined up (dropping back to 0.2 while turning), and lerps the velocity
    // a fifth of the way toward the desired vector. A horizontal collision
    // flips it 180 degrees.
    class PhantomMoveControl : public MoveControl {
    public:
        explicit PhantomMoveControl(Phantom* phantom);
        void Tick() override;

    private:
        Phantom* m_phantom;
        float m_speed = 0.1f;
    };

    // MC Phantom.PhantomLookControl — tick() is empty: the move control owns
    // every rotation.
    class PhantomLookControl : public LookControl {
    public:
        explicit PhantomLookControl(Phantom* phantom);
        void Tick() override {}
    };

    // MC Phantom.PhantomBodyRotationControl — head follows the body, body
    // follows the yaw, no lag at all.
    class PhantomBodyRotationControl : public BodyRotationControl {
    public:
        explicit PhantomBodyRotationControl(Phantom* phantom);
        void ClientTick() override;

    private:
        Phantom* m_phantom;
    };

    // MC Phantom.PhantomMoveTargetGoal — the shared MOVE-flag base with the
    // "close enough to the move target" test.
    class PhantomMoveTargetGoal : public Goal {
    public:
        explicit PhantomMoveTargetGoal(Phantom* phantom);

    protected:
        // MC touchingTarget: within 2 blocks of moveTargetPoint.
        bool TouchingTarget() const;

        Phantom* m_phantom;
    };

    // MC Phantom.PhantomCircleAroundAnchorGoal — the idle orbit around the
    // anchor point, with the random height/radius/direction drift.
    class PhantomCircleAroundAnchorGoal : public PhantomMoveTargetGoal {
    public:
        explicit PhantomCircleAroundAnchorGoal(Phantom* phantom)
            : PhantomMoveTargetGoal(phantom) {}

        bool CanUse() override;
        void Start() override;
        void Tick() override;
        const char* Name() const override { return "PhantomCircleAroundAnchorGoal"; }

    private:
        void SelectNext();

        float m_angle = 0.0f;
        float m_distance = 0.0f;
        float m_height = 0.0f;
        float m_clockwise = 0.0f;
    };

    // MC Phantom.PhantomSweepAttackGoal — the dive: chase the target's
    // midpoint, bite on box contact, break off on a wall hit, a hurt tick,
    // or a cat within 16 blocks (checked every 20 ticks).
    class PhantomSweepAttackGoal : public PhantomMoveTargetGoal {
    public:
        explicit PhantomSweepAttackGoal(Phantom* phantom)
            : PhantomMoveTargetGoal(phantom) {}

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override {}
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "PhantomSweepAttackGoal"; }

    private:
        bool m_isScaredOfCat = false;
        int  m_catSearchTick = 0;
    };

    // MC Phantom.PhantomAttackStrategyGoal — the referee: with a valid
    // target it alternates CIRCLE and SWOOP on MC's timers and keeps the
    // anchor parked above the target.
    class PhantomAttackStrategyGoal : public Goal {
    public:
        explicit PhantomAttackStrategyGoal(Phantom* phantom)
            : m_phantom(phantom) {}

        bool CanUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "PhantomAttackStrategyGoal"; }

    private:
        void SetAnchorAboveTarget();

        Phantom* m_phantom;
        int m_nextSweepTick = 0;
    };

    // MC Phantom.PhantomAttackPlayerTargetGoal — scan for players every 60
    // ticks in a follow box inflated (16, 64, 16), HIGHEST player first.
    class PhantomAttackPlayerTargetGoal : public Goal {
    public:
        explicit PhantomAttackPlayerTargetGoal(Phantom* phantom);

        bool CanUse() override;
        bool CanContinueToUse() override;
        const char* Name() const override { return "PhantomAttackPlayerTargetGoal"; }

    private:
        Phantom* m_phantom;
        TargetingConditions m_attackTargeting;
        int m_nextScanTick;
    };

} // namespace Game
