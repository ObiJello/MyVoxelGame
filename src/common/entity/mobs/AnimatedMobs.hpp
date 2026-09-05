// File: src/common/entity/mobs/AnimatedMobs.hpp
//
// The mobs whose defining behaviour IS their animation state machine.
//
// MC drives every episodic animation from a Game::AnimationState timer started
// in `setupAnimationStates()` (client-side) or `onSyncedDataUpdated(DATA_POSE)`.
// The keyframes for all of them are already generated — what was missing was
// anything to start the timers, which is why a frog never croaked and a bat
// never flapped even though FROG_CROAK and BAT_FLYING were sitting in
// GeneratedAnimations.cpp.
//
// These four are the ones whose driving state this port can honestly produce:
//
//   Frog       pose CROAKING / USING_TONGUE / LONG_JUMPING, plus a purely
//              client-side swim idle. Croaking needed one goal (MC's Croak
//              behaviour); the rest follow from the pose being on the wire.
//   Camel      a free-running idle timer — no synched state at all.
//   Bat        resting vs flying, from MC's own flight AI, which is a plain
//              customServerAiStep with no brain and ports directly.
//   Armadillo  MC's four-state ARMADILLO_STATE machine plus the scare sensor.
//
// Sniffer, warden, breeze, creaking and the copper golem drive their timers
// from MC's brain, which now exists — each has its own <Mob>Ai in ai/brain/
// and a class below that decodes the synched state into clip starts.
#pragma once

#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/NeutralMob.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <vector>

namespace Game {

    // ── Frog ───────────────────────────────────────────────────────────────

    class Frog : public GenericAnimal {
    public:
        explicit Frog(EntityLevel* level);

        // MC Frog.isBaby is hardcoded false — frogs hatch from tadpoles, they
        // are never baby frogs, and the baby scale would shrink an adult.
        bool IsBaby() const override { return false; }

        void Tick() override;

        // MC Frog.onSyncedDataUpdated's DATA_POSE branch, verbatim.
        void OnPoseUpdated() override;

        // MC Frog.customServerAiStep's second half — FrogAi.updateActivity.
        void UpdateBrainActivity() override;

    protected:
        // MC Frog.updateWalkAnimation — the base multiplier is 25, not 4, AND
        // it drops to zero mid-jump so the frog holds the jump pose instead of
        // running the walk cycle through it.
        void UpdateWalkAnimation(float distance) override;
    };

    // ── Camel ──────────────────────────────────────────────────────────────

    class Camel : public GenericAnimal {
    public:
        // MC's two pose-transition lengths, in ticks. A camel is "in
        // transition" until the relevant one has elapsed, and the sit-down and
        // stand-up clips are exactly these long.
        static constexpr int kSitDownDuration = 40;
        static constexpr int kStandUpDuration = 52;

        // `type` so a camel husk keeps its own EntityTypeId (and therefore its
        // own texture, attributes and loot) while sharing MC's Camel class.
        explicit Camel(EntityLevel* level, EntityTypeId type = EntityTypeId::Camel);

        // MC CamelHusk.removeWhenFarAway (CamelHusk.java:28-30) — the husk
        // despawns like the monster it is; the living camel keeps Animal's
        // never-despawn.
        bool RemoveWhenFarAway(double distSq) const override {
            return GetType() == EntityTypeId::CamelHusk
                       ? true
                       : GenericAnimal::RemoveWhenFarAway(distSq);
        }

        void SetupAnimationStates() override;
        void UpdateBrainActivity() override;
        void OnPoseUpdated() override;
        void Tick() override;

        // MC's whole sit state is ONE synched value: the game tick of the last
        // pose change, NEGATED while sitting. Everything else is derived.
        bool IsCamelSitting() const { return m_lastPoseChangeTick < 0; }
        int64_t GetPoseTime() const;
        bool IsInPoseTransition() const {
            return GetPoseTime() < (IsCamelSitting() ? kSitDownDuration : kStandUpDuration);
        }
        bool IsCamelVisuallySitting() const {
            return (GetPoseTime() < 0) != IsCamelSitting();
        }
        bool IsVisuallySittingDown() const {
            return IsCamelSitting() && GetPoseTime() < kSitDownDuration && GetPoseTime() >= 0;
        }
        // MC Camel.refuseToMove — a sitting or transitioning camel ignores
        // every movement instruction, which is why its brain gates strolling
        // on this.
        bool RefuseToMove() const { return IsCamelSitting() || IsInPoseTransition(); }

        void SitDown();
        void StandUp();
        void StandUpInstantly();

        // MC Camel.DASH — a synched boolean. The ONLY thing that sets it in MC
        // is executeRidersJump (a saddled camel's rider charging the jump bar),
        // so until riding exists the server never raises it — but the whole
        // wire-and-clip path is live: setDashing → anim byte → dash clip, and
        // the 55-tick cooldown / auto-clear in tick are MC's verbatim.
        static constexpr int kDashCooldownTicks = 55;

        bool IsDashing() const { return m_dashing; }
        void SetDashing(bool v) { m_dashing = v; }

        // MC Camel.getJumpCooldown — the dash cooldown, read by
        // CamelRenderer.extractRenderState as state.jumpCooldown.
        int GetJumpCooldown() const { return m_dashCooldown; }

