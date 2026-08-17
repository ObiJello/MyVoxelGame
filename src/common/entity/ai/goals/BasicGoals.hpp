// File: src/common/entity/ai/goals/BasicGoals.hpp
//
// The goals every one of the eight mobs uses: floating, wandering, looking
// around, looking at players, and panicking.
//
// Each is a direct port. Where a constant looks arbitrary it is MC's, and the
// comment says what it controls — the 0.02 probabilities in particular are what
// make idle mobs feel unhurried rather than twitchy, and raising them is the
// classic way to make a world full of mobs look wrong.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/TargetingConditions.hpp"

#include <glm/glm.hpp>

namespace Game {

    class Mob;
    class PathfinderMob;
    class LivingEntity;

    // MC FloatGoal — swim. Claims JUMP, so it coexists with a movement goal:
    // a panicking cow that falls in water keeps fleeing AND keeps its head up.
    class FloatGoal : public Goal {
    public:
        explicit FloatGoal(Mob* mob);
        bool CanUse() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        void Tick() override;
        const char* Name() const override { return "FloatGoal"; }

    private:
        Mob* m_mob;
    };

    // MC RandomStrollGoal — the idle wander.
    class RandomStrollGoal : public Goal {
    public:
        static constexpr int kDefaultInterval = 120;

        RandomStrollGoal(PathfinderMob* mob, double speedModifier,
                         int interval = kDefaultInterval, bool checkNoActionTime = true);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "RandomStrollGoal"; }

        void Trigger() { m_forceTrigger = true; }

    protected:
        // Overridden by WaterAvoidingRandomStrollGoal.
        virtual bool GetPosition(glm::dvec3& out);

        PathfinderMob* m_mob;
        double m_wantedX = 0.0, m_wantedY = 0.0, m_wantedZ = 0.0;
        double m_speedModifier;
        int    m_interval;
        bool   m_forceTrigger = false;
        bool   m_checkNoActionTime;
    };

    // MC WaterAvoidingRandomStrollGoal — the same wander, but a mob standing in
    // water aims for land, and 999 times in 1000 it prefers a land target
    // anyway. That tiny 0.001 is what stops land mobs from NEVER entering water.
    class WaterAvoidingRandomStrollGoal : public RandomStrollGoal {
    public:
        static constexpr float kProbability = 0.001f;

        WaterAvoidingRandomStrollGoal(PathfinderMob* mob, double speedModifier,
                                      float probability = kProbability);

        const char* Name() const override { return "WaterAvoidingRandomStrollGoal"; }

    protected:
        bool GetPosition(glm::dvec3& out) override;

    private:
        float m_probability;
    };

    // MC LookAtPlayerGoal. Claims LOOK only, so a mob can walk and watch you
    // at the same time.
    class LookAtPlayerGoal : public Goal {
    public:
        static constexpr float kDefaultProbability = 0.02f;

        LookAtPlayerGoal(Mob* mob, float lookDistance,
                         float probability = kDefaultProbability,
                         bool onlyHorizontal = false);

        // MC SetEntityLookTargetSometimes — what the BRAIN mobs use instead of
        // this goal. It fires on a uniform tick TIMER rather than a per-poll
        // probability, and the two are not interchangeable: a 2% roll is
        // geometric with a ~100-tick mean and a long tail, so a camel MC has
        // glance at you every 45 ticks on average would instead go several
        // seconds at a stretch without doing it. The eight mobs MC drives this
        // way are marked in GeneratedMobDefs.
        //
        // `holdMin`/`holdMax` are MC's LookAtTargetSink duration, which is
        // longer than this goal's own 40-80 — brain mobs hold a look about
        // twice as long as goal mobs do.
        void UseInterval(int intervalMin, int intervalMax,
                         int holdMin = 45, int holdMax = 90);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "LookAtPlayerGoal"; }
        // Defined in the .cpp: comparing a LivingEntity*/Animal*
        // against an Entity* needs the derived-to-base conversion,
        // and these headers only forward-declare those types.
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Mob*          m_mob;
        LivingEntity* m_lookAt = nullptr;
        float         m_lookDistance;
        float         m_probability;
        bool          m_onlyHorizontal;
        int           m_lookTime = 0;
        TargetingConditions m_conditions;

        // Interval mode. `m_intervalMin` 0 leaves the probability roll in
        // charge, which is MC's behaviour for every mob with a registerGoals.
        int     m_intervalMin = 0, m_intervalMax = 0;
        int     m_holdMin = 45, m_holdMax = 90;
        int64_t m_nextLookTick = -1;
    };

    // MC RestrictSunGoal — while it is bright out, tell the navigation to
    // route around sunlit blocks. It sets a flag rather than moving the mob,
    // which is why it declares no goal flags and never conflicts.
    class RestrictSunGoal : public Goal {
    public:
        explicit RestrictSunGoal(PathfinderMob* mob) : m_mob(mob) {}

        bool CanUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "RestrictSunGoal"; }

    private:
        void SetAvoidSun(bool v);

        PathfinderMob* m_mob;
    };

    // MC FleeSunGoal — a BURNING mob that can see the sky runs for shade.
    // Together with RestrictSunGoal this is why a caught-out skeleton dives
    // under an overhang instead of standing there burning.
    class FleeSunGoal : public Goal {
    public:
        FleeSunGoal(PathfinderMob* mob, double speedModifier)
            : m_mob(mob), m_speedModifier(speedModifier) {
            SetFlags(static_cast<uint8_t>(GoalFlag::Move));
        }

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        const char* Name() const override { return "FleeSunGoal"; }

    private:
        bool FindHidePos();

        PathfinderMob* m_mob;
        double m_speedModifier;
        double m_wantedX = 0.0, m_wantedY = 0.0, m_wantedZ = 0.0;
    };

    // MC RandomLookAroundGoal. Claims MOVE **and** LOOK — deliberately, so that
    // an idle mob glancing around is not simultaneously trying to walk
    // somewhere. This is why it can interrupt a stroll.
    class RandomLookAroundGoal : public Goal {
    public:
        explicit RandomLookAroundGoal(Mob* mob);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "RandomLookAroundGoal"; }

    private:
        Mob*   m_mob;
        double m_relX = 0.0, m_relZ = 0.0;
        int    m_lookTime = 0;
    };

    // MC PanicGoal — run somewhere else after being hurt, and toward water if
    // on fire.
    class PanicGoal : public Goal {
    public:
        PanicGoal(PathfinderMob* mob, double speedModifier);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "PanicGoal"; }

    protected:
        virtual bool ShouldPanic() const;
        bool FindRandomPosition();
        bool LookForWater();

        PathfinderMob* m_mob;
        double m_speedModifier;
        double m_posX = 0.0, m_posY = 0.0, m_posZ = 0.0;
        bool   m_isRunning = false;
    };

} // namespace Game
