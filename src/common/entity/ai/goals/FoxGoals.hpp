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
#include "common/entity/ai/goals/MoveToBlockGoal.hpp"
#include "common/world/block/BlockState.hpp"

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
    // makes the cub trust the love-cause players — Fox::SpawnChildFromBreeding
    // (the shared path BreedGoal calls) does that half.
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
    // Its checkAndPerformAttack adds FOX_BITE after the swing.
    class FoxMeleeAttackGoal : public MeleeAttackGoal {
    public:
        FoxMeleeAttackGoal(Fox* fox, double speedModifier, bool trackTarget);
        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "FoxMeleeAttackGoal"; }

    protected:
        void CheckAndPerformAttack(LivingEntity& target) override;

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

    // MC Fox.FoxEatBerriesGoal — a MoveToBlockGoal (1.2, 12, 1) to a sweet
    // berry bush of age >= 2 or a glow-berried cave vine; 40 ticks at the
    // bush (sniffing on the way, 5% a tick), then — mobGriefing on — it
    // picks: a berry bush drops to age 1 and yields 1 + nextInt(2) (+1 at
    // age 3) sweet berries, one into an empty mouth and the rest popped; a
    // vine loses its berries (CaveVines.use: one glow berry popped).
    class FoxEatBerriesGoal : public MoveToBlockGoal {
    public:
        FoxEatBerriesGoal(Fox* fox, double speedModifier, int searchRange,
                          int verticalSearchRange);
        bool CanUse() override;
        void Start() override;
        void Tick() override;
        const char* Name() const override { return "FoxEatBerriesGoal"; }

    protected:
        double AcceptedDistance() const override { return 2.0; }
        bool ShouldRecalculatePath() const override { return m_tryTicks % 100 == 0; }
        bool IsValidTarget(const IBlockAccess& blocks, const glm::ivec3& pos) const override;

    private:
        void OnReachedTarget();
        void PickSweetBerries(BlockState state);
        void PickGlowBerry(BlockState state);

        Fox* m_fox;
        int  m_ticksWaited = 0;
    };

    // MC Fox.FoxSearchForItemsGoal — an empty-mouthed, unthreatened fox that
    // can move (1 in reducedTickDelay(10) per check) walks at 1.2 to the
    // first pickup-ready item entity within 8 blocks; Mob's looting then
    // takes it into the mouth.
    class FoxSearchForItemsGoal : public Goal {
    public:
        explicit FoxSearchForItemsGoal(Fox* fox) : m_fox(fox) {
            SetFlags(static_cast<uint8_t>(GoalFlag::Move));
        }
        bool CanUse() override;
        void Start() override;
        void Tick() override;
        const char* Name() const override { return "FoxSearchForItemsGoal"; }

    private:
        // The first item entity (ALLOWED_ITEMS: past its pickup delay) in
        // the fox's box inflated by 8 — false when there is none.
        bool FindItem(glm::dvec3& out) const;

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

    // MC Fox.DefendTrustedTargetGoal (a NearestAttackableTargetGoal, random
    // interval 10, mustSee/mustReach false) — retaliate for whatever last
    // hurt a TRUSTED entity: the first trusted identity that resolves in
    // this level is checked; a new hurt-by timestamp on it, and an attacker
    // passing the goal's selector (TRUSTED_TARGET_SELECTOR — it has hurt
    // something within its last 600 ticks — and not itself trusted), makes
    // the fox defend: aggro sound, DEFENDING flag, awake, targeting it.
    class DefendTrustedTargetGoal : public TargetGoal {
    public:
        explicit DefendTrustedTargetGoal(Fox* fox);
        bool CanUse() override;
        void Start() override;
        void ClearReferenceTo(const Entity* entity) override;
        const char* Name() const override { return "DefendTrustedTargetGoal"; }

    private:
        Fox*          m_fox;
        LivingEntity* m_trustedLastHurtBy = nullptr;
        LivingEntity* m_trustedLastHurt = nullptr;
        int64_t       m_timestamp = 0;
    };

    // MC registers plain AvoidEntityGoals on the fox with per-goal lambda
    // gates: players — AVOID_PLAYERS (not sneaking, not creative/spectator)
    // && !trusts && !isDefending; wolves — !isTame && !isDefending; polar
    // bears — !isDefending. The per-candidate halves are AcceptsThreat;
    // !isDefending gates the goal itself.
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

    protected:
        bool AcceptsThreat(const LivingEntity& candidate) const override;

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