        uint8_t GetAnimStateByte() const override { return m_dashing ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override;

    private:
        void ResetLastPoseChangeTick(int64_t syncedPoseTickTime);

        // MC Camel.dashCooldown — local on both sides; the client rebuilds it
        // from the synched dash flag in onSyncedDataUpdated.
        bool m_dashing = false;
        int  m_dashCooldown = 0;

        // MC Camel.idleAnimationTimeout. No synched state and no server
        // involvement: every client runs its own timer, which is why two
        // players see the same camel sway at different moments in MC too.
        int m_idleAnimationTimeout = 0;

        // MC's LAST_POSE_CHANGE_TICK synched long. Not on the wire here: the
        // POSE already crosses it, and both sides recompute this from the tick
        // they saw the pose change on. The transitions are 40 and 52 ticks and
        // the pose arrives within 3, so the skew is invisible — where sending
        // a 64-bit game tick for one mob would not be.
        int64_t m_lastPoseChangeTick = 0;

    public:
        // MC's "LastPoseTick". The value carries the sitting state in its
        // SIGN (negative while sitting), which is why the save layer copies
        // the raw int64 rather than deriving it from IsCamelSitting().
        int64_t GetLastPoseChangeTick() const { return m_lastPoseChangeTick; }
        void    SetLastPoseChangeTick(int64_t tick) { m_lastPoseChangeTick = tick; }
    };

    // ── Bat ────────────────────────────────────────────────────────────────

    class Bat : public GenericMob {
    public:
        explicit Bat(EntityLevel* level);

        bool IsResting() const { return m_resting; }
        void SetResting(bool v) { m_resting = v; }

        // MC's DATA_ID_FLAGS bit 0. One bit in the shared animation-state byte
        // here; the meaning is private to this class on both sides.
        uint8_t GetAnimStateByte() const override { return m_resting ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_resting = (v & 1) != 0; }

        void Tick() override;
        void SetupAnimationStates() override;

    protected:
        void CustomServerAiStep() override;

    private:
        bool m_resting = true;      // MC's constructor calls setResting(true)
        bool m_hasTarget = false;
        glm::ivec3 m_targetPosition{0};
    };

    // ── Tadpole / Goat / Hoglin ────────────────────────────────────────────
    //
    // Three more mobs off the goal system. Each is only its brain plus whatever
    // state that brain reads — which is the point of having built the brain:
    // the third one costs a constructor.

    // PathfinderMob, not Animal: MC's Tadpole is an AbstractFish and gets
    // CreateMobAttributes, not CreateAnimalAttributes. Picking the wrong base
    // silently changes the attributes the mob is built with.
    class Tadpole : public GenericPathfinderMob {
    public:
        explicit Tadpole(EntityLevel* level);
        void UpdateBrainActivity() override;

        // MC Tadpole is an AbstractFish (WaterAnimal): out of water its air
        // drains and it suffocates — see HandleWaterAnimalAirSupply.
        void BaseTick() override;
    };

    class Goat : public GenericAnimal {
    public:
        explicit Goat(EntityLevel* level);
        void UpdateBrainActivity() override;
    };

    // Animal, not Monster: MC's Hoglin extends Animal and merely implements
    // Enemy, so it breeds and takes the animal attribute set.
    class Hoglin : public GenericAnimal {
    public:
        explicit Hoglin(EntityLevel* level);
        void UpdateBrainActivity() override;

        // MC Hoglin.removeWhenFarAway (Hoglin.java:182-184) — true: hoglins
        // despawn despite being Animals (they are Enemy). Overrides Animal's
        // blanket never-despawn.
        bool RemoveWhenFarAway(double) const override { return true; }

        // MC Hoglin.ageBoundaryReached (Hoglin.java:159-167): xpReward 3 as a
        // baby, 5 as an adult (the type table's monster default).
        int GetXpReward() const override { return IsBaby() ? 3 : TypeInfo().xpReward; }

        // MC Hoglin.doHurtTarget — attackAnimationRemainingTicks=10 + entity
        // event 4 where the brain's melee behaviour lands the hit.
        bool DoHurtTarget(Entity& target) override;

        // MC Hoglin.aiStep — the clock counts down BEFORE super, both sides.
        void AiStep() override;

        // MC Hoglin.handleEntityEvent(4) — start the headbutt animation.
        void HandleEntityEvent(uint8_t id) override;

        // MC HoglinBase.getAttackAnimationRemainingTicks — what
        // AbstractHoglinRenderer.extractRenderState reads.
        int GetAttackAnimationRemainingTicks() const {
            return m_attackAnimationRemainingTicks;
        }

    private:
        int m_attackAnimationRemainingTicks = 0;
    };

    // MC monster/Zoglin — the zombified hoglin, on MC's own small Brain
    // (idle / fight, ZoglinAi here), plus the headbutt animation machinery MC
    // keeps on the entity itself: doHurtTarget arms
    // attackAnimationRemainingTicks + entity event 4, exactly like the hoglin
    // it shares HoglinBase with in MC.
    class Zoglin : public GenericMonster {
    public:
        explicit Zoglin(EntityLevel* level);

        void UpdateBrainActivity() override;

        // MC Zoglin's DATA_BABY_ID. The wire's shared baby flag carries it.
        bool IsBaby() const override { return m_baby; }
        // MC Zoglin.setBaby — a baby zoglin's attack damage drops to 0.5.
        void SetBaby(bool baby) override;

        // MC Zoglin.finalizeSpawn — 20% of spawns are babies.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Zoglin.doHurtTarget — clock + event 4, then the hit
        // (HoglinBase.hurtAndThrowTarget's damage half rides the base;
        // ATTACK_KNOCKBACK from the def covers the shove).
        bool DoHurtTarget(Entity& target) override;

        // MC Zoglin.hurtServer — a hit turns the attacker into the brain's
        // ATTACK_TARGET unless the current target is much closer.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Zoglin.aiStep — the clock counts down BEFORE super, both sides.
        void AiStep() override;

