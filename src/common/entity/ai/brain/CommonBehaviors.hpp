// File: src/common/entity/ai/brain/CommonBehaviors.hpp
//
// MC's shared brain behaviours — the ones more than one mob uses.
//
// These are the behaviours that make a brain mob move, look, panic, breed and
// pick fights. MC's own versions are mostly written with BehaviorBuilder, a
// declarative form where the entry conditions and the body are one expression;
// there is no C++ equivalent that is clearer than a small class, so each is a
// class with the same entry conditions and the same body. The SEMANTICS are
// what has to match, and those are: which memories must be present or absent,
// what the behaviour writes, and when it stops.
//
// A behaviour NEVER touches the mob's navigation or look control directly.
// It writes WALK_TARGET or LOOK_TARGET, and the CORE sinks do the rest. The two
// exceptions in MC are AnimalPanic (which stops navigation on start) and
// ShootTongue, and both are exceptions in this port too.
#pragma once

#include "common/entity/ai/brain/Behavior.hpp"
#include "common/entity/ai/brain/Sensor.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"

#include <functional>

namespace Game {

    class Mob;
    class PathfinderMob;
    class Animal;

    // MC CountDownCooldownTicks — decrement an int memory each tick and erase
    // it at zero. This is how every "cooldown" in the brain works: a behaviour
    // sets the memory, and its own entry condition is that memory being ABSENT.
    class CountDownCooldownTicks : public Behavior {
    public:
        explicit CountDownCooldownTicks(MemoryModule cooldown);
        const char* DebugString() const override { return "CountDownCooldownTicks"; }

    protected:
        // MC overrides timedOut to FALSE — a cooldown must not be cut short by
        // the behaviour duration, or it would expire early and the gated
        // behaviour would fire too often.
        bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override;
        void Tick(EntityLevel&, LivingEntity&, int64_t) override;
        void Stop(EntityLevel&, LivingEntity&, int64_t) override;

    private:
        MemoryModule m_cooldown;
    };

    // MC AnimalPanic. Runs while IS_PANICKING is set or the mob was hurt by
    // something in the PANIC_CAUSES tag, and re-picks a flee position whenever
    // the navigation runs out.
    class AnimalPanic : public Behavior {
    public:
        explicit AnimalPanic(float speedMultiplier);
        const char* DebugString() const override { return "AnimalPanic"; }

    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;
        bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override { return true; }
        void Start(EntityLevel&, LivingEntity&, int64_t) override;
        void Tick(EntityLevel&, LivingEntity&, int64_t) override;
        void Stop(EntityLevel&, LivingEntity&, int64_t) override;

    private:
        float m_speedMultiplier;
    };

    // MC RandomStroll.stroll / .swim — pick a wander position and write it as
    // WALK_TARGET. A one-shot: it succeeds, sets the memory, and stops.
    class RandomStroll : public Behavior {
    public:
        enum class Kind : uint8_t { Land, Swim };

        // `mayStrollFromWater` false is MC's `stroll(speed, false)`, which stops
        // a swimming mob picking a land target it cannot path to.
        static BehaviorPtr Stroll(float speedModifier, bool mayStrollFromWater = true);
        static BehaviorPtr Swim(float speedModifier);

        RandomStroll(float speedModifier, Kind kind, bool mayStrollFromWater);
        const char* DebugString() const override { return "RandomStroll"; }

    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;

    private:
        float m_speedModifier;
        Kind  m_kind;
        bool  m_mayStrollFromWater;
    };

    // MC SetWalkTargetFromLookTarget — walk to whatever you are looking at.
    class SetWalkTargetFromLookTarget : public Behavior {
    public:
        SetWalkTargetFromLookTarget(float speedModifier, int closeEnoughDistance);
        const char* DebugString() const override { return "SetWalkTargetFromLookTarget"; }

    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;

    private:
        float m_speedModifier;
        int   m_closeEnoughDistance;
    };

