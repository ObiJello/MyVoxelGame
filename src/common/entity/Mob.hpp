// File: src/common/entity/Mob.hpp
//
// MC net.minecraft.world.entity.Mob — a LivingEntity with goals, navigation
// and control loops.
//
// The ordering inside ServerAiStep is the most load-bearing thing in the whole
// mob port, so it is spelled out rather than left to the .cpp:
//
//   sensing.Tick()                    clear the line-of-sight cache
//   (tickCount + id) % 2 == 0 ?       full goal evaluation on even ticks,
//     selectors.Tick()                running-goal ticks on odd ones. The id
//   : selectors.TickRunningGoals()    term staggers mobs so a herd does not
//                                     all re-evaluate on the same tick.
//   navigation.Tick()                 advance along the path, which calls
//                                     MoveControl::SetWantedPosition
//   CustomServerAiStep()              per-mob extras (creeper fuse, sheep eat)
//   moveControl.Tick()                wanted position -> yaw + zza
//   lookControl.Tick()                head yaw + pitch
//   jumpControl.Tick()                latch -> jumping flag
//
// Everything above only sets inputs. LivingEntity::AiStep then consumes them
// in the jump and travel phases of the SAME tick — which is why ServerAiStep is
// called from inside AiStep and not before it.
#pragma once

#include "common/entity/LivingEntity.hpp"

#include <algorithm>
#include "common/entity/AnimationState.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/world/pathfinder/PathType.hpp"

#include <memory>
#include <unordered_map>

namespace Game {

    class PathNavigation;
    class Sensing;

    class Mob : public LivingEntity {
    public:
        Mob(EntityTypeId type, EntityLevel* level);
        ~Mob() override;

        // ── AI plumbing ────────────────────────────────────────────────────
        GoalSelector& Goals()   { return m_goalSelector; }
        GoalSelector& Targets() { return m_targetSelector; }

        MoveControl& GetMoveControl() { return *m_moveControl; }
        LookControl& GetLookControl() { return *m_lookControl; }
        JumpControl& GetJumpControl() { return *m_jumpControl; }

        // Defined out of line: PathNavigation and Sensing are only
        // forward-declared here (including them would be circular — both
        // depend on Mob), and dereferencing a unique_ptr to an incomplete type
        // in an inline body would force every caller to include them too.
        PathNavigation&       GetNavigation();
        const PathNavigation& GetNavigation() const;
        Sensing&              GetSensing();

        // ── Target ─────────────────────────────────────────────────────────
        LivingEntity* GetTarget() const { return m_target; }
        virtual void  SetTarget(LivingEntity* target) { m_target = target; }

        // MC Mob.canAttack — overridden by Creeper (ignores goats) and by the
        // player adapter (never attackable in creative/spectator).
        virtual bool CanAttack(const LivingEntity& target) const;

        // ── Aggression / state flags (MC DATA_MOB_FLAGS_ID) ───────────────
        bool IsAggressive() const { return m_aggressive; }
        void SetAggressive(bool v) { m_aggressive = v; }
        bool IsNoAi() const { return m_noAi; }
        void SetNoAi(bool v) { m_noAi = v; }

        // ── Persistence / despawn ──────────────────────────────────────────
        bool IsPersistenceRequired() const { return m_persistenceRequired; }
        void SetPersistenceRequired(bool v) { m_persistenceRequired = v; }

        // MC Mob.removeWhenFarAway — true means "eligible for despawn". The
        // base says yes; Animal overrides to no, which is why cows you walked
        // away from are still there when you come back.
        virtual bool RemoveWhenFarAway(double distanceToClosestPlayerSq) const { return true; }

        // MC Mob.checkDespawn. Runs BEFORE tick() for every mob, whether or not
        // it is in ticking range.
        virtual void CheckDespawn();

        // MC Mob.finalizeSpawn — the post-construction randomisation every
        // spawn path runs: EntityType.create calls it for spawn eggs and for
        // /summon, and NaturalSpawner calls it for natural spawns. Anything
        // rolled once at spawn (a sheep's wool colour) belongs here rather
        // than in the constructor, so that an egg, /summon and the spawner
        // agree instead of each inventing their own answer.
        //
        // Called AFTER the position is set — a subclass may read the biome it
        // landed in.
        virtual void FinalizeSpawn() {}

        // MC Mob.mobInteract — the ENTITY's own answer to a right-click,
        // e.g. shears on a sheep or a saddle on a pig.
        //
        // MC Player.interactOn runs this BEFORE the held item's
        // Item.interactLivingEntity, and only falls through to the item when
        // this returns a non-consuming result. Returning Pass is what lets dye
        // reach a sheep at all.
        virtual UseResult MobInteract(LivingEntity& player, ItemStack& held) {
            (void)player; (void)held;
            return UseResult::Pass;
        }