        // MC Zoglin.handleEntityEvent(4) — start the headbutt animation.
        void HandleEntityEvent(uint8_t id) override;

        int GetAttackAnimationRemainingTicks() const {
            return m_attackAnimationRemainingTicks;
        }

    private:
        int  m_attackAnimationRemainingTicks = 0;
        bool m_baby = false;
    };

    // ── Piglin / PiglinBrute ───────────────────────────────────────────────

    // MC monster/piglin/Piglin on the ported PiglinAi brain. The inventory,
    // admiring/bartering, equipment and crossbow halves of the class are
    // SKIPPED (no item or equipment systems); what remains is the brain, the
    // baby flag, the dancing flag, and MC's overworld zombification clock —
    // this engine's one dimension IS the overworld, where PIGLINS_ZOMBIFY is
    // true, so an unprotected piglin converts after 300 ticks exactly as a
    // vanilla piglin brought through a portal does.
    class Piglin : public GenericMonster {
    public:
        explicit Piglin(EntityLevel* level);

        void UpdateBrainActivity() override;

        // MC Piglin's DATA_BABY_ID — the wire's shared baby flag carries it.
        bool IsBaby() const override { return m_baby; }
        // MC Piglin.setBaby — babies move 20% faster (SPEED_MODIFIER_BABY).
        void SetBaby(bool baby) override;

        // MC AbstractPiglin.isAdult.
        bool IsAdult() const { return !IsBaby(); }

        // MC Piglin.canHunt — the CannotHunt save flag.
        bool CanHunt() const { return !m_cannotHunt; }
        void SetCannotHunt(bool v) { m_cannotHunt = v; }

        // MC's DATA_IS_DANCING synched boolean — bit 0 of the anim byte. The
        // renderer's arm-pose/dance input reads it (PiglinArmPose.DANCING).
        bool IsDancing() const { return m_dancing; }
        void SetDancing(bool v) { m_dancing = v; }

        // MC AbstractPiglin's DATA_IMMUNE_TO_ZOMBIFICATION.
        bool IsImmuneToZombification() const { return m_immuneToZombification; }
        void SetImmuneToZombification(bool v) { m_immuneToZombification = v; }

        // MC AbstractPiglin.timeInOverworld — the 300-tick zombification
        // countdown. A save that drops it hands every overworld piglin a
        // fresh 15 seconds on each reload.
        int  GetTimeInOverworld() const { return m_timeInOverworld; }
        void SetTimeInOverworld(int ticks) { m_timeInOverworld = ticks; }

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(m_dancing ? 1 : 0);
        }
        void SetAnimStateByte(uint8_t v) override { m_dancing = (v & 1) != 0; }

        // MC Piglin.finalizeSpawn — 20% baby, initMemories (the spawn-weapon
        // and armor rolls are skipped: no equipment system).
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Piglin.hurtServer → PiglinAi.wasHurtBy.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

    protected:
        // MC AbstractPiglin.customServerAiStep — the zombification clock.
        void CustomServerAiStep() override;

    private:
        bool m_baby = false;
        bool m_cannotHunt = false;
        bool m_dancing = false;
        bool m_immuneToZombification = false;
        int  m_timeInOverworld = 0;   // MC AbstractPiglin.timeInOverworld
    };

    // MC monster/piglin/PiglinBrute on the ported PiglinBruteAi brain — the
    // simpler always-hostile cousin: no baby form, no bartering, never flees,
    // and it patrols the HOME position it spawned at. The golden-axe spawn
    // equipment is skipped (no equipment system).
    class PiglinBrute : public GenericMonster {
    public:
        explicit PiglinBrute(EntityLevel* level);

        void UpdateBrainActivity() override;

        bool IsImmuneToZombification() const { return m_immuneToZombification; }
        void SetImmuneToZombification(bool v) { m_immuneToZombification = v; }

        // MC AbstractPiglin.timeInOverworld — see the note on Piglin's copy.
        int  GetTimeInOverworld() const { return m_timeInOverworld; }
        void SetTimeInOverworld(int ticks) { m_timeInOverworld = ticks; }

        // MC PiglinBrute.finalizeSpawn — PiglinBruteAi.initMemories (HOME).
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC PiglinBrute.hurtServer → PiglinBruteAi.wasHurtBy.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

    protected:
        void CustomServerAiStep() override;

    private:
        bool m_immuneToZombification = false;
        int  m_timeInOverworld = 0;
    };

    // ── Axolotl ────────────────────────────────────────────────────────────

    // MC animal/axolotl/Axolotl on the ported AxolotlAi brain. The bucketing
    // half of the class (Bucketable, fromBucket persistence, mobInteract,
    // saveToBucketTag) is SKIPPED — no bucket-item system; leashing likewise.
    class Axolotl : public GenericAnimal {
    public:
        // MC Axolotl.Variant — the ids are MC's and they are the wire encoding
        // (low 3 bits of the animation-state byte).
        enum class Variant : uint8_t {
            Lucy = 0, Wild = 1, Gold = 2, Cyan = 3, Blue = 4,
        };
        static constexpr int kVariantCount = 5;

        static constexpr int kTotalPlayDeadTime = 200;   // MC TOTAL_PLAYDEAD_TIME
        static constexpr int kMaxAirSupply = 6000;       // MC AXOLOTL_TOTAL_AIR_SUPPLY

        explicit Axolotl(EntityLevel* level);

        Variant GetVariant() const { return m_variant; }
        void    SetVariant(Variant v) { m_variant = v; }

