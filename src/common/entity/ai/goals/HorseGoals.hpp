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
    class HorseTaming;
    class PathfinderMob;

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

    // MC ai/goal/RunAroundLikeCrazyGoal — an untamed equine carrying a
    // player (not steered by a mob) bolts to random spots 5 out, and on a
    // 1-in-adjustedTickDelay(50) tick either takes to the rider
    // (nextInt(maxTemper) < temper → tameWithName) or throws them (temper
    // + 5, ejectPassengers, makeMad, entity event 6). Registered by the
    // horse family and the llama (MC: every AbstractHorse).
    class RunAroundLikeCrazyGoal : public Goal {
    public:
        RunAroundLikeCrazyGoal(AbstractHorse* horse, double speedModifier);
        RunAroundLikeCrazyGoal(PathfinderMob* mob, HorseTaming* taming, double speedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Tick() override;
        const char* Name() const override { return "RunAroundLikeCrazyGoal"; }

    private:
        PathfinderMob* m_horse;
        HorseTaming*   m_taming;
        double m_speedModifier;
        double m_posX = 0.0, m_posY = 0.0, m_posZ = 0.0;
    };

    // MC RandomStandGoal, verbatim: nextStand starts at minus the ambient
    // stand interval, canUse counts it up and past zero rolls
    // nextInt(1000) < nextStand, then (interval reset) a 1-in-10 for the
    // actual rear. start() rears via standIfPossible and voices the ambient
    // stand sound.
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
