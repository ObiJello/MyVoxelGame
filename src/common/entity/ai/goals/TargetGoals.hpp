// File: src/common/entity/ai/goals/TargetGoals.hpp
//
// MC net.minecraft.world.entity.ai.goal.target — the goals that decide WHO a
// mob is angry at. They all claim GoalFlag::Target, which is a separate flag
// from MOVE/LOOK/JUMP precisely so that acquiring a target never competes with
// walking toward one; that is why Mob keeps two selectors.
//
// The `mustSee` / unseenTicks mechanism is what gives MC mobs their memory: a
// target that breaks line of sight is kept for 60 more ticks (300 if the mob
// was hurt by it), so stepping behind a wall does not instantly shake a zombie.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"

namespace Game {

    class Mob;
    class LivingEntity;

    class TargetGoal : public Goal {
    public:
        TargetGoal(Mob* mob, bool mustSee, bool mustReach = false);

        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "TargetGoal"; }
        // Defined in the .cpp: comparing a LivingEntity*/Animal*
        // against an Entity* needs the derived-to-base conversion,
        // and these headers only forward-declare those types.
        void ClearReferenceTo(const Entity* entity) override;

    protected:
        double GetFollowDistance() const;

        Mob*          m_mob;
        LivingEntity* m_targetMob = nullptr;
        bool          m_mustSee;
        bool          m_mustReach;
        int           m_unseenTicks = 0;
        // MC's default memory. HurtByTargetGoal raises it to 300 on start.
        int           m_unseenMemoryTicks = 60;
    };

    // MC HurtByTargetGoal — retaliate. Deliberately ignores line of sight and
    // invisibility: something hit you, you know where it is.
    class HurtByTargetGoal : public TargetGoal {
    public:
        explicit HurtByTargetGoal(Mob* mob);

        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "HurtByTargetGoal"; }

        // MC setAlertOthers — wake nearby mobs of the same type. Zombies use
        // it, which is why hitting one pulls the whole group.
        HurtByTargetGoal& SetAlertOthers() { m_alertOthers = true; return *this; }

    private:
        void AlertOthers();

        int64_t m_timestamp = 0;
        bool    m_alertOthers = false;
    };

    // MC NearestAttackableTargetGoal.
    //
    // MC parameterises it on a `Class<T>` — the kind of entity to hunt. Twenty
    // mobs pass `Player.class`, and the rest pass IronGolem, AbstractVillager,
    // Turtle and a handful of one-offs. This carries that as an EntityTypeId
    // list plus a players flag, generated from MC's own registerGoals, because
    // a zombie that hunts players but ignores villagers and iron golems is not
    // the same mob.
    class NearestAttackableTargetGoal : public TargetGoal {
    public:
        static constexpr int kDefaultRandomInterval = 10;

        // The player form — MC's `Player.class`.
        NearestAttackableTargetGoal(Mob* mob, bool mustSee, bool mustReach = false,
                                    int randomInterval = kDefaultRandomInterval);

        // The entity-type form. `types` must outlive the goal; it points into
        // the generated def table.
        NearestAttackableTargetGoal(Mob* mob, const EntityTypeId* types, int typeCount,
                                    bool mustSee, bool mustReach = false,
                                    int randomInterval = kDefaultRandomInterval);

        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "NearestAttackableTargetGoal"; }
        // Defined in the .cpp: comparing a LivingEntity*/Animal*
        // against an Entity* needs the derived-to-base conversion,
        // and these headers only forward-declare those types.
        void ClearReferenceTo(const Entity* entity) override;

        // Spider and its light check: MC subclasses the goal to add a
        // condition, which this hook replaces.
        using ExtraCondition = bool (*)(Mob&);
        NearestAttackableTargetGoal& SetExtraCondition(ExtraCondition fn) {
            m_extraCondition = fn;
            return *this;
        }

    private:
        void FindTarget();

        LivingEntity*       m_target = nullptr;
        const EntityTypeId* m_types = nullptr;
        int                 m_typeCount = 0;
        bool                m_targetsPlayers = true;
        int                 m_randomInterval;
        TargetingConditions m_conditions;
        ExtraCondition      m_extraCondition = nullptr;
    };

    // The name this port used before the goal was generalised. MC has one goal
    // and so does this — the player case is the type list being empty.
    using NearestAttackablePlayerGoal = NearestAttackableTargetGoal;

} // namespace Game