        // MC's DATA_PLAYING_DEAD synched boolean — bit 3 of the anim byte.
        bool IsPlayingDead() const { return m_playingDead; }
        void SetPlayingDead(bool v) { m_playingDead = v; }

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((static_cast<uint8_t>(m_variant) & 0x7)
                                        | (m_playingDead ? 0x8 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            const uint8_t id = v & 0x7;
            m_variant = id < kVariantCount ? static_cast<Variant>(id) : Variant::Lucy;
            m_playingDead = (v & 0x8) != 0;
        }

        // MC Axolotl.getMaxAirSupply — 6000 ticks of air, and it dries OUT of
        // water rather than drowning in it (handleAirSupply in BaseTick).
        int GetMaxAirSupply() const override { return kMaxAirSupply; }

        void BaseTick() override;
        // MC 26.2 Axolotl.tickBabyAnimations (client): one of seven keyframe
        // AnimationStates runs at a time for the remodeled baby — the adult
        // keeps its four easing animators (TickAnimations).
        void TickBabyAnimations();
        void UpdateBrainActivity() override;

        // MC Axolotl.hurtServer — the play-dead roll on a qualifying hit.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Axolotl.getMaxHeadXRot/getMaxHeadYRot — 1 degree: the body turns,
        // never the head alone.
        int GetMaxHeadXRot() const override { return 1; }
        int GetMaxHeadYRot() const override { return 1; }

        // MC Axolotl.getWalkTargetValue — flat 0.
        float GetWalkTargetValue(const glm::ivec3&) const override { return 0.0f; }

        // MC Axolotl.finalizeSpawn — the two-variant pack token; the third
        // member onward of a pack spawns as a baby.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Axolotl.getBreedOffspring — 1-in-1200 rare (blue), else a coin
        // flip between the two parents' variants.
        void SpawnChildFromBreeding(Animal& partner) override;
        std::unique_ptr<Animal> CreateBaby() override;

        // MC Axolotl.onStopAttacking / applySupportingEffects — the kill
        // reward: regeneration (100 ticks, stacking to 2400) and mining-
        // fatigue removal for the player who helped.
        static void OnStopAttacking(EntityLevel& level, Axolotl& axolotl,
                                    LivingEntity& target);
        void ApplySupportingEffects(LivingEntity& player);

        // ── Client animation (MC's four BinaryAnimators) ───────────────────
        // What AxolotlRenderer.extractRenderState reads into the render
        // state's playingDeadFactor / inWaterFactor / onGroundFactor /
        // movingFactor inputs.
        float GetPlayingDeadFactor(float partialTick) const {
            return m_playingDeadAnimator.Factor(partialTick);
        }
        float GetInWaterFactor(float partialTick) const {
            return m_inWaterAnimator.Factor(partialTick);
        }
        float GetOnGroundFactor(float partialTick) const {
            return m_onGroundAnimator.Factor(partialTick);
        }
        float GetMovingFactor(float partialTick) const {
            return m_movingAnimator.Factor(partialTick);
        }

    private:
        // MC util/BinaryAnimator with EasingType.IN_OUT_SINE, length 10 — a
        // ramp that runs toward 1 while its state holds and back to 0 when it
        // does not.
        struct BinaryAnimator {
            int length = 10;
            int ticks = 0, ticksOld = 0;
            void TickAnim(bool active) {
                ticksOld = ticks;
                if (active) { if (ticks < length) ++ticks; }
                else if (ticks > 0) --ticks;
            }
            float Factor(float partialTick) const;
        };

        void TickAnimations();

        Variant m_variant = Variant::Lucy;
        bool    m_playingDead = false;

        // The partner AnimalMakeLove is breeding this axolotl with, stashed
        // for the variant coin flip — CreateBaby has no partner parameter.
        Animal* m_breedPartner = nullptr;

        BinaryAnimator m_playingDeadAnimator;
        BinaryAnimator m_inWaterAnimator;
        BinaryAnimator m_onGroundAnimator;
        BinaryAnimator m_movingAnimator;
    };

    // ── Bee ────────────────────────────────────────────────────────────────

    // MC animal/bee/Bee, promoted for its hover-roll and its ANGER: the bee
    // is a NeutralMob — a hit starts a 400..780-tick grudge, the swarm is
    // alerted (BeeHurtByOtherGoal), the isAngryAt-gated player hunt
    // (BeeBecomeAngryTargetGoal) and the anger-gated melee (BeeAttackGoal,
    // all in BeeGoals.hpp) run off it, and stinging ends it (hasStung +
    // stopBeingAngry, then the 1200-tick sting-death countdown). The hive
    // layer (pollination, flowers, crop growth — ten bespoke goals) needs
    // block entities and flower POI this engine does not have, so the class
    // keeps GenericAnimal's def-driven goal set for wandering, and adds MC's
    // rollAmount machinery: the synced `rolling` flag (flag 2 of
    // DATA_FLAGS_ID, carried on the wire's anim byte), the server-side roll
    // condition from aiStep, and the client lerp the renderer reads.
    class Bee : public GenericAnimal, public NeutralMob {
    public:
        explicit Bee(EntityLevel* level);

        // MC Bee.tick — updateRollAmount runs on both sides.
        void Tick() override;

        // MC Bee.aiStep's server half — setRolling(shouldRoll), MC's exact
        // isAngry && !hasStung && target-within-2-blocks condition.
        void AiStep() override;

        // MC Bee.doHurtTarget: the sting — poison by difficulty (10 s NORMAL /
        // 18 s HARD, none on EASY), hasStung set, stopBeingAngry. The bee
        // then dies within ~1200 ticks (the countdown in CustomServerAiStep).
        bool DoHurtTarget(Entity& target) override;

        // MC Bee.startPersistentAngerTimer — rangeOfSeconds(20, 39).
        void StartPersistentAngerTimer() override;