        int  GetNoActionTime() const { return m_noActionTime; }
        void SetNoActionTime(int t) { m_noActionTime = t; }

        // MC Mob.getMaxSpawnClusterSize — how many of this type one spawn
        // attempt may place.
        virtual int GetMaxSpawnClusterSize() const { return 4; }
        virtual bool IsMaxGroupSizeReached(int groupSize) const { return false; }

        // ── Pathfinding maluses (MC Mob.setPathfindingMalus) ──────────────
        float GetPathfindingMalus(PathType type) const;
        void  SetPathfindingMalus(PathType type, float malus);

        // ── Combat ─────────────────────────────────────────────────────────
        // MC Mob.doHurtTarget. Returns whether the hit landed.
        virtual bool DoHurtTarget(Entity& target);

        // MC Mob.isWithinMeleeAttackRange — an AABB overlap test against the
        // attacker's box inflated by the reach, NOT a centre-to-centre
        // distance. Using distance instead makes wide mobs (spiders) unable to
        // reach a target their body is already touching.
        bool IsWithinMeleeAttackRange(const LivingEntity& target) const;

        static constexpr double kDefaultAttackReach = 0.8284271247461903; // sqrt(2.04) - 0.6

        // ── Inputs (Mob couples speed to forward motion) ───────────────────
        // MC Mob.setSpeed sets BOTH `speed` and `zza`. A mob that only had its
        // `speed` set would face the right way and never move.
        void SetSpeed(float s) override {
            const float v = s * m_landSpeedFactor;
            LivingEntity::SetSpeed(v);
            zza = v;
        }

        // MC SmoothSwimmingMoveControl's outsideWaterSpeedModifier — the last
        // multiplier before setSpeed, applied only by the mobs MC gives that
        // control to (frog, dolphin, tadpole 0.1; nautilus 0.0). Those mobs
        // carry a MOVEMENT_SPEED of 1.0 or more because it is a SWIM speed;
        // without the factor a frog walks at five times a cow. 1.0 for every
        // mob on the ordinary MoveControl, which is all the rest.
        float GetLandSpeedFactor() const { return m_landSpeedFactor; }
        void  SetLandSpeedFactor(float f) { m_landSpeedFactor = f; }

        // MC LivingEntity.updateWalkAnimation and the three overrides of it.
        // The defaults ARE LivingEntity's, so a hand-written mob that never
        // calls the setter behaves exactly as before.
        void SetWalkAnimParams(float scale, float cap, float factor, float babyScale) {
            m_walkAnimScale = scale;
            m_walkAnimCap = cap;
            m_walkAnimFactor = factor;
            m_walkAnimBabyScale = babyScale;
        }

    protected:
        void UpdateWalkAnimation(float distance) override {
            const float target = std::min(distance * m_walkAnimScale, m_walkAnimCap);
            walkAnimation.Update(target, m_walkAnimFactor,
                                 IsBaby() ? m_walkAnimBabyScale : 1.0f);
        }

    public:
        // ── Animation state timers (MC AnimationState) ─────────────────────
        //
        // MC gives each mob named fields; this port gives every mob the same
        // slot table (see MobAnim) so the generated model can name a slot
        // without knowing which class it is talking to.
        //
        // Allocated on first use: nine of ninety mob types have any timers at
        // all, and a fixed member would put 140 unused bytes on every zombie.
        AnimationState& Anim(MobAnim slot) {
            if (!m_animStates) m_animStates = std::make_unique<MobAnimationStates>();
            return (*m_animStates)[static_cast<size_t>(slot)];
        }
        // The const read never allocates — a mob that has never started a clip
        // reports every slot stopped, which is the truth.
        const AnimationState& Anim(MobAnim slot) const {
            static const AnimationState kStopped;
            if (!m_animStates) return kStopped;
            return (*m_animStates)[static_cast<size_t>(slot)];
        }
        bool HasAnimStates() const { return m_animStates != nullptr; }

        // MC's per-mob `setupAnimationStates()`. Called from Tick, CLIENT-SIDE
        // ONLY, exactly where MC calls it — the timers are derived from synched
        // state rather than sent, so running this on the server would start
        // clips nobody ever sees and burn the server's RNG stream doing it.
        virtual void SetupAnimationStates() {}

        // The one synched byte a mob's animations key on beyond the pose — MC's
        // per-class enum (Armadillo.ARMADILLO_STATE, Bat's resting bit). The
        // meaning is private to the subclass on both sides; the tracker just
        // ships whatever the server's copy reports and the client hands it back.
        virtual uint8_t GetAnimStateByte() const { return 0; }
        virtual void    SetAnimStateByte(uint8_t v) { (void)v; }

