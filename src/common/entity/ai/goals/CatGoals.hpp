// File: src/common/entity/ai/goals/CatGoals.hpp
//
// MC's feline goals — OcelotAttackGoal (a top-level MC goal shared by cat
// and ocelot), the nested goals of Cat.java and Ocelot.java, and
// NonTameRandomTargetGoal. They live here because every goal class in this
// port does.
//
// The tame-gated half of the cat's table (SitWhenOrderedTo, RelaxOnOwner,
// LieOnBed, FollowOwner, SitOnBlock) is declared with MC's own gate, which
// can never open without the ownership system — each says so at its site.
#pragma once

#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"

#include <vector>

namespace Game {

    class Cat;
    class Ocelot;

    // MC ai/goal/OcelotAttackGoal — the feline stalk-fight: creep at 0.6
    // inside 15 blocks, sprint at 1.33 between melee range and 4 blocks, and
    // bite on a 20-tick clock inside twice the body width. The move-control
    // speed it sets is what Cat/Ocelot::CustomServerAiStep turns into the
    // crouch pose and sprint flag.
    class OcelotAttackGoal : public Goal {
    public:
        explicit OcelotAttackGoal(Mob* mob);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "OcelotAttackGoal"; }

    private:
        Mob*          m_mob;
        LivingEntity* m_target = nullptr;
        int           m_attackTime = 0;
    };

    // MC Ocelot.OcelotTemptGoal — TemptGoal(0.6, OCELOT_FOOD, canScare) whose
    // canScare is waived for a TRUSTING ocelot (Ocelot.mobInteract's fed-fish
    // trust roll sets it).
    class OcelotTemptGoal : public TemptGoal {
    public:
        OcelotTemptGoal(Ocelot* ocelot, double speedModifier, bool canScare);
        const char* Name() const override { return "OcelotTemptGoal"; }

    protected:
        bool CanScare() const override;

    private:
        Ocelot* m_ocelot;
    };

    // MC Cat.CatTemptGoal — TemptGoal(0.6, CAT_FOOD, canScare) gated on
    // !isTame. MC also lets a randomly "selected" player tempt without
    // scaring for a while (the 1-in-600 selectedPlayer roll); that detail
    // stays skipped — the base canScare behaviour stands in.
    class CatTemptGoal : public TemptGoal {
    public:
        CatTemptGoal(Cat* cat, double speedModifier, bool canScare);
        bool CanUse() override;
        const char* Name() const override { return "CatTemptGoal"; }

    private:
        Cat* m_cat;
    };

    // MC Cat.CatAvoidEntityGoal / Ocelot.OcelotAvoidEntityGoal — wild/
    // untrusting felines flee players (16 blocks, 0.8 walk, 1.33 sprint).
    // MC swaps these in and out via reassessTameGoals/reassessTrustingGoals;
    // the isTame/isTrusting gates here are the belt to that suspender.
    class CatAvoidEntityGoal : public AvoidEntityGoal {
    public:
        CatAvoidEntityGoal(Cat* cat, float maxDistance,
                           double walkSpeedModifier, double sprintSpeedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        const char* Name() const override { return "CatAvoidEntityGoal"; }

    private:
        Cat* m_cat;
    };

    class OcelotAvoidEntityGoal : public AvoidEntityGoal {
    public:
        OcelotAvoidEntityGoal(Ocelot* ocelot, float maxDistance,
                              double walkSpeedModifier, double sprintSpeedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        const char* Name() const override { return "OcelotAvoidEntityGoal"; }

    private:
        Ocelot* m_ocelot;
    };

    // MC target/NonTameRandomTargetGoal — the wild feline/wolf hunt: a
    // NearestAttackableTargetGoal gated on !isTame, carrying a per-target
    // predicate (plain rabbits; baby turtles on land; the wolf's
    // sheep/rabbit/fox PREY_SELECTOR as a type list) the shared target goal
    // cannot express — so this one searches itself on TargetGoal's plumbing.
    class NonTameRandomTargetGoal : public TargetGoal {
    public:
        NonTameRandomTargetGoal(Mob* mob, EntityTypeId targetType, bool mustSee,
                                bool babyOnLandOnly);
        // The type-list form — MC's Class<Animal> + selector shape, used by
        // the wolf's PREY_SELECTOR (sheep, rabbit, fox).
        NonTameRandomTargetGoal(Mob* mob, const EntityTypeId* targetTypes,
                                int targetTypeCount, bool mustSee,
                                bool babyOnLandOnly);
        bool CanUse() override;
        void Start() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "NonTameRandomTargetGoal"; }

    private:
        std::vector<EntityTypeId> m_targetTypes;
        bool          m_babyOnLandOnly;
        LivingEntity* m_found = nullptr;
    };

    // ── The still-inert cat goals ──────────────────────────────────────────
    //
    // Taming/ownership landed (TamableAnimal + TamableGoals: sit-on-command
    // and follow-owner are REAL now), but these three hang on beds — a block
    // this engine does not have — so MC's own gates still never open and the
    // honest port of each remains the gate. Kept so the cat's goal table
    // reads like MC's and each has a home when beds land.

    // MC Cat.CatRelaxOnOwnerGoal — curl up beside a SLEEPING OWNER on a bed
    // (the writer of IS_LYING / RELAX_STATE_ONE, and the morning gift).
    // Needs player sleep + beds.
    class CatRelaxOnOwnerGoal : public Goal {
    public:
        explicit CatRelaxOnOwnerGoal(Cat* cat) : m_cat(cat) {}
        bool CanUse() override { return false; }   // needs owner ASLEEP on a bed
        const char* Name() const override { return "CatRelaxOnOwnerGoal"; }

    private:
        Cat* m_cat;
    };

    // MC CatLieOnBedGoal — a TAME cat seeks a bed to lie on. Needs beds.
    class CatLieOnBedGoal : public Goal {
    public:
        CatLieOnBedGoal(Cat* cat, double speedModifier, int searchRange)
            : m_cat(cat) { (void)speedModifier; (void)searchRange; }
        bool CanUse() override { return false; }   // needs a bed to find
        const char* Name() const override { return "CatLieOnBedGoal"; }

    private:
        Cat* m_cat;
    };

    // MC CatSitOnBlockGoal — a TAME cat perches on chests, beds, slabs and
    // lit furnaces; every block in its whitelist test is a block entity or
    // bed this engine does not model, so the search would find nothing.
    class CatSitOnBlockGoal : public Goal {
    public:
        CatSitOnBlockGoal(Cat* cat, double speedModifier)
            : m_cat(cat) { (void)speedModifier; }
        bool CanUse() override { return false; }   // no sit-worthy blocks exist
        const char* Name() const override { return "CatSitOnBlockGoal"; }

    private:
        Cat* m_cat;
    };

} // namespace Game