        void ClearReferenceTo(const Entity* entity) override {
            GenericAnimal::ClearReferenceTo(entity);
            ClearAngerReferenceTo(entity);
        }

        // MC Bee.getRollAmount — what BeeRenderer.extractRenderState reads.
        float GetRollAmount(float partialTick) const;

        bool IsRolling() const { return m_rolling; }
        bool HasStung() const { return m_hasStung; }
        // Restore path. Deliberately NOT SetAnimStateByte: that one assigns
        // all three bits at once, so using it to restore the sting would also
        // clobber the roll and nectar flags with whatever the caller guessed.
        void SetHasStung(bool v) { m_hasStung = v; }

        // The bee's pollination state lives in a shared BeeFlowerState the
        // goals also hold, so the save layer cannot reach it through this
        // class without a seam. These four are it: MC's "flower_pos",
        // "HasNectar", "TicksSincePollination" and
        // "CropsGrownSincePollination". Defined out of line because
        // BeeFlowerState is only forward-declared here.
        bool       HasNectar() const;
        void       SetHasNectar(bool v);
        bool       HasSavedFlowerPos() const;
        glm::ivec3 GetSavedFlowerPos() const;
        void       SetSavedFlowerPos(const glm::ivec3& pos);
        int        GetTicksWithoutNectar() const;
        void       SetTicksWithoutNectar(int ticks);
        int        GetCropsGrownSincePollination() const;
        void       SetCropsGrownSincePollination(int n);

        // MC packs these into DATA_FLAGS_ID (FLAG_ROLL = 2, FLAG_HAS_STUNG =
        // 4, FLAG_HAS_NECTAR = 8); here they ride the wire's one anim state
        // byte — bit 0 roll, bit 1 stung, bit 2 nectar — so the client's copy
        // drives the stinger drop and the nectar texture swap. Out-of-line:
        // the nectar bit reads the shared BeeFlowerState (BeeGoals.hpp).
        uint8_t GetAnimStateByte() const override;
        void SetAnimStateByte(uint8_t v) override;

    protected:
        // MC Bee.customServerAiStep — the underwater drown clock (20 ticks
        // submerged, then 1.0 drown damage per tick: bees are the one land
        // mob with a FASTER-than-air-supply drowning rule), the sting-death
        // countdown, and updatePersistentAnger(level, false) — note the
        // FALSE: a bee does not stay angry just because it has a target.
        void CustomServerAiStep() override;

    private:
        // MC Bee.updateRollAmount, verbatim: +0.2/tick toward 1 while
        // rolling, -0.24/tick toward 0 while not.
        void UpdateRollAmount();

        bool  m_rolling = false;
        bool  m_hasStung = false;
        int   m_timeSinceSting = 0;   // MC Bee.timeSinceSting
        int   m_underWaterTicks = 0;  // MC Bee.underWaterTicks
        float m_rollAmount = 0.0f;
        float m_rollAmountO = 0.0f;

        // Shared with the flower/crop goals (BeeGoals.hpp) — MC keeps these
        // fields on the Bee itself; the shared_ptr keeps the goals in their
        // own file without the class crossing into it.
        std::shared_ptr<struct BeeFlowerState> m_flowerState;
        // Client mirror of the nectar bit (the client has no flower state).
        bool m_clientHasNectar = false;
    };

    // ── Breeze ─────────────────────────────────────────────────────────────

    // MC monster/breeze/Breeze. Everything episodic hangs off the POSE its
    // brain sets (SHOOTING / INHALING / SLIDING / LONG_JUMPING), plus the
    // free-running idle and the slide→slideBack transition, both derived
    // client-side in tick() exactly as MC does.
    class Breeze : public GenericMonster {
    public:
        explicit Breeze(EntityLevel* level);

        void Tick() override;
        void OnPoseUpdated() override;
        void UpdateBrainActivity() override;

        // MC Breeze.canAttack — players and iron golems only.
        bool CanAttack(const LivingEntity& target) const override;

        int GetMaxHeadYRot() const override { return 30; }
        int GetHeadRotSpeed() const override { return 25; }

        // MC Breeze.getFiringYPosition — mid-body plus 0.3, where the wind
        // charge leaves the model.
        double GetFiringYPosition() const {
            return position.y + GetBbHeight() / 2.0 + 0.3;
        }

        // MC Breeze.withinInnerCircleRange — inside 4 blocks XZ / 10 Y of the
        // breeze's own block centre.
        bool WithinInnerCircleRange(const glm::dvec3& target) const;

        // MC Breeze.getHurtBy — the brain's HURT_BY_ENTITY memory.
        LivingEntity* GetHurtBy() const;

        // Fires a real BreezeWindCharge entity along MC Shoot.tick's exact
        // aim math (Projectile.spawnProjectileUsingShoot at speed 0.7).
        // Replaced the old server-side point-simulation stub when the
        // projectile subsystem landed; the brain's timings are unchanged.
        void ShootWindCharge(double xd, double yd, double zd, float inaccuracy);

    private:
        void ResetAnimations();
    };

    // ── Warden ─────────────────────────────────────────────────────────────

    // MC monster/warden/Warden minus the vibration system: there are no game
    // events or sculk sensors in this engine, so the warden cannot HEAR — it
    // angers by sniffing you out (WardenEntitySensor feeds NEAREST_ATTACKABLE,
    // TryToSniff/Sniffing raise anger on proximity), by touch, and by being
    // hit. The anger ladder, roar, sonic boom and the emerge/dig lifecycle are
    // the real MC structure on the ported brain.
    class Warden : public GenericMonster {
    public:
        explicit Warden(EntityLevel* level);

