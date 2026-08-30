// File: src/common/entity/ai/goals/HorseGoals.hpp
//
// MC ai/goal/RandomStandGoal — the equines' ambient rear-up. MC registers it
// from AbstractHorse.registerGoals for every equine whose canPerformRearing
// is true (all but the llama). It lives here because every goal class in
// this port does.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"

namespace Game {

    class AbstractHorse;

    // MC AbstractHorse.MountPanicGoal — PanicGoal that stands its ground
    // while a MOB is steering it (a jockey's ride does not bolt).
    class MountPanicGoal : public PanicGoal {
    public:
        MountPanicGoal(AbstractHorse* horse, double speedModifier);
        // Keeps the base name so PathfinderMob::IsPanicking still sees it
        // (MC tests `instanceof PanicGoal`; the PolarBear precedent).
        const char* Name() const override { return "PanicGoal"; }

    protected:
        bool ShouldPanic() const override;

    private:
        AbstractHorse* m_horse;
    };

    // MC ai/goal/RunAroundLikeCrazyGoal — an untamed horse bucks under a
    // PLAYER rider. Player riding does not exist in this port, so a horse's
    // only riders are mobs (jockeys) and MC's own !isMobControlled gate
    // keeps the goal idle — but every term is live, so it works the day a
    // player can climb on. The tick-side taming/dismount rolls need the
    // taming layer and are named skipped in the .cpp.
    class RunAroundLikeCrazyGoal : public Goal {
    public:
        RunAroundLikeCrazyGoal(AbstractHorse* horse, double speedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        const char* Name() const override { return "RunAroundLikeCrazyGoal"; }

    private:
        AbstractHorse* m_horse;
        double m_speedModifier;
        double m_posX = 0.0, m_posY = 0.0, m_posZ = 0.0;
    };

    // MC RandomStandGoal, verbatim: nextStand starts at minus the ambient
    // stand interval, canUse counts it up and past zero rolls
    // nextInt(1000) < nextStand, then (interval reset) a 1-in-10 for the
    // actual rear. start() rears via standIfPossible. (The ambient stand
    // sound waits on the sound system.)
    class RandomStandGoal : public Goal {
    public:
        explicit RandomStandGoal(AbstractHorse* horse);
        bool CanUse() override;
        bool CanContinueToUse() override { return false; }
        void Start() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "RandomStandGoal"; }

    private:
        void ResetStandInterval();

        AbstractHorse* m_horse;
        int m_nextStand = 0;
    };

} // namespace Game
