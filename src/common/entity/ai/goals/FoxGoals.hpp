// File: src/common/entity/ai/goals/FoxGoals.hpp
//
// MC animal/fox/Fox.java's nested goals. MC declares every one of these as an
// inner class of Fox; they live here because every goal class in this port
// does. The fox's two custom controls (FoxMoveControl / FoxLookControl) are
// file-local in FoxGoals.cpp, reached through the two Make* factories below.
#pragma once

#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"

#include <memory>

namespace Game {

    class Fox;
    class MoveControl;
    class LookControl;

    // MC's FoxMoveControl (ticks only while the fox canMove) and
    // FoxLookControl (frozen while sleeping; keeps the pounce pitch). Class
    // definitions are file-local to FoxGoals.cpp; Fox's constructor takes
    // them through these factories.
    std::unique_ptr<MoveControl> MakeFoxMoveControl(Fox* fox);
    std::unique_ptr<LookControl> MakeFoxLookControl(Fox* fox);

    // MC Fox.FoxFloatGoal — FloatGoal that clears every fox state on start.
    // (MC additionally gates on getFluidHeight(WATER) > 0.25; this port has
    // no per-fluid height query, so plain "in water" stands in — the same
    // predicate the base FloatGoal already uses for every other mob.)
    class FoxFloatGoal : public FloatGoal {
    public:
        explicit FoxFloatGoal(Fox* fox);
        void Start() override;
        const char* Name() const override { return "FoxFloatGoal"; }

    private:
        Fox* m_fox;
    };

    // MC ClimbOnTopOfPowderSnowGoal — canUse is `mob.wasInPowderSnow`, and
    // nothing in this engine ever puts an entity IN powder snow (the block
    // renders but has no entity-sinking behaviour), so the gate never opens.
    // Kept so the fox's (and rabbit's) goal table reads like MC's.
    class ClimbOnTopOfPowderSnowGoal : public Goal {
    public:
        explicit ClimbOnTopOfPowderSnowGoal(Mob* mob) : m_mob(mob) {
            SetFlags(static_cast<uint8_t>(GoalFlag::Jump));
        }
        bool CanUse() override { return false; }
        const char* Name() const override { return "ClimbOnTopOfPowderSnowGoal"; }

    private:
        Mob* m_mob;
    };

    // MC Fox.FaceplantGoal — hold the face-in-the-snow pose for 40 ticks
    // after a missed pounce, then clear it.
    class FaceplantGoal : public Goal {
    public:
        explicit FaceplantGoal(Fox* fox);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "FaceplantGoal"; }