        void SetZza(float v) { zza = v; }
        void SetXxa(float v) { xxa = v; }
        void SetYya(float v) { yya = v; }

        // MC Mob.stopInPlace — cancel navigation and all steering at once.
        void StopInPlace();

        void Tick() override;

        // Also clears the current target and forwards to every goal in both
        // selectors. See Goal::ClearReferenceTo.
        void ClearReferenceTo(const Entity* entity) override;

        // MC Mob.tickHeadTurn is replaced by the body rotation control.
        int GetMaxHeadXRot() const override { return 40; }
        int GetMaxHeadYRot() const override { return 75; }
        int GetHeadRotSpeed() const override { return 10; }

        // MC Mob.getAmbientSoundInterval — Animal overrides to 120.
        virtual int GetAmbientSoundInterval() const { return 80; }

    protected:
        // Subclasses register their goals here. Called once from the concrete
        // mob's constructor — NOT from Mob's, because a virtual call during
        // base construction would dispatch to the base version.
        virtual void RegisterGoals() {}

        // MC Mob.customServerAiStep — per-mob work that must happen after
        // navigation but before the controls.
        //
        // A brain mob's whole AI runs from here: MC's Frog.customServerAiStep is
        // `getBrain().tick(level, this); FrogAi.updateActivity(this);` and
        // nothing else. TickBrain below is that first half.
        virtual void CustomServerAiStep() {}

        // Runs the brain and then lets the subclass choose the next activity.
        // Split so that a mob with a brain does not also have to remember the
        // tick order — MC ticks the brain BEFORE updating the activity, so a
        // behaviour that writes a memory this tick is seen by the activity
        // switch on the SAME tick.
        void TickBrain();
        virtual void UpdateBrainActivity() {}

        void ServerAiStep() final;
        void TickHeadTurn(float yBodyRotTarget) override;
        void BaseTick() override;

        // MC Mob.aiStep — the base step, then the daylight burn. The order is
        // MC's and it matters: burnUndead draws from the level random, so
        // running it first would shift every roll the base step makes.
        void AiStep() override;

    protected:
        // MC's EntityTypeTags.BURN_IN_DAYLIGHT membership. A tag in vanilla,
        // an override here — with eight mobs a data file would be more
        // machinery than the two `return true`s it replaces.
        virtual bool BurnsInDaylight() const { return false; }

        // MC Mob.isSunBurnTick / burnUndead. NOT const: the brightness roll
        // consumes the level's random exactly once per tick per burning mob,
        // and that draw is part of the shared spawn/AI RNG stream.
        bool IsSunBurnTick();
        void BurnUndead();

        // MC Mob.updateControlFlags — every 5 ticks, and only relevant once
        // riding exists. Kept so the cadence is visible.
        void UpdateControlFlags();

        float m_landSpeedFactor = 1.0f;

        float m_walkAnimScale     = 4.0f;   // LivingEntity.java:2501
        float m_walkAnimCap       = 1.0f;
        float m_walkAnimFactor    = 0.4f;
        float m_walkAnimBabyScale = 3.0f;

        GoalSelector m_goalSelector;
        GoalSelector m_targetSelector;

        std::unique_ptr<MoveControl>         m_moveControl;
        std::unique_ptr<LookControl>         m_lookControl;
        std::unique_ptr<JumpControl>         m_jumpControl;
        std::unique_ptr<BodyRotationControl> m_bodyRotationControl;
        std::unique_ptr<PathNavigation>      m_navigation;
        std::unique_ptr<Sensing>             m_sensing;

        LivingEntity* m_target = nullptr;

        bool m_aggressive = false;
        bool m_noAi = false;
        bool m_persistenceRequired = false;

        int  m_noActionTime = 0;
        int  m_ambientSoundTime = 0;

        // Sparse: only the types a mob actually overrides. Everything else
        // falls through to PathType's default malus.
        std::unordered_map<uint8_t, float> m_pathfindingMalus;

        std::unique_ptr<MobAnimationStates> m_animStates;
    };

    // MC PathfinderMob — a Mob that walks. Adds the walk-target cost that the
    // spawner uses to reject a position the mob would immediately flee.
    class PathfinderMob : public Mob {
    public:
        PathfinderMob(EntityTypeId type, EntityLevel* level) : Mob(type, level) {}

        virtual float GetWalkTargetValue(const glm::ivec3& pos) const { return 0.0f; }

        bool IsPathFinding() const;
        bool IsPanicking() const { return m_goalSelector.IsRunning("PanicGoal"); }
    };

} // namespace Game
