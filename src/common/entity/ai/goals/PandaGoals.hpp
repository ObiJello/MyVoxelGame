// File: src/common/entity/ai/goals/PandaGoals.hpp
//
// MC animal/panda/Panda.java's nested goals. MC declares them as inner
// classes of Panda; they live here because every goal class in this port
// does. PandaMoveControl is file-local in PandaGoals.cpp, reached through
// MakePandaMoveControl.
#pragma once

#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"

#include <memory>

namespace Game {

    class Panda;
    class MoveControl;

    // MC's PandaMoveControl — steers only while the panda canPerformAction.
    // Class file-local to PandaGoals.cpp.
    std::unique_ptr<MoveControl> MakePandaMoveControl(Panda* panda);

    // MC Panda.PandaPanicGoal — pandas only panic at ENVIRONMENTAL damage
    // (DamageTypeTags.PANIC_ENVIRONMENTAL_CAUSES: fire, freezing — never an
    // attacker), and a panda that has sat down stops fleeing.
    class PandaPanicGoal : public PanicGoal {
    public:
        PandaPanicGoal(Panda* panda, double speedModifier);
        bool CanContinueToUse() override;
        // Keeps the base name so PathfinderMob::IsPanicking still sees it
        // (MC tests `instanceof PanicGoal`; the PolarBear precedent).
        const char* Name() const override { return "PanicGoal"; }

    protected:
        bool ShouldPanic() const override;

    private:
        Panda* m_panda;
    };

    // MC Panda.PandaBreedGoal — breeding demands bamboo within 8 blocks; a
    // panda that cannot find any goes unhappy for 32 ticks (600-tick
    // cooldown) and glares at the nearest player through the look goal.
    class PandaBreedGoal : public BreedGoal {
    public:
        PandaBreedGoal(Panda* panda, double speedModifier);
        bool CanUse() override;
        const char* Name() const override { return "PandaBreedGoal"; }

    private:
        bool CanFindBamboo() const;

        Panda* m_panda;
        int    m_unhappyCooldown = 0;
    };

    // MC Panda.PandaAttackGoal — melee gated on canPerformAction.
    class PandaAttackGoal : public MeleeAttackGoal {
    public:
        PandaAttackGoal(Panda* panda, double speedModifier, bool trackTarget);
        bool CanUse() override;
        const char* Name() const override { return "PandaAttackGoal"; }

    private:
        Panda* m_panda;
    };

    // MC Panda.PandaAvoidGoal — only WORRIED pandas flee (players at 8,
    // monsters at 4, both 2.0/2.0).
    class PandaAvoidGoal : public AvoidEntityGoal {
    public:
        // Player form and type-list form, mirroring the base.
        PandaAvoidGoal(Panda* panda, float maxDistance,
                       double walkSpeedModifier, double sprintSpeedModifier);
        PandaAvoidGoal(Panda* panda, const EntityTypeId* types, int typeCount,
                       float maxDistance, double walkSpeedModifier,
                       double sprintSpeedModifier);

        bool CanUse() override;
        const char* Name() const override { return "PandaAvoidGoal"; }

    private:
        Panda* m_panda;
    };

    // MC Panda.PandaSitGoal — sit down to eat what the panda holds or spots
    // on the ground. Every trigger is a mob-held item or an ItemEntity query
    // (canPickUpAndEat), and this port has no mob item pickup, so MC's own
    // canUse never opens. (The worried thunderstorm sit is Panda::Tick's, and
    // the FED-bamboo sit is Panda::MobInteract's TryToSit — both real; only
    // this goal's hold-and-chew loop is inert until the item layer lands.)
    class PandaSitGoal : public Goal {
    public:
        explicit PandaSitGoal(Panda* panda) : m_panda(panda) {
            SetFlags(static_cast<uint8_t>(GoalFlag::Move));
        }
        bool CanUse() override { return false; }
        const char* Name() const override { return "PandaSitGoal"; }

    private:
        Panda* m_panda;
    };

    // MC Panda.PandaLieOnBackGoal — lazy pandas flop onto their backs.
    class PandaLieOnBackGoal : public Goal {
    public:
        explicit PandaLieOnBackGoal(Panda* panda);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "PandaLieOnBackGoal"; }

    private:
        Panda* m_panda;
        int    m_cooldown = 0;
    };

    // MC Panda.PandaSneezeGoal — cubs sneeze (weak cubs 1-in-500 per poll,
    // the rest 1-in-6000); the sneeze itself runs on Panda's tick clock.
    class PandaSneezeGoal : public Goal {
    public:
        explicit PandaSneezeGoal(Panda* panda) : m_panda(panda) {}
        bool CanUse() override;
        bool CanContinueToUse() override { return false; }
        void Start() override;
        const char* Name() const override { return "PandaSneezeGoal"; }

    private:
        Panda* m_panda;
    };

    // MC Panda.PandaRollGoal — cubs and playful pandas somersault: always
    // when facing a ledge, 1-in-60 for playful ones, 1-in-500 otherwise.
    class PandaRollGoal : public Goal {
    public:
        explicit PandaRollGoal(Panda* panda);
        bool CanUse() override;
        bool CanContinueToUse() override { return false; }
        bool IsInterruptable() const override { return false; }
        void Start() override;
        const char* Name() const override { return "PandaRollGoal"; }

    private:
        Panda* m_panda;
    };

    // MC Panda.PandaLookAtPlayerGoal — the shared look goal plus an injected
    // stare target (PandaBreedGoal points an unhappy panda at the nearest
    // player) and the canPerformAction gate. Standalone rather than a
    // LookAtPlayerGoal subclass because the base keeps its look target
    // private and this one must be handed a target from outside.
    class PandaLookAtPlayerGoal : public Goal {
    public:
        PandaLookAtPlayerGoal(Panda* panda, float lookDistance);

        // MC's setTarget — the breed goal's unhappy stare.
        void SetTarget(LivingEntity* entity) { m_lookAt = entity; }

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "PandaLookAtPlayerGoal"; }

    private:
        Panda*        m_panda;
        LivingEntity* m_lookAt = nullptr;
        float         m_lookDistance;
        int           m_lookTime = 0;
    };

    // MC Panda.PandaHurtByTargetGoal — retaliation that stands down once the
    // panda got its bite in (didBite; MC's gotBamboo half rides the item
    // layer), and alerts only AGGRESSIVE-gene pandas.
    class PandaHurtByTargetGoal : public HurtByTargetGoal {
    public:
        explicit PandaHurtByTargetGoal(Panda* panda);
        bool CanContinueToUse() override;
        const char* Name() const override { return "PandaHurtByTargetGoal"; }

    protected:
        void AlertOther(Mob& other, LivingEntity& attacker) override;

    private:
        Panda* m_panda;
    };

} // namespace Game
