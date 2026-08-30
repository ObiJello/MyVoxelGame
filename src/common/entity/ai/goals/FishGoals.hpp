// File: src/common/entity/ai/goals/FishGoals.hpp
//
// MC AbstractFish.FishSwimGoal + FollowFlockLeaderGoal.
#pragma once

#include "common/entity/ai/goals/BasicGoals.hpp"

namespace Game {

    class Fish;
    class SchoolingFish;

    // MC AbstractFish.FishSwimGoal — RandomSwimmingGoal(1.0, 40) gated on
    // canRandomSwim, which is what stops a follower from wandering away from
    // its school.
    class FishSwimGoal : public RandomSwimmingGoal {
    public:
        explicit FishSwimGoal(Fish* fish);

        bool CanUse() override;
        const char* Name() const override { return "FishSwimGoal"; }

    private:
        Fish* m_fish;
    };

    // MC FollowFlockLeaderGoal — every 200-220 ticks a leaderless fish either
    // joins a school with room or recruits its leaderless neighbours; while
    // following, repath to the leader every 10 ticks until it strays past 11
    // blocks.
    class FollowFlockLeaderGoal : public Goal {
    public:
        static constexpr int kIntervalTicks = 200;

        explicit FollowFlockLeaderGoal(SchoolingFish* fish);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "FollowFlockLeaderGoal"; }

    private:
        int NextStartTick() const;

        SchoolingFish* m_fish;
        int m_timeToRecalcPath = 0;
        int m_nextStartTick = 0;
    };

    class Pufferfish;

    // MC Pufferfish.PufferfishPuffGoal — start inflating while any scary
    // LivingEntity is within the fish's box inflated by 2 blocks; deflate
    // once the coast is clear (stop() only clears the inflate counter — the
    // deflate timer in Pufferfish::Tick does the rest).
    class PufferfishPuffGoal : public Goal {
    public:
        explicit PufferfishPuffGoal(Pufferfish* fish) : m_fish(fish) {}

        bool CanUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "PufferfishPuffGoal"; }

    private:
        Pufferfish* m_fish;
    };

    class Squid;

    // MC Squid.SquidRandomMovementGoal — a fresh random jet direction every
    // ~50 ticks (or immediately when beached/idle); an idle squid (600+ ticks
    // of noActionTime... MC uses 100) parks.
    class SquidRandomMovementGoal : public Goal {
    public:
        explicit SquidRandomMovementGoal(Squid* squid) : m_squid(squid) {}
        bool CanUse() override { return true; }
        void Tick() override;
        const char* Name() const override { return "SquidRandomMovementGoal"; }
    private:
        Squid* m_squid;
    };

    // MC Squid.SquidFleeGoal — jet hard away from whatever last hurt it,
    // while it stays within 10 blocks.
    class SquidFleeGoal : public Goal {
    public:
        explicit SquidFleeGoal(Squid* squid) : m_squid(squid) {}
        bool CanUse() override;
        void Start() override { m_fleeTicks = 0; }
        bool RequiresUpdateEveryTick() const override { return true; }
        void Tick() override;
        const char* Name() const override { return "SquidFleeGoal"; }
    private:
        Squid* m_squid;
        int m_fleeTicks = 0;
    };

} // namespace Game