        // MC AngerLevel minimums.
        static constexpr int kAngerAgitated = 40;
        static constexpr int kAngerAngry    = 80;

        bool DoHurtTarget(Entity& target) override;
        void HandleEntityEvent(uint8_t id) override;
        void OnPoseUpdated() override;
        void Tick() override;
        void UpdateBrainActivity() override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        bool RemoveWhenFarAway(double) const override { return false; }
        void ClearReferenceTo(const Entity* entity) override;

        // MC Warden.getWalkTargetValue — flat 0: a warden does not prefer the
        // dark the way Monster's light-based default would make it.
        float GetWalkTargetValue(const glm::ivec3&) const override { return 0.0f; }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Warden.isDiggingOrEmerging — invulnerable and unpushable while
        // half inside the ground.
        bool IsDiggingOrEmerging() const;

        // MC Warden.ignoreExplosion — the same window. A warden burrowing in
        // or out takes nothing at all from a blast, where the default entity
        // takes full damage.
        bool IgnoreExplosion() const override { return IsDiggingOrEmerging(); }

        // MC Warden.canTargetEntity.
        bool CanTargetEntity(const Entity* entity) const;

        // MC Warden.setAttackTarget — also wipes the roar target and arms the
        // 200-tick sonic-boom delay, so a fresh fight opens with melee.
        void SetAttackTarget(LivingEntity* target);

        // ── AngerManagement (MC monster/warden/AngerManagement) ────────────
        // The UUID-persistence half is dropped with mob saving; the live map,
        // the 1-per-second decay and the angry>player>anger sort order are MC's.
        void IncreaseAngerAt(Entity* entity) { IncreaseAngerAt(entity, 35, true); }
        void IncreaseAngerAt(Entity* entity, int amount, bool playSound);
        void ClearAnger(const Entity* entity);
        int  GetActiveAnger() const;
        LivingEntity* GetEntityAngryAt() const;
        bool IsAngry() const { return GetActiveAnger() >= kAngerAngry; }

    private:
        void TickAngerManagement();
        void SortAnger();

        struct AngerEntry {
            Entity* entity;
            int     anger;
        };
        std::vector<AngerEntry> m_anger;   // sorted per MC AngerManagement.Sorter
    };

    // ── Creaking ───────────────────────────────────────────────────────────

    // MC monster/creaking/Creaking WITHOUT the creaking-heart block: no home
    // position, no heart protection. What that costs, explicitly:
    //   - MC's heart-bound creaking takes NO health damage (the heart absorbs
    //     hits and only a player damaging it counts). Here damage lands
    //     normally, but every hit that MC would route to the heart still
    //     flashes the 8-tick invulnerability shimmer (event 66), so the clip
    //     plays where MC plays it.
    //   - MC only tears down (45-tick twitch + crumble) a heart-bound
    //     creaking; the summoned one falls over like any monster. Here EVERY
    //     creaking tears down, because the twitch death is the mob's
    //     signature and the heart that would gate it does not exist.
    // The freeze-when-observed gate (checkCanMove / isLookingAtMe) is MC's.
    class Creaking : public GenericMonster {
    public:
        explicit Creaking(EntityLevel* level);

        bool CanMove() const { return m_canMove; }
        bool IsActive() const { return m_isActive; }
        bool IsTearingDown() const { return m_tearingDown; }

        // MC's CAN_MOVE / IS_ACTIVE / IS_TEARING_DOWN synched booleans, packed
        // into the one anim-state byte. Meaning private to this class.
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_canMove ? 1 : 0)
                                        | (m_isActive ? 2 : 0)
                                        | (m_tearingDown ? 4 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_canMove     = (v & 1) != 0;
            m_isActive    = (v & 2) != 0;
            m_tearingDown = (v & 4) != 0;
        }

        bool DoHurtTarget(Entity& target) override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void HandleEntityEvent(uint8_t id) override;
        void Tick() override;
        void AiStep() override;
        void SetupAnimationStates() override;
        void UpdateBrainActivity() override;
        void Die(MobDamageSource source, Entity* attacker) override;
        void TickDeath() override;

        // MC gates the body-rotation control on canMove, so a frozen creaking
        // does not even settle its torso.
        void TickHeadTurn(float yBodyRotTarget) override;

        // MC Creaking.knockback — an unseen creaking cannot be shoved.
        void Knockback(double power, double dx, double dz) override;

        // MC Creaking.activate / deactivate.
        void Activate(LivingEntity* player);
        void Deactivate();

        // MC Creaking.getWalkTargetValue — flat 0, like the warden.
        float GetWalkTargetValue(const glm::ivec3&) const override { return 0.0f; }

    protected:
        void UpdateWalkAnimation(float distance) override;

    private:
        // MC Creaking.checkCanMove — the freeze rule. True = free to move.
        bool CheckCanMove();
        // MC LivingEntity.isLookingAtMe(tolerance 0.5, three body heights).
        bool IsLookingAtMe(const LivingEntity& player) const;

        bool m_canMove = true;
        bool m_isActive = false;
        bool m_tearingDown = false;

        // MC Creaking.attackAnimationRemainingTicks. Counted on both sides: the
        // server sets it in doHurtTarget, the client from entity event 4.
        int m_attackAnimationRemainingTicks = 0;
        // MC Creaking.invulnerabilityAnimationRemainingTicks — event 66.
        int m_invulnerabilityAnimationRemainingTicks = 0;
    };

    // ── Sniffer ────────────────────────────────────────────────────────────

