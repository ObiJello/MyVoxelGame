// File: src/common/entity/ai/goals/RangedGoals.hpp
//
// The goals that make a mob SHOOT something other than an arrow, plus the
// movement goals MC nests inside those mobs' classes (the ghast's float and
// look, the shulker's peek). Each is a transcription of its MC original; the
// priority numbers live at the registration sites in the mob classes.
#pragma once

#include "common/entity/ai/Goal.hpp"

#include <glm/glm.hpp>

namespace Game {

    class Mob;
    class LivingEntity;
    class RangedAttackMob;
    class Blaze;
    class Ghast;
    class Shulker;
    class Drowned;

    // MC RangedAttackGoal ("ArrowAttackGoal") — the generic ranged cadence:
    // close to within the radius, hold and look, fire every
    // lerp(intervalMin..intervalMax) ticks scaled by distance, with `power`
    // = clamp(dist/radius, 0.1, 1). Snow golem (1.25, 20, 10), witch
    // (1.0, 60, 10), llama (1.25, 40, 20), drowned trident (subclass below).
    class RangedAttackGoal : public Goal {
    public:
        RangedAttackGoal(Mob* mob, RangedAttackMob* shooter, double speedModifier,
                         int attackIntervalMin, int attackIntervalMax,
                         float attackRadius);
        RangedAttackGoal(Mob* mob, RangedAttackMob* shooter, double speedModifier,
                         int attackInterval, float attackRadius)
            : RangedAttackGoal(mob, shooter, speedModifier, attackInterval,
                               attackInterval, attackRadius) {}

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override {}
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "RangedAttackGoal"; }
        void ClearReferenceTo(const Entity* entity) override;

    protected:
        Mob*             m_mob;
        RangedAttackMob* m_shooter;
        LivingEntity*    m_target = nullptr;
        double m_speedModifier;
        int    m_attackIntervalMin;
        int    m_attackIntervalMax;
        float  m_attackRadius;
        float  m_attackRadiusSqr;
        int    m_attackTime = -1;
        int    m_seeTime = 0;
    };

    // MC Drowned.DrownedTridentAttackGoal — RangedAttackGoal(1.0, 40, 10)
    // gated on the drowned actually holding a trident (the 6.25% spawn roll;
    // see Drowned::FinalizeSpawn), raising the aggressive pose while it aims.
    class DrownedTridentAttackGoal : public RangedAttackGoal {
    public:
        DrownedTridentAttackGoal(Drowned* drowned, double speedModifier,
                                 int attackInterval, float attackRadius);

        bool CanUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "DrownedTridentAttackGoal"; }

    private:
        Drowned* m_drowned;
    };

    // MC Blaze.BlazeAttackGoal — the 3-shot burst: 60-tick charged wind-up,
    // then shots every 6 ticks through attackStep 2..4, then a 100-tick cool
    // down; inside 2 blocks it melees on a 20-tick clock instead.
    class BlazeAttackGoal : public Goal {
    public:
        explicit BlazeAttackGoal(Blaze* blaze);

        bool CanUse() override;
        void Start() override { m_attackStep = 0; }
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "BlazeAttackGoal"; }

    private:
        double GetFollowDistance() const;

        Blaze* m_blaze;
        int m_attackStep = 0;
        int m_attackTime = 0;
        int m_lastSeen = 0;
    };

    // MC Ghast.RandomFloatAroundGoal — pick a random point within ±16 on each
    // axis and hand it to the move control; re-pick when idle or the current
    // wanted point is closer than 1 or farther than 60 blocks.
    class RandomFloatAroundGoal : public Goal {
    public:
        explicit RandomFloatAroundGoal(Mob* ghast);

        bool CanUse() override;
        bool CanContinueToUse() override { return false; }
        void Start() override;
        const char* Name() const override { return "RandomFloatAroundGoal"; }

        // MC getSuitableFlyToPosition, minus the home restriction (no home
        // system) with distanceToBlocks 0 — which makes the 64-attempt loop
        // accept its first pick, exactly as MC does for the plain ghast.
        static glm::dvec3 GetSuitableFlyToPosition(Mob& mob);

    private:
        Mob* m_ghast;
    };

    // MC Ghast.GhastLookGoal — face the travel direction, or the target when
    // one is within 64 blocks.
    class GhastLookGoal : public Goal {
    public:
        explicit GhastLookGoal(Mob* ghast) ;

        bool CanUse() override { return true; }
        bool RequiresUpdateEveryTick() const override { return true; }
        void Tick() override;
        const char* Name() const override { return "GhastLookGoal"; }

    private:
        Mob* m_ghast;
    };

    // MC Ghast.GhastShootFireballGoal — the 20-tick wind-up (charging synced
    // from tick 10) ending in a LargeFireball aimed from 4 blocks along the
    // view vector, then a 40-tick cooldown (chargeTime jumps to -40).
    class GhastShootFireballGoal : public Goal {
    public:
        explicit GhastShootFireballGoal(Ghast* ghast);

        bool CanUse() override;
        void Start() override { m_chargeTime = 0; }
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "GhastShootFireballGoal"; }

    private:
        Ghast* m_ghast;
        int m_chargeTime = 0;
    };

    // MC Shulker.ShulkerAttackGoal — peek fully open while a target lives,
    // fire a ShulkerBullet every 20 + rand(10)*10 ticks inside 20 blocks.
    class ShulkerAttackGoal : public Goal {
    public:
        explicit ShulkerAttackGoal(Shulker* shulker);

        bool CanUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "ShulkerAttackGoal"; }

    private:
        Shulker* m_shulker;
        int m_attackTime = 0;
    };

    // MC Shulker.ShulkerPeekGoal — the idle 30% peek, 20-60 ticks, on a
    // 1-in-40 roll per evaluation while nothing is targeted.
    class ShulkerPeekGoal : public Goal {
    public:
        explicit ShulkerPeekGoal(Shulker* shulker);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override { --m_peekTime; }
        const char* Name() const override { return "ShulkerPeekGoal"; }

    private:
        Shulker* m_shulker;
        int m_peekTime = 0;
    };

} // namespace Game
