// File: src/common/entity/ai/goals/AttackGoals.hpp
//
// The goals that act on a target once a target goal has chosen one.
//
// MeleeAttackGoal is the interesting one. Its path-recalculation schedule is
// what makes MC pursuit look deliberate rather than either robotic or laggy:
// it re-paths every 4-11 ticks, adds 5 ticks past 16 blocks and 10 past 32, and
// adds a further 15 whenever the path request FAILED. That last rule is the one
// people leave out, and without it a mob that cannot reach you burns a full A*
// search every few ticks forever.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/world/pathfinder/Path.hpp"

#include <optional>

namespace Game {

    class Mob;
    struct EntityLevel;
    class PathfinderMob;
    class LivingEntity;
    class Creeper;
    class RangedAttackMob;
    class PolarBear;
    class Rabbit;

    class MeleeAttackGoal : public Goal {
    public:
        MeleeAttackGoal(PathfinderMob* mob, double speedModifier, bool followingTargetEvenIfNotSeen);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "MeleeAttackGoal"; }

    protected:
        virtual void CheckAndPerformAttack(LivingEntity& target);
        bool  IsTimeToAttack() const { return m_ticksUntilNextAttack <= 0; }
        void  ResetAttackCooldown() { m_ticksUntilNextAttack = AdjustedTickDelay(kAttackInterval); }
        bool  CanPerformAttack(LivingEntity& target);

        static constexpr int   kAttackInterval = 20;
        static constexpr int64_t kCooldownBetweenCanUseChecks = 20;

        PathfinderMob* m_mob;
        double m_speedModifier;
        bool   m_followingTargetEvenIfNotSeen;