    // MC SetEntityLookTargetSometimes — glance at the nearest player on a
    // uniform timer. This is the behaviour that makes brain mobs look at you
    // noticeably more than goal mobs do.
    class SetEntityLookTargetSometimes : public Behavior {
    public:
        SetEntityLookTargetSometimes(float maxDist, int intervalMin, int intervalMax);
        const char* DebugString() const override { return "SetEntityLookTargetSometimes"; }

    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;

    private:
        // MC's Ticker: counts down, fires at zero, re-samples.
        int   m_ticksUntilNextStart = 0;
        float m_maxDistSqr;
        int   m_intervalMin, m_intervalMax;
    };

    // MC StartAttacking — turn a candidate into ATTACK_TARGET.
    class StartAttacking : public Behavior {
    public:
        using CanAttack   = std::function<bool(Mob&)>;
        using TargetFinder = std::function<LivingEntity*(Mob&)>;

        StartAttacking(CanAttack canAttack, TargetFinder finder);
        const char* DebugString() const override { return "StartAttacking"; }

    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;

    private:
        CanAttack    m_canAttack;
        TargetFinder m_finder;
    };

    // MC StopAttackingIfTargetInvalid — drop a target that died, became
    // unattackable, or that the mob has been failing to reach for 200 ticks.
    class StopAttackingIfTargetInvalid : public Behavior {
    public:
        StopAttackingIfTargetInvalid();
        const char* DebugString() const override { return "StopAttackingIfTargetInvalid"; }

    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;
    };

    // MC FollowTemptation — walk toward a player holding this mob's food.
    class FollowTemptation : public Behavior {
    public:
        static constexpr int kTemptationCooldown = 100;   // MC TEMPTATION_COOLDOWN

        explicit FollowTemptation(float speedModifier,
                                  double closeEnoughDistance = 2.5);
        const char* DebugString() const override { return "FollowTemptation"; }

    protected:
        bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override;
        void Start(EntityLevel&, LivingEntity&, int64_t) override;
        void Tick(EntityLevel&, LivingEntity&, int64_t) override;
        void Stop(EntityLevel&, LivingEntity&, int64_t) override;

    private:
        float  m_speedModifier;
        double m_closeEnoughDistance;
    };

    // MC AnimalMakeLove — pair up with a nearby partner of the same type and
    // produce a baby after 60..110 ticks of standing together.
    class AnimalMakeLove : public Behavior {
    public:
        explicit AnimalMakeLove(EntityTypeId partnerType, float speedModifier = 1.0f,
                                int closeEnoughDistance = 2);
        const char* DebugString() const override { return "AnimalMakeLove"; }
        void ClearReferenceTo(const Entity* entity) override;

    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;
        bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override;
        void Start(EntityLevel&, LivingEntity&, int64_t) override;
        void Tick(EntityLevel&, LivingEntity&, int64_t) override;
        void Stop(EntityLevel&, LivingEntity&, int64_t) override;

    private:
        Animal* FindValidBreedPartner(Animal& body) const;

        EntityTypeId m_partnerType;
        float        m_speedModifier;
        int          m_closeEnoughDistance;
        int64_t      m_spawnChildAtTime = 0;
    };

    // MC TryFindLand — a swimming mob looks for a dry block to climb out onto.
    class TryFindLand : public Behavior {
    public:
        TryFindLand(int range, float speedModifier);
        const char* DebugString() const override { return "TryFindLand"; }

    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;

    private:
        int     m_range;
        float   m_speedModifier;
        // MC's 60-tick throttle: the search is a Manhattan ball scan and doing
        // it every tick for every swimming mob is the expensive shape.
        int64_t m_nextOkStartTime = 0;
    };

    // MC Swim — the brain's FloatGoal. Jumps with `chance` probability every
    // tick spent in water, which is what keeps a non-swimmer at the surface.
    class Swim : public Behavior {
    public:
        explicit Swim(float chance) : Behavior({}), m_chance(chance) {}
        const char* DebugString() const override { return "Swim"; }
    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override;
        bool CanStillUse(EntityLevel& l, LivingEntity& b, int64_t) override {
            return CheckExtraStartConditions(l, b);
        }
        void Tick(EntityLevel&, LivingEntity&, int64_t) override;
    private:
        float m_chance;
    };