    // MC animal/sniffer/Sniffer. One synched State enum drives all five clips;
    // the state machine (scent → sniff → search → dig → rise → happy) is
    // SnifferAi's, ported whole.
    class Sniffer : public GenericAnimal {
    public:
        // MC Sniffer.State — the ids are MC's and they are the wire encoding
        // of the animation-state byte.
        enum class State : uint8_t {
            Idling = 0, FeelingHappy = 1, Scenting = 2, Sniffing = 3,
            Searching = 4, Digging = 5, Rising = 6,
        };

        explicit Sniffer(EntityLevel* level);

        State GetState() const { return m_state; }

        // MC Sniffer.transitionTo — the ONLY writer of the state, so the
        // per-state entry sounds have one home when sounds exist.
        Sniffer& TransitionTo(State state);

        bool IsTempted() const;
        // MC Sniffer.canSniff / canDig.
        bool CanSniff() const;
        bool CanDig() const;

        // MC Sniffer.calculateDigPosition — five LandRandomPos rolls of
        // widening radius, first diggable hit wins.
        std::optional<glm::ivec3> CalculateDigPosition();
        // MC Sniffer.onDiggingComplete — remembers the dug column so the next
        // sniff avoids it.
        void OnDiggingComplete(bool success);

        uint8_t GetAnimStateByte() const override { return static_cast<uint8_t>(m_state); }
        void    SetAnimStateByte(uint8_t v) override;

        void Tick() override;
        void UpdateBrainActivity() override;
        bool IsFood(uint32_t itemId) const override;
        void Die(MobDamageSource source, Entity* attacker) override;

        // MC ItemTags.SNIFFER_FOOD — torchflower seeds.
        static bool IsSnifferFood(uint32_t itemId);

    private:
        glm::ivec3 GetHeadBlock() const;
        bool CanDigAt(const glm::ivec3& pos) const;
        void ResetAnimations();
        void DropSeed();

        State m_state = State::Idling;

        // MC's DATA_DROP_SEED_AT_TICK — server-only here; it is synched in MC
        // only because dropSeed once ran client-side.
        int m_dropSeedAtTick = -1;

        // MC's SNIFFER_EXPLORED_POSITIONS brain memory (List<GlobalPos>). The
        // memory variant has no BlockPos-list alternative and this is its only
        // reader, so the list lives on the class; 20-entry cap is MC's.
        std::vector<glm::ivec3> m_exploredPositions;
    };

    // ── Armadillo ──────────────────────────────────────────────────────────

    class Armadillo : public GenericAnimal {
    public:
        // MC Armadillo.ArmadilloState. The ids are MC's and they are the wire
        // encoding of the animation-state byte.
        enum class State : uint8_t {
            Idle = 0, Rolling = 1, Scared = 2, Unrolling = 3,
        };

        static bool  IsThreatened(State s) { return s != State::Idle; }
        static int   AnimationDuration(State s);
        static bool  ShouldHideInShell(State s, int64_t ticksInState);

        explicit Armadillo(EntityLevel* level);

        State GetState() const { return m_state; }
        void  SwitchToState(State s);

        bool IsScared() const { return m_state != State::Idle; }
        bool ShouldHideInShell() const { return ShouldHideInShell(m_state, m_inStateTicks); }
        bool ShouldSwitchToScaredState() const {
            return m_state == State::Rolling
                && m_inStateTicks > AnimationDuration(State::Rolling);
        }

        // MC Armadillo.canStayRolledUp. Leashes, riding and being ridden do not
        // exist here, so this is the two conditions that do.
        bool CanStayRolledUp() const { return !IsPanicking() && !IsInLiquid(); }

        // MC Armadillo.isScaredBy — what makes an armadillo curl up.
        bool IsScaredBy(const LivingEntity& other) const;

        void RollUp();
        void RollOut();

        // MC's DANGER_DETECTED_RECENTLY memory — ArmadilloBallUp reads the
        // REMAINING time (brain getTimeUntilExpiry) to decide when to unroll.
        int64_t DangerTicksRemaining() const;
        bool    DangerDetected() const { return DangerTicksRemaining() > 0; }

        void UpdateBrainActivity() override;

        uint8_t GetAnimStateByte() const override { return static_cast<uint8_t>(m_state); }
        void    SetAnimStateByte(uint8_t v) override {
            SwitchToState(v <= 3 ? static_cast<State>(v) : State::Idle);
        }

        void Tick() override;
        void SetupAnimationStates() override;
        void HandleEntityEvent(uint8_t id) override;

        // MC Armadillo.hurtServer halves the damage (minus one) while rolled up.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

    private:
        State   m_state = State::Idle;
        int64_t m_inStateTicks = 0;

        // MC's client-only `peekReceivedClient`, set by entity event 64. It is
        // what stops the peek animation restarting every tick after the server
        // asks for one.
        bool m_peekReceivedClient = false;
    };

    // ── CopperGolem ────────────────────────────────────────────────────────

    // MC animal/golem/CopperGolem on the ported CopperGolemAi brain — the
    // chest courier: it walks chest to chest, faces one for a 60-tick
    // interaction, and ferries up to 16 items of one stack per trip (empty
    // hand = collecting, full hand = delivering). One synched CopperGolemState
    // picks the four interaction clips at tick 1 of that window; IDLE re-arms
    // the 200-240-tick head-spin between trips. Skipped MC systems, each
    // named at its slot in the .cpp: the weathering/oxidation ladder (and the
    // oxidized-statue conversion), lightning de-oxidation and honeycomb
    // waxing, the antenna equipment slot + shearing, and sounds.
    class CopperGolem : public GenericPathfinderMob {
    public:
        // MC CopperGolemState. The ids are MC's and they are the wire encoding
        // of the animation-state byte's low three bits.
        enum class State : uint8_t {
            Idle = 0, GettingItem = 1, GettingNoItem = 2,
            DroppingItem = 3, DroppingNoItem = 4,
        };

