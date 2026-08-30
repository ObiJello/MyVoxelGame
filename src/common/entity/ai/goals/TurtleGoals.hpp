// File: src/common/entity/ai/goals/TurtleGoals.hpp
//
// MC animal/turtle/Turtle.java's nested goals. MC declares them as static
// inner classes of Turtle; they live here because every goal class in this
// port does. TurtleMoveControl is file-local in TurtleGoals.cpp, reached
// through MakeTurtleMoveControl.
#pragma once

#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/MoveToBlockGoal.hpp"

#include <memory>

namespace Game {

    class Turtle;
    class MoveControl;

    // MC's TurtleMoveControl — buoyancy in water, half-speed shamble on
    // land, thirds for hatchlings, and its own steering that swims along the
    // vertical component of the waypoint. Class file-local to TurtleGoals.cpp.
    std::unique_ptr<MoveControl> MakeTurtleMoveControl(Turtle* turtle);

    // MC Turtle.TurtlePanicGoal — panic prefers WATER: scan 7 blocks around
    // (the base goal's fire search is 5) and only fall back to a random spot
    // when no water is in reach.
    class TurtlePanicGoal : public PanicGoal {
    public:
        TurtlePanicGoal(Turtle* turtle, double speedModifier);
        bool CanUse() override;
        // Keeps the base name so PathfinderMob::IsPanicking still sees it
        // (MC tests `instanceof PanicGoal`; the PolarBear precedent).
        const char* Name() const override { return "PanicGoal"; }
    };

    // MC Turtle.TurtleBreedGoal — no courting while an egg is carried; the
    // breed itself is Turtle::SpawnChildFromBreeding (egg, not baby).
    class TurtleBreedGoal : public BreedGoal {
    public:
        TurtleBreedGoal(Turtle* turtle, double speedModifier);
        bool CanUse() override;
        const char* Name() const override { return "TurtleBreedGoal"; }

    private:
        Turtle* m_turtle;
    };

    // MC Turtle.TurtleLayEggGoal — walk to sand near home, dig for ~200
    // goal-ticks (LAYING_EGG drives the dig pose), then place the turtle_egg
    // block one above the sand. (MC rolls 1-4 eggs into the block's EGGS
    // state; block-state properties do not reach the mob seam, so one egg
    // block is placed. The lay sound and dig particles wait on their
    // systems.)
    class TurtleLayEggGoal : public MoveToBlockGoal {
    public:
        TurtleLayEggGoal(Turtle* turtle, double speedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Tick() override;
        const char* Name() const override { return "TurtleLayEggGoal"; }

    protected:
        bool IsValidTarget(const IBlockAccess& blocks, const glm::ivec3& pos) const override;

    private:
        Turtle* m_turtle;
    };

    // MC Turtle.TurtleGoToWaterGoal — a beached turtle (hatchlings at double
    // speed) walks to the nearest water block within 24.
    class TurtleGoToWaterGoal : public MoveToBlockGoal {
    public:
        TurtleGoToWaterGoal(Turtle* turtle, double speedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        bool ShouldRecalculatePath() const override { return m_tryTicks % 160 == 0; }
        const char* Name() const override { return "TurtleGoToWaterGoal"; }

    protected:
        bool IsValidTarget(const IBlockAccess& blocks, const glm::ivec3& pos) const override;

    private:
        Turtle* m_turtle;
    };

    // MC Turtle.TurtleGoHomeGoal — an egg-carrier (or, 1-in-700, any adult
    // far from home) swims back toward the home beach in getPosTowards hops.
    class TurtleGoHomeGoal : public Goal {
    public:
        TurtleGoHomeGoal(Turtle* turtle, double speedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "TurtleGoHomeGoal"; }

    private:
        Turtle* m_turtle;
        double  m_speedModifier;
        bool    m_stuck = false;
        int     m_closeToHomeTryTicks = 0;
    };

    // MC Turtle.TurtleTravelGoal — the open-water wander: pick a point up to
    // 512 blocks out and swim toward it in getPosTowards hops.
    class TurtleTravelGoal : public Goal {
    public:
        TurtleTravelGoal(Turtle* turtle, double speedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "TurtleTravelGoal"; }

    private:
        Turtle* m_turtle;
        double  m_speedModifier;
        bool    m_stuck = false;
    };

    // MC Turtle.TurtleRandomStrollGoal — the land stroll, gated off in water
    // and while homing or carrying an egg.
    class TurtleRandomStrollGoal : public RandomStrollGoal {
    public:
        TurtleRandomStrollGoal(Turtle* turtle, double speedModifier, int interval);
        bool CanUse() override;
        const char* Name() const override { return "TurtleRandomStrollGoal"; }

    private:
        Turtle* m_turtle;
    };

} // namespace Game