    // MC DoNothing — occupies a gate slot so the mob visibly pauses. Without it
    // an idle mob picks a real option every time the gate opens and never
    // stands still.
    class DoNothing : public Behavior {
    public:
        DoNothing(int minDuration, int maxDuration)
            : Behavior({}, minDuration, maxDuration) {}
        const char* DebugString() const override { return "DoNothing"; }
    protected:
        bool CanStillUse(EntityLevel&, LivingEntity&, int64_t) override { return true; }
    };

    // MC RandomLookAround — look somewhere random and set a gaze cooldown, so
    // the mob holds the new direction instead of twitching every tick.
    class RandomLookAround : public Behavior {
    public:
        RandomLookAround(int intervalMin, int intervalMax,
                         float maxYaw, float minPitch, float maxPitch);
        const char* DebugString() const override { return "RandomLookAround"; }
    protected:
        void Start(EntityLevel&, LivingEntity&, int64_t) override;
    private:
        int   m_intervalMin, m_intervalMax;
        float m_maxYaw, m_minPitch, m_pitchRange;
    };

    // MC BehaviorBuilder.triggerIf(predicate) — a one-tick behaviour that
    // succeeds when the predicate holds and does nothing else. It exists to
    // take WEIGHT away from the other options in a gate, which is how MC tunes
    // how often a mob picks a real action versus standing there.
    class TriggerIf : public Behavior {
    public:
        using Pred = bool (*)(LivingEntity&);
        explicit TriggerIf(Pred p) : Behavior({}, 1), m_pred(p) {}
        const char* DebugString() const override { return "TriggerIf"; }
    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity& body) override {
            return m_pred(body);
        }
    private:
        Pred m_pred;
    };

    // MC MeleeAttack — swing when the target is in reach and off cooldown.
    // Note it also reads NEAREST_VISIBLE_LIVING_ENTITIES: MC will not let a mob
    // hit something it cannot currently SEE even if ATTACK_TARGET still names
    // it, which is what stops mobs punching through walls.
    class MeleeAttack : public Behavior {
    public:
        explicit MeleeAttack(int cooldownBetweenAttacks);
        const char* DebugString() const override { return "MeleeAttack"; }
    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;
    private:
        int m_cooldown;
    };

    // MC SetWalkTargetFromAttackTargetIfTargetOutOfReach.
    class SetWalkTargetFromAttackTarget : public Behavior {
    public:
        explicit SetWalkTargetFromAttackTarget(float speedModifier);
        const char* DebugString() const override { return "SetWalkTargetFromAttackTarget"; }
    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;
    private:
        float m_speedModifier;
    };

    // MC EraseMemoryIf — the brain's way of cancelling something: one behaviour
    // clears the memory another is gated on.
    class EraseMemoryIf : public Behavior {
    public:
        using Pred = std::function<bool(LivingEntity&)>;
        EraseMemoryIf(Pred pred, MemoryModule memory);
        const char* DebugString() const override { return "EraseMemoryIf"; }
    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;
    private:
        Pred         m_pred;
        MemoryModule m_memory;
    };

    // MC SetEntityLookTarget — look at the nearest entity matching a predicate.
    class SetEntityLookTarget : public Behavior {
    public:
        using Pred = std::function<bool(LivingEntity&)>;
        SetEntityLookTarget(Pred pred, float maxDist);
        static BehaviorPtr OfType(EntityTypeId type, float maxDist);
        static BehaviorPtr Any(float maxDist);
        const char* DebugString() const override { return "SetEntityLookTarget"; }
    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;
    private:
        Pred  m_pred;
        float m_maxDistSqr;
    };

    // MC BabyFollowAdult — a baby trails the nearest adult of its own type, but
    // only between the range's min and max, so it neither crowds the adult nor
    // sprints after one from across the world.
    class BabyFollowAdult : public Behavior {
    public:
        BabyFollowAdult(int followRangeMin, int followRangeMax, float speedModifier);
        const char* DebugString() const override { return "BabyFollowAdult"; }
    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;
    private:
        int   m_min, m_max;
        float m_speedModifier;
    };

    // MC SetWalkTargetAwayFrom.entity — flee whatever a memory names.
    class SetWalkTargetAwayFrom : public Behavior {
    public:
        SetWalkTargetAwayFrom(MemoryModule avoidMemory, float speedModifier,
                              int desiredDistance, bool interruptCurrentWalk);
        const char* DebugString() const override { return "SetWalkTargetAwayFrom"; }
    protected:
        bool CheckExtraStartConditions(EntityLevel&, LivingEntity&) override;
    private:
        MemoryModule m_avoid;
        float m_speedModifier;
        int   m_desiredDistance;
        bool  m_interruptCurrentWalk;
    };

    // MC LongJumpMidJump — shared by frog and goat.
    class LongJumpMidJump : public Behavior {
    public:
        LongJumpMidJump(int cooldownMin, int cooldownMax);
        const char* DebugString() const override { return "LongJumpMidJump"; }
    protected:
        bool CanStillUse(EntityLevel&, LivingEntity& body, int64_t) override;
        void Start(EntityLevel&, LivingEntity&, int64_t) override;
        void Stop(EntityLevel&, LivingEntity&, int64_t) override;
    private:
        int m_cooldownMin, m_cooldownMax;
    };

    // ── Sensors ────────────────────────────────────────────────────────────

    // MC AdultSensor — the nearest visible adult of the mob's OWN type.
    class AdultSensor : public Sensor {
    public:
        std::vector<MemoryModule> Requires() const override {
            return { MemoryModule::NearestVisibleAdult };
        }
    protected:
        void DoTick(EntityLevel&, LivingEntity&) override;
    };


    // MC IsInWaterSensor.
    class IsInWaterSensor : public Sensor {
    public:
        // MC does not override the scan rate, so this is the default 20 —
        // which means up to a second of lag switching between IDLE and SWIM.
        // That is MC's own latency, not a shortcut.
        IsInWaterSensor() = default;
        std::vector<MemoryModule> Requires() const override {
            return { MemoryModule::IsInWater };
        }
    protected:
        void DoTick(EntityLevel&, LivingEntity&) override;
    };

    // MC HurtBySensor.
    class HurtBySensor : public Sensor {
    public:
        HurtBySensor() : Sensor(1) {}
        std::vector<MemoryModule> Requires() const override {
            return { MemoryModule::HurtBy, MemoryModule::HurtByEntity };
        }
    protected:
        void DoTick(EntityLevel&, LivingEntity&) override;
    };

    // MC TemptingSensor — the nearest player holding food this mob wants.
    class TemptingSensor : public Sensor {
    public:
        using IsTemptation = std::function<bool(uint32_t)>;

        explicit TemptingSensor(IsTemptation pred) : m_pred(std::move(pred)) {}
        std::vector<MemoryModule> Requires() const override {
            return { MemoryModule::TemptingPlayer };
        }
    protected:
        void DoTick(EntityLevel&, LivingEntity&) override;

    private:
        IsTemptation m_pred;
        // MC's TEMPTATION_RANGE.
        static constexpr double kTemptationRange = 10.0;
    };

    // MC's per-mob "attackables" sensors, parameterised. The frog's version
    // accepts slimes and magma cubes; the shape is shared.
    class NearestAttackableSensor : public Sensor {
    public:
        using IsAttackable = std::function<bool(LivingEntity&)>;

        explicit NearestAttackableSensor(IsAttackable pred) : m_pred(std::move(pred)) {}
        std::vector<MemoryModule> Requires() const override {
            return { MemoryModule::NearestAttackable };
        }
    protected:
        void DoTick(EntityLevel&, LivingEntity&) override;

    private:
        IsAttackable m_pred;
    };

} // namespace Game
