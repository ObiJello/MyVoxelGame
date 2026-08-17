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

    // The name this port used before the goal was generalised.
    using AvoidPlayerGoal = AvoidEntityGoal;

} // namespace Game
