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
    class PolarBear;
    class NeutralMob;

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
        // Virtual because MC's is (Java): PolarBearAttackPlayersGoal halves
        // it, and FindTarget must see the override.
        virtual double GetFollowDistance() const;

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

        // MC setAlertOthers(exceptTheseTypes...) — wake nearby mobs, which is
        // why hitting one zombie pulls the whole group. MC gathers
        // mob.getClass() (so a Zombie wakes every Zombie SUBCLASS — husks,
        // drowned, zombie villagers) and then skips the exact classes listed.
        // C++ has no runtime getEntitiesOfClass, so the caller passes the
        // whole family test as a predicate; null keeps the same-exact-type
        // default, which is right for every mob whose class has no subclasses.
        using AlertPredicate = bool (*)(const Mob& self, const Mob& other);
        HurtByTargetGoal& SetAlertOthers(AlertPredicate familyFilter = nullptr) {
            m_alertOthers = true;
            m_alertFilter = familyFilter;
            return *this;
        }

    protected:
        // Protected rather than private: MC's PolarBearHurtByTargetGoal calls
        // alertOthers() directly when a cub is hit.
        void AlertOthers();

        // MC HurtByTargetGoal.alertOther — hand ONE nearby mob the attacker.
        // Virtual because the polar bear's override filters to adult bears.
        virtual void AlertOther(Mob& other, LivingEntity& attacker);

    private:
        int64_t        m_timestamp = 0;
        bool           m_alertOthers = false;
        AlertPredicate m_alertFilter = nullptr;
    };

    // MC PolarBear.PolarBearHurtByTargetGoal — retaliation with the family
    // rule: a hit CUB does not fight back, it alerts every ADULT bear nearby
    // and stands down. MC nests it in PolarBear.java; it lives here because
    // every goal class in this port does.
    class PolarBearHurtByTargetGoal : public HurtByTargetGoal {
    public:
        explicit PolarBearHurtByTargetGoal(PolarBear* bear);

        void Start() override;
        const char* Name() const override { return "PolarBearHurtByTargetGoal"; }

    protected:
        void AlertOther(Mob& other, LivingEntity& attacker) override;
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

        // MC's TargetingConditions.Selector — the per-CANDIDATE predicate the
        // sixth constructor argument carries (`this::isAngryAt` on every
        // NeutralMob, the enemy-but-not-creeper filter on the iron golem).
        // A capture-less lambda downcasting `mob` is the idiom, mirroring
        // MC's bound method reference.
        using CandidateSelector = bool (*)(Mob&, const LivingEntity&);
        NearestAttackableTargetGoal& SetSelector(CandidateSelector fn) {
            m_selector = fn;
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
        CandidateSelector   m_selector = nullptr;
    };

    // The name this port used before the goal was generalised. MC has one goal
    // and so does this — the player case is the type list being empty.
    using NearestAttackablePlayerGoal = NearestAttackableTargetGoal;

    class Llama;
    class Shulker;

    // MC Llama.LlamaHurtByTargetGoal — retaliation that stands down after ONE
    // spit: canContinueToUse consumes the didSpit flag and drops the grudge,
    // which is why a llama spits once at whatever hit it and moves on.
    class LlamaHurtByTargetGoal : public HurtByTargetGoal {
    public:
        explicit LlamaHurtByTargetGoal(Llama* llama);

        bool CanContinueToUse() override;
        const char* Name() const override { return "LlamaHurtByTargetGoal"; }

    private:
        Llama* m_llama;
    };

    // MC Llama.LlamaAttackWolfGoal — hunt UNTAMED wolves (no taming system,
    // so every wolf qualifies), at a quarter of the usual follow distance and
    // without needing line of sight first.
    class LlamaAttackWolfGoal : public NearestAttackableTargetGoal {
    public:
        explicit LlamaAttackWolfGoal(Llama* llama);

        const char* Name() const override { return "LlamaAttackWolfGoal"; }

    protected:
        double GetFollowDistance() const override;
    };

    // MC Shulker.ShulkerNearestAttackGoal — the player hunt, gated off on
    // peaceful. (MC also reshapes the target-search box along the attach
    // axis; the standard follow-range volume stands in for that here.)
    class ShulkerNearestAttackGoal : public NearestAttackableTargetGoal {
    public:
        explicit ShulkerNearestAttackGoal(Shulker* shulker);

        bool CanUse() override;
        const char* Name() const override { return "ShulkerNearestAttackGoal"; }

    private:
        Shulker* m_shulker;
    };

    // MC Shulker.ShulkerDefenseAttackGoal — defends the shulker's scoreboard
    // TEAM against hostile mobs. MC's own canUse gate (`getTeam() == null →
    // false`) never opens outside a team, and no team system exists here, so
    // this never runs — kept so the shulker's goal table reads like MC's.
    class ShulkerDefenseAttackGoal : public NearestAttackableTargetGoal {
    public:
        explicit ShulkerDefenseAttackGoal(Shulker* shulker);

        bool CanUse() override { return false; }
        const char* Name() const override { return "ShulkerDefenseAttackGoal"; }
    };

    // MC NearestAttackableWitchTargetGoal — the witch's player hunt with the
    // canAttack toggle her raid-healing goal flips while she tends raiders.
    // No raids exist, so the toggle simply stays on.
    class NearestAttackableWitchTargetGoal : public NearestAttackableTargetGoal {
    public:
        NearestAttackableWitchTargetGoal(Mob* mob, bool mustSee, bool mustReach,
                                         int randomInterval)
            : NearestAttackableTargetGoal(mob, mustSee, mustReach, randomInterval) {}

        void SetCanAttack(bool v) { m_canAttack = v; }
        bool CanUse() override {
            return m_canAttack && NearestAttackableTargetGoal::CanUse();
        }
        const char* Name() const override {
            return "NearestAttackableWitchTargetGoal";
        }

    private:
        bool m_canAttack = true;
    };

    // MC PolarBear.PolarBearAttackPlayersGoal — hunt players (interval 20,
    // must see, must reach), but ONLY while a cub is nearby (8x4x8 box), at
    // half the usual follow distance, and never as a cub. This is why adult
    // polar bears are neutral until you walk up to a family. MC nests it in
    // PolarBear.java; it lives here because every goal class in this port
    // does.
    class PolarBearAttackPlayersGoal : public NearestAttackableTargetGoal {
    public:
        explicit PolarBearAttackPlayersGoal(PolarBear* bear);

        bool CanUse() override;
        const char* Name() const override { return "PolarBearAttackPlayersGoal"; }

    protected:
        double GetFollowDistance() const override;
    };

    // MC Spider.SpiderTargetGoal — target acquisition gated on darkness: at a
    // light-level magic value of 0.5 or more (brightness >= 13) the spider
    // stops hunting entirely. MC nests it in Spider.java; it lives here
    // because every goal class in this port does.
    class SpiderTargetGoal : public NearestAttackableTargetGoal {
    public:
        // The player form and the type-list form, mirroring the base.
        SpiderTargetGoal(Mob* mob, bool mustSee)
            : NearestAttackableTargetGoal(mob, mustSee) {}
        SpiderTargetGoal(Mob* mob, const EntityTypeId* types, int typeCount, bool mustSee)
            : NearestAttackableTargetGoal(mob, types, typeCount, mustSee) {}

        bool CanUse() override;
        const char* Name() const override { return "SpiderTargetGoal"; }
    };

    // MC ai/goal/target/ResetUniversalAngerTargetGoal<T extends Mob &
    // NeutralMob> — when the UNIVERSAL_ANGER game rule is on, a mob hurt by
    // a player forgets its specific grudge and becomes angry at ALL players
    // (optionally alerting every same-type neighbour to do the same).
    //
    // The game rule defaults OFF and no game-rule system exists here, so
    // canUse never opens — exactly MC's behaviour in a default world. The
    // start/alert machinery is ported whole so flipping the constant is all
    // a future game-rule system needs.
    //
    // MC's <T extends Mob & NeutralMob> intersection type is carried as the
    // Mob* plus a cross-cast to NeutralMob at construction.
    class ResetUniversalAngerTargetGoal : public Goal {
    public:
        // The UNIVERSAL_ANGER game rule (MC default: false).
        static constexpr bool kUniversalAnger = false;

        static constexpr int kAlertRangeY = 10;   // MC ALERT_RANGE_Y

        ResetUniversalAngerTargetGoal(Mob* mob, bool alertOthersOfSameType);

        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "ResetUniversalAngerTargetGoal"; }

    private:
        bool WasHurtByPlayer() const;

        Mob*        m_mob;
        NeutralMob* m_neutral;
        bool        m_alertOthersOfSameType;
        int64_t     m_lastHurtByPlayerTimestamp = 0;
    };

} // namespace Game