    private:
        Fox* m_fox;
        int  m_countdown = 0;
    };

    // MC Fox.FoxPanicGoal — a defending fox stands its ground.
    class FoxPanicGoal : public PanicGoal {
    public:
        FoxPanicGoal(Fox* fox, double speedModifier);
        // Keeps the base name so PathfinderMob::IsPanicking still sees it
        // (MC tests `instanceof PanicGoal`; the PolarBear precedent).
        const char* Name() const override { return "PanicGoal"; }

    protected:
        bool ShouldPanic() const override;

    private:
        Fox* m_fox;
    };

    // MC Fox.FoxBreedGoal — clears both foxes' states on start. MC's breed()
    // additionally trusts the love-cause players (the trust system rides the
    // item/taming layer this port skips) and snaps the cub to the parent;
    // the baby itself comes from the shared SpawnChildFromBreeding path.
    // Each partner's own FoxBreedGoal clears its own states, which covers
    // MC's clearing of both.
    class FoxBreedGoal : public BreedGoal {
    public:
        FoxBreedGoal(Fox* fox, double speedModifier);
        void Start() override;
        const char* Name() const override { return "FoxBreedGoal"; }

    private:
        Fox* m_fox;
    };

    // MC Fox.StalkPreyGoal — creep toward chicken/rabbit prey beyond 6
    // blocks, then lock into the crouch+interested pose when close or when
    // the pounce path is clear.
    class StalkPreyGoal : public Goal {
    public:
        explicit StalkPreyGoal(Fox* fox);
        bool CanUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "StalkPreyGoal"; }

    private:
        Fox* m_fox;
    };

    // MC Fox.FoxPounceGoal — the leap: fires only from a full crouch with a
    // clear arc, adds (dir * 0.8, 0.9) to the velocity, pitches the body
    // along the arc, bites at <= 2 blocks, and faceplants on snow.
    class FoxPounceGoal : public Goal {
    public:
        explicit FoxPounceGoal(Fox* fox);
        bool CanUse() override;
        bool CanContinueToUse() override;
        bool IsInterruptable() const override { return false; }
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "FoxPounceGoal"; }

    private:
        Fox* m_fox;
    };

    // MC Fox.SeekShelterGoal — a FleeSunGoal subclass in MC. Reimplemented
    // standalone (the port's FleeSunGoal keeps its hide-position search
    // private): in a thunderstorm under open sky, or by day under open sky
    // on the 100-tick interval, walk to a spot the sky cannot see.
    // MC's !isVillage(pos) term is dropped — no villages exist.
    class SeekShelterGoal : public Goal {
    public:
        SeekShelterGoal(Fox* fox, double speedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        const char* Name() const override { return "SeekShelterGoal"; }

    private:
        bool SetWantedPos();

        Fox*   m_fox;
        double m_speedModifier;
        int    m_interval;
        double m_wantedX = 0.0, m_wantedY = 0.0, m_wantedZ = 0.0;
    };

    // MC Fox.FoxMeleeAttackGoal — melee gated off every special fox state.
    // (MC also plays FOX_BITE on the hit; sounds wait on the sound system.)
    class FoxMeleeAttackGoal : public MeleeAttackGoal {
    public:
        FoxMeleeAttackGoal(Fox* fox, double speedModifier, bool trackTarget);
        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "FoxMeleeAttackGoal"; }

    private:
        Fox* m_fox;
    };

    // MC Fox.SleepGoal — nap by day under shelter when nothing alertable is
    // near, after a random 0..140-tick settle countdown.
    class SleepGoal : public Goal {
    public:
        explicit SleepGoal(Fox* fox);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "SleepGoal"; }

    private:
        bool CanSleep();

        Fox* m_fox;
        int  m_countdown;
    };

    // MC Fox.PerchAndSearchGoal — sit down and scan the horizon in 2-4
    // random directions, 80-100 ticks per look.
    class PerchAndSearchGoal : public Goal {
    public:
        explicit PerchAndSearchGoal(Fox* fox);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "PerchAndSearchGoal"; }

    private:
        void ResetLook();

        Fox*   m_fox;
        double m_relX = 0.0, m_relZ = 0.0;
        int    m_lookTime = 0;
        int    m_looksRemaining = 0;
    };

    // MC Fox.FoxFollowParentGoal — a defending cub does not tag along.
    class FoxFollowParentGoal : public FollowParentGoal {
    public:
        FoxFollowParentGoal(Fox* fox, double speedModifier);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        const char* Name() const override { return "FoxFollowParentGoal"; }

    private:
        Fox* m_fox;
    };

    // MC Fox.FoxStrollThroughVillageGoal — needs the village POI system,
    // which does not exist here; StrollThroughVillageGoal's own canUse can
    // never find a village section, so the honest port is an inert gate.
    class FoxStrollThroughVillageGoal : public Goal {
    public:
        FoxStrollThroughVillageGoal(Fox* fox, int searchRadius, int interval)
            : m_fox(fox) { (void)searchRadius; (void)interval; }
        bool CanUse() override { return false; }
        const char* Name() const override { return "FoxStrollThroughVillageGoal"; }

    private:
        Fox* m_fox;
    };

    // MC Fox.FoxEatBerriesGoal — picking a bush reads and writes the
    // SweetBerryBushBlock.AGE block-state property and puts a berry in the
    // fox's mouth; neither per-block ages nor mob-held items exist in this
    // engine yet, so without the AGE reset a fox would farm one bush
    // forever. Inert until block-state properties reach the mob seam.
    class FoxEatBerriesGoal : public Goal {
    public:
        FoxEatBerriesGoal(Fox* fox, double speedModifier, int searchRange,
                          int verticalSearchRange)
            : m_fox(fox) {
            (void)speedModifier; (void)searchRange; (void)verticalSearchRange;
        }
        bool CanUse() override { return false; }
        const char* Name() const override { return "FoxEatBerriesGoal"; }

    private:
        Fox* m_fox;
    };

    // MC Fox.FoxSearchForItemsGoal — walks to dropped ItemEntities to pick
    // them up in the mouth. Mob item pickup does not exist in this port
    // (Mob::CanPickUpLoot is a stored flag nothing consumes), so the goal
    // is inert until it does.
    class FoxSearchForItemsGoal : public Goal {
    public:
        explicit FoxSearchForItemsGoal(Fox* fox) : m_fox(fox) {
            SetFlags(static_cast<uint8_t>(GoalFlag::Move));
        }
        bool CanUse() override { return false; }
        const char* Name() const override { return "FoxSearchForItemsGoal"; }

    private:
        Fox* m_fox;
    };

    // MC Fox.FoxLookAtPlayerGoal — no player-watching mid-stalk or while
    // planted in the snow.
    class FoxLookAtPlayerGoal : public LookAtPlayerGoal {
    public:
        FoxLookAtPlayerGoal(Fox* fox, float lookDistance);
        bool CanUse() override;
        bool CanContinueToUse() override;
        const char* Name() const override { return "FoxLookAtPlayerGoal"; }

    private:
        Fox* m_fox;
    };

    // MC Fox.DefendTrustedTargetGoal — retaliate for whatever last hurt a
    // TRUSTED player. Trust is only ever granted through the item/taming
    // layer (feeding berries to breeding foxes), which this port skips, so
    // the trusted list is permanently empty and MC's own canUse loop over it
    // returns false — exactly what this gate encodes.
    class DefendTrustedTargetGoal : public Goal {
    public:
        explicit DefendTrustedTargetGoal(Fox* fox) : m_fox(fox) {
            SetFlags(static_cast<uint8_t>(GoalFlag::Target));
        }
        bool CanUse() override { return false; }
        const char* Name() const override { return "DefendTrustedTargetGoal"; }

    private:
        Fox* m_fox;
    };

    // MC registers plain AvoidEntityGoals on the fox with per-goal lambda
    // gates (!trusts && !isDefending for players; !isTame && !isDefending
    // for wolves; !isDefending for polar bears). Trust and wolf taming do
    // not exist, so the surviving gate is !isDefending on all three.
    class FoxAvoidEntityGoal : public AvoidEntityGoal {
    public:
        // Player form and type-list form, mirroring the base.
        FoxAvoidEntityGoal(Fox* fox, float maxDistance,
                           double walkSpeedModifier, double sprintSpeedModifier);
        FoxAvoidEntityGoal(Fox* fox, const EntityTypeId* types, int typeCount,
                           float maxDistance, double walkSpeedModifier,
                           double sprintSpeedModifier);

        bool CanUse() override;
        bool CanContinueToUse() override;
        const char* Name() const override { return "FoxAvoidEntityGoal"; }

    private:
        Fox* m_fox;
    };

    // MC's three NearestAttackableTargetGoal registrations on the fox, which
    // carry per-target predicates (chicken-or-rabbit / baby-turtle-on-land /
    // schooling fish) the port's shared goal cannot express — so this one
    // searches itself, on TargetGoal's plumbing.
    class FoxPreyTargetGoal : public TargetGoal {
    public:
        enum class Kind : uint8_t { LandPrey, BabyTurtles, Fish };

        FoxPreyTargetGoal(Fox* fox, Kind kind, int randomInterval);
        bool CanUse() override;
        void Start() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "FoxPreyTargetGoal"; }

    private:
        Fox*          m_fox;
        Kind          m_kind;
        int           m_randomInterval;
        LivingEntity* m_found = nullptr;
    };

} // namespace Game