        // MC CopperGolem.SPIN_ANIMATION_{MIN,MAX}_COOLDOWN — the idle
        // head-spin re-arm window.
        static constexpr int kSpinAnimationMinCooldown = 200;
        static constexpr int kSpinAnimationMaxCooldown = 240;
        // MC CopperGolem.SPAWN_COOLDOWN_{MIN,MAX} — the first transport trip
        // waits this long after spawn.
        static constexpr int kSpawnCooldownMin = 60;
        static constexpr int kSpawnCooldownMax = 100;

        explicit CopperGolem(EntityLevel* level);

        State GetState() const { return m_state; }
        // MC CopperGolem.setState — a synched accessor in MC; here the tracker
        // polls the anim byte, so a plain write is the whole job.
        void SetState(State state) { m_state = state; }

        // MC CopperGolem.getMainHandItem / setItemInHand(MAIN_HAND, …) — the
        // one stack it carries between chests. No equipment system exists, so
        // the stack lives on the class and the wire carries only the holding
        // BIT (bit 3 of the anim byte), which is all the WALK_ITEM clip gate
        // reads.
        const ItemStack& GetMainHandItem() const { return m_handItem; }
        void SetItemInHand(const ItemStack& stack) { m_handItem = stack; }

        // What MobRenderer reads into state.isHoldingItem. The server side
        // has the real stack; the client has the synched bit. Out-of-line:
        // EntityLevel is incomplete here.
        bool IsHoldingItem() const;

        // MC CopperGolem.setOpenedChestPos / clearOpenedChestPos. MC's only
        // reader is hasContainerOpen, ContainerOpenersCounter's callback —
        // which waits on the chest lid counter — so nothing consumes it yet;
        // kept so CopperGolemAi transcribes verbatim.
        void SetOpenedChestPos(const glm::ivec3& pos) { m_openedChestPos = pos; }
        void ClearOpenedChestPos() { m_openedChestPos.reset(); }

        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;

        // MC CopperGolem.setupAnimationStates — the whole client clip machine.
        void SetupAnimationStates() override;

        // MC CopperGolem.customServerAiStep's second half.
        void UpdateBrainActivity() override;

        // MC CopperGolem.mobInteract's empty-hand branch: take what the golem
        // carries. (The shears, honeycomb and axe branches are skipped — no
        // antenna equipment slot, no weathering.)
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC CopperGolem.dropEquipment → dropPreservedEquipment: the carried
        // stack drops on death (setGuaranteedDrop's whole point).
        void Die(MobDamageSource source, Entity* attacker) override;

    protected:
        // MC CopperGolem.actuallyHurt — a hit knocks it out of the chest
        // interaction pose.
        void ActuallyHurt(MobDamageSource source, float amount,
                          Entity* attacker) override;

    private:
        State     m_state = State::Idle;
        ItemStack m_handItem;
        // Client mirror of the holding bit (the client has no hand stack).
        bool      m_clientHoldingItem = false;
        // MC CopperGolem.openedChestPos.
        std::optional<glm::ivec3> m_openedChestPos;
        // MC CopperGolem.idleAnimationStartTick — client-only, like the timer
        // it arms.
        int m_idleAnimationStartTick = 0;
    };

    // ── Allay ──────────────────────────────────────────────────────────────

    // MC animal/allay/Allay on the ported AllayAi brain — the float/panic/
    // flying-wander core. The rest of MC's class is the item courier
    // (pick up a matching item, ferry stacks to the liked player or liked
    // noteblock), the jukebox dance + duplication ritual, and the vibration
    // listener — all riding the item, jukebox-event and vibration systems,
    // all skipped and named in AllayAi.cpp.
    class Allay : public GenericPathfinderMob {
    public:
        explicit Allay(EntityLevel* level);
        void UpdateBrainActivity() override;

        // MC Allay.removeWhenFarAway (Allay.java:384-386) — false: an allay
        // never distance-despawns. Mob's base default is true, which was
        // silently despawning them.
        bool RemoveWhenFarAway(double) const override { return false; }
    };

    // ── HappyGhast ─────────────────────────────────────────────────────────

    // MC animal/ghast/HappyGhast, with MC's real AI split: the ADULT is
    // goal-driven (this port keeps the def-driven set — which carries the
    // flying wander — plus HappyGhastFloatGoal at MC's priority 3), and the
    // BABY ghastling runs HappyGhastAi's brain (tempt-follow, trailing
    // players and followable adults, flying wander, panic). The class swaps
    // setups at the age boundary as MC's adultGhastSetup/babyGhastSetup do.
    // Harness riding, the still timeout and body armor ride the riding/
    // equipment systems and are named skipped here.
    class HappyGhast : public GenericAnimal {
    public:
        explicit HappyGhast(EntityLevel* level);

        void UpdateBrainActivity() override;

        // MC HappyGhast.getMaxSpawnClusterSize (HappyGhast.java:201-203).
        int GetMaxSpawnClusterSize() const override { return 1; }

        // MC HappyGhast.isOnStillTimeout — only the riding system sets it.
        bool IsOnStillTimeout() const { return false; }

        std::unique_ptr<Animal> CreateBaby() override {
            return std::make_unique<HappyGhast>(m_level);
        }

    protected:
        // MC HappyGhast.ageBoundaryReached, detected by polling — the port's
        // AgeableMob has no boundary hook.
        void CustomServerAiStep() override;

    private:
        void AdultSetup();
        void BabySetup();
        void RegisterAdultGoals();

        bool m_wasBaby = false;
    };

} // namespace Game