        std::optional<Path> m_path;
        double m_pathedTargetX = 0.0, m_pathedTargetY = 0.0, m_pathedTargetZ = 0.0;
        int    m_ticksUntilNextPathRecalculation = 0;
        int    m_ticksUntilNextAttack = 0;
        int64_t m_lastCanUseCheck = 0;
    };

    // MC RangedBowAttackGoal — the skeleton's bow behaviour: close to within
    // the attack radius, hold ground and strafe while drawing, release after
    // 20 draw ticks, cool down for attackIntervalMin. The strafe flips
    // direction with probability 0.3 every 20 strafing ticks, backs off inside
    // 25% of the radius and closes beyond 75% — the dance every player knows.
    //
    // MC gates the goal on isHolding(BOW) and drives the draw through the
    // item-use system; this port has no mob equipment or item use, so the mob
    // is treated as permanently holding a bow and the draw is a goal-local
    // counter with identical timing.
    class RangedBowAttackGoal : public Goal {
    public:
        RangedBowAttackGoal(Mob* mob, RangedAttackMob* shooter, double speedModifier,
                            int attackIntervalMin, float attackRadius);

        void SetMinAttackInterval(int ticks) { m_attackIntervalMin = ticks; }

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "RangedBowAttackGoal"; }

    private:
        Mob*             m_mob;
        RangedAttackMob* m_shooter;
        double m_speedModifier;
        int    m_attackIntervalMin;
        float  m_attackRadiusSqr;
        int    m_attackTime = -1;
        int    m_seeTime = 0;
        bool   m_strafingClockwise = false;
        bool   m_strafingBackwards = false;
        int    m_strafingTime = -1;
        // The stand-in for MC's item-use draw: <0 = not drawing.
        int    m_useTicks = -1;
    };

    // MC Spider.SpiderAttackGoal — MeleeAttackGoal(1.0, true) that drops its
    // target with a 1-in-100 per-tick roll while in bright light (the spider's
    // daytime truce). MC nests it in Spider.java; it lives here because every
    // goal class in this port does.
    class SpiderAttackGoal : public MeleeAttackGoal {
    public:
        explicit SpiderAttackGoal(PathfinderMob* mob);

        bool CanContinueToUse() override;
        const char* Name() const override { return "SpiderAttackGoal"; }
    };

    // MC PolarBear.PolarBearMeleeAttackGoal — MeleeAttackGoal(1.25, true)
    // whose attack check is where the REAR-UP lives: inside warning range
    // (target width + 3) the bear stands for the last 10 ticks of every
    // attack cooldown and drops back down when it swings. MC nests it in
    // PolarBear.java; it lives here because every goal class in this port
    // does.
    class PolarBearMeleeAttackGoal : public MeleeAttackGoal {
    public:
        explicit PolarBearMeleeAttackGoal(PolarBear* bear);

        void Stop() override;
        const char* Name() const override { return "PolarBearMeleeAttackGoal"; }

    protected:
        void CheckAndPerformAttack(LivingEntity& target) override;

    private:
        PolarBear* m_bear;
    };

    // MC ZombieAttackGoal — a melee goal that also drives the "arms out"
    // aggressive pose the renderer reads.
    class ZombieAttackGoal : public MeleeAttackGoal {
    public:
        ZombieAttackGoal(PathfinderMob* mob, double speedModifier, bool followingTargetEvenIfNotSeen);

        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "ZombieAttackGoal"; }

    private:
        int m_raiseArmTicks = 0;
    };

    // MC SwellGoal — the creeper fuse. Claims MOVE so that swelling stops the
    // creeper walking; the fuse itself lives on the Creeper.
    class SwellGoal : public Goal {
    public:
        explicit SwellGoal(Creeper* creeper);

        bool CanUse() override;
        void Start() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "SwellGoal"; }
        // Defined in the .cpp: comparing a LivingEntity*/Animal*
        // against an Entity* needs the derived-to-base conversion,
        // and these headers only forward-declare those types.
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Creeper*      m_creeper;
        LivingEntity* m_target = nullptr;
    };

    // MC LeapAtTargetGoal — the spider pounce.
    class LeapAtTargetGoal : public Goal {
    public:
        LeapAtTargetGoal(Mob* mob, float yd);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        const char* Name() const override { return "LeapAtTargetGoal"; }
        // Defined in the .cpp: comparing a LivingEntity*/Animal*
        // against an Entity* needs the derived-to-base conversion,
        // and these headers only forward-declare those types.
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Mob*          m_mob;
        LivingEntity* m_target = nullptr;
        float         m_yd;
    };

    // MC AvoidEntityGoal, specialised to players. Used by nothing in the
    // default eight (creepers avoid cats, skeletons avoid wolves — neither
    // exists here yet), but kept because it is what a future cat/wolf needs and
    // because PanicGoal's flee-direction helper is shared with it.
    // MC AvoidEntityGoal. Parameterised on a `Class<T>` in MC; three mobs pass
    // Player.class and the rest name another mob (a creaking, a wolf, an
    // ocelot), so the type list comes from MC's own registerGoals.
    class AvoidEntityGoal : public Goal {
    public:
        // The player form — MC's `Player.class`.
        AvoidEntityGoal(PathfinderMob* mob, float maxDistance,
                        double walkSpeedModifier, double sprintSpeedModifier);

        // The entity-type form. `types` must outlive the goal; it points into
        // the generated def table.
        AvoidEntityGoal(PathfinderMob* mob, const EntityTypeId* types, int typeCount,
                        float maxDistance, double walkSpeedModifier,
                        double sprintSpeedModifier);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "AvoidEntityGoal"; }
        // Defined in the .cpp: comparing a LivingEntity*/Animal*
        // against an Entity* needs the derived-to-base conversion,
        // and these headers only forward-declare those types.
        void ClearReferenceTo(const Entity* entity) override;

    private:
        LivingEntity* FindThreat(EntityLevel& level) const;

        PathfinderMob* m_mob;
        LivingEntity*  m_toAvoid = nullptr;
        std::optional<Path> m_path;
        const EntityTypeId* m_types = nullptr;
        int    m_typeCount = 0;
        bool   m_avoidsPlayers = true;
        float  m_maxDistance;
        double m_walkSpeedModifier;
        double m_sprintSpeedModifier;
    };

    // MC Rabbit.RabbitAvoidEntityGoal — AvoidEntityGoal whose canUse is gated
    // on the rabbit not being the EVIL variant. The variant system is not
    // ported (every rabbit is a plain brown rabbit), so the gate always
    // passes; the class is kept so the rabbit's goal table reads like MC's
    // and the gate has a home when variants land. MC nests it in Rabbit.java;
    // it lives here because every goal class in this port does.
    class RabbitAvoidEntityGoal : public AvoidEntityGoal {
    public:
        // The player form and the type-list form, mirroring the base.
        RabbitAvoidEntityGoal(Rabbit* rabbit, float maxDistance,
                              double walkSpeedModifier, double sprintSpeedModifier);
        RabbitAvoidEntityGoal(Rabbit* rabbit, const EntityTypeId* types, int typeCount,
                              float maxDistance, double walkSpeedModifier,
                              double sprintSpeedModifier);

        const char* Name() const override { return "RabbitAvoidEntityGoal"; }
    };

    // The name this port used before the goal was generalised.
    using AvoidPlayerGoal = AvoidEntityGoal;

} // namespace Game
