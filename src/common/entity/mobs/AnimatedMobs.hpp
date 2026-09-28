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

#include "common/world/level/gameevent/VibrationSystem.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/NeutralMob.hpp"
#include "common/core/Uuid.hpp"
#include "common/inventory/SimpleContainer.hpp"
#include "common/entity/MobCrossbow.hpp"
#include "common/entity/PlayerRideableJumping.hpp"
#include "common/entity/MountInventory.hpp"
#include "common/sound/SoundEvents.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <vector>

namespace Game {

    struct BucketEntityData;

    // ── Frog ───────────────────────────────────────────────────────────────

    class Frog : public GenericAnimal {
    public:
        // MC Frog.isPushedByFluid: false.
        bool IsPushedByFluid() const override { return false; }
        // MC Frog.playEatingSound: FROG_EAT bound to the frog at 2.0.
        void PlayEatingSound() override;

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

    // MC Camel extends AbstractHorse; the engine's AbstractHorse is the
    // horse family's own class, so the equine half a camel uses lives here:
    // the saddle rule for steering, the ridden input/rotation, the dash as
    // its PlayerRideableJumping, grazing, the fall rule, the two seats.
    class Camel : public GenericAnimal, public PlayerRideableJumping {
    public:
        // MC's two pose-transition lengths, in ticks. A camel is "in
        // transition" until the relevant one has elapsed, and the sit-down and
        // stand-up clips are exactly these long.
        static constexpr int kSitDownDuration = 40;
        static constexpr int kStandUpDuration = 52;

        // `type` so a camel husk keeps its own EntityTypeId (and therefore its
        // own texture, attributes and loot) while sharing MC's Camel class.
        explicit Camel(EntityLevel* level, EntityTypeId type = EntityTypeId::Camel);

        // MC Camel.playStepSound: a padded step on #camel_sand_step_sound_blocks
        // (sand, concrete powder), the ordinary one elsewhere — 1.0 for the
        // camel, 0.4 for the husk (CamelHusk.playStepSound).
        void PlayStepSound(const glm::ivec3& pos, BlockState state) override;
        // MC Camel.playEatingSound (via Animal feeding).
        void PlayEatingSound() override;
        bool IsHusk() const { return GetType() == EntityTypeId::CamelHusk; }

        // ── Dimensions (MC Camel.getDefaultDimensions) ─────────────────────
        // SITTING: the adult box less SITTING_HEIGHT_DIFFERENCE (1.43), eye
        // 0.845; the baby's BABY_SITTING_DIMENSIONS 0.95 x 0.425, eye 0.41.
        // Standing keeps the type's box.
        static constexpr float kSittingHeightDifference = 1.43f;
        float BaseBbWidth() const override;
        float BaseBbHeight() const override;
        float BaseEyeHeight() const override;
        // MC Camel.getAgeScale: 0.6 for a calf.
        float GetCamelAgeScale() const { return IsBaby() ? 0.6f : 1.0f; }
        // MC Camel.getBodyAnchorAnimationYOffset — the body's height through
        // the sit / stand clips, which the seats and the lead follow.
        // `dimensionsHeight` is getDimensions(getPose()).height(), `scale`
        // the age scale (times the SCALE attribute).
        double GetBodyAnchorAnimationYOffset(bool isFront, float partialTicks, float dimensionsHeight,
                                             float scale) const;

        // ── The camel husk (MC CamelHusk) ─────────────────────────────────
        // canBeABaby false: never a calf, no age saved, no love.
        bool IsBaby() const override { return !IsHusk() && GenericAnimal::IsBaby(); }
        void SetBaby(bool baby) override { if (!IsHusk()) GenericAnimal::SetBaby(baby); }
        bool CanFallInLove() const override { return !IsHusk() && GenericAnimal::CanFallInLove(); }
        // MC AbstractHorse.isMobControlled (false) / CamelHusk's: a mob (the
        // husk jockey) in the first seat.
        bool IsMobControlled() const;

        // ── Breeding / feeding (MC Camel.canMate, handleEating) ───────────
        // Both parents canParent: not ridden, not riding, adult, at full
        // health and in love (a camel is always tamed). The husk never mates.
        bool CanMate(const Animal& other) const override;
        bool CanParent() const;
        // MC Camel.mobInteract: sneak → the mount inventory; the held item's
        // own interaction (a saddle); food; else climb on (two seats).
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // MC AbstractHorse.fedFood with Camel.handleEating: heal 2, love (an
        // adult at age 0), a calf ages 10 s — the eat sound when any took.
        UseResult FedFood(LivingEntity& player, ItemStack& held);
        bool HandleEating(LivingEntity& player, const ItemStack& held);

        // ── Equipment and inventory (MC AbstractHorse as a camel inherits
        //    it — isTamed is always true; the system is MountInventory's) ────
        // canUseSlot: the saddle on a live, grown camel.
        bool CanUseSlot(EquipmentSlot slot) const override {
            if (slot != EquipmentSlot::SADDLE) return GenericAnimal::CanUseSlot(slot);
            return IsAlive() && !IsBaby();
        }
        // canDispenserEquipIntoSlot: the body or saddle of a (tamed) camel.
        bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const override {
            return slot == EquipmentSlot::BODY || slot == EquipmentSlot::SADDLE ||
                   GenericAnimal::CanDispenserEquipIntoSlot(slot);
        }
        // MC Camel.getEquipSound: getSaddleSound() — CAMEL_SADDLE, the husk's
        // CAMEL_HUSK_SADDLE.
        std::string GetEquipSound(EquipmentSlot slot, const ItemStack& stack,
                                  const Equippable& equippable) const override {
            if (slot != EquipmentSlot::SADDLE) return GenericAnimal::GetEquipSound(slot, stack, equippable);
            return IsHusk() ? SoundEvents::CAMEL_HUSK_SADDLE : SoundEvents::CAMEL_SADDLE;
        }
        MountInventory*       GetMountInventory() override       { return &m_mountInventory; }
        const MountInventory* GetMountInventory() const override { return &m_mountInventory; }
        bool HasCustomInventoryScreen() const override { return true; }
        // MC Camel.openCustomInventoryScreen: server side, always (a camel is
        // tamed, and its second seat does not close the screen).
        void OpenCustomInventoryScreen(LivingEntity& player) override;
        // MC AbstractHorse.finalizeSpawn → AgeableMob's with
        // AgeableMobGroupData(0.2): later herd members are calves 20 % of the
        // time; Camel.finalizeSpawn stands it up fully first.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;
        // MC AbstractHorse.getMaxSpawnClusterSize.
        int GetMaxSpawnClusterSize() const override { return 6; }

        // ── MC AbstractHorse, as the camel inherits it ────────────────────
        // isPushable: not while carrying anyone.
        bool IsPushable() const override { return !IsVehicle(); }
        // isImmobile: (dead && ridden && saddled) || eating (a camel cannot
        // rear, so never standing).
        bool IsImmobile() const override {
            return (GenericAnimal::IsImmobile() && IsVehicle() && IsSaddled()) || m_eating;
        }
        // The EATING flag — canEatGrass is true for a camel too: on a grass
        // block an unridden camel grazes, planted, for 50 ticks.
        bool IsEating() const { return m_eating; }
        void SetEating(bool v) { m_eating = v; }
        // aiStep: the 1-in-900 self-heal and the grazing roll (server).
        void AiStep() override;
        // causeFallDamage: HORSE_LAND past one block, then hurt + the block
        // fall sound.
        bool CauseFallDamage(double fallDist, float damageMultiplier) override;
        // MC Camel.getMaxHeadYRot: 30.
        int GetMaxHeadYRot() const override { return 30; }
        // MC Camel.canCamelChangePose — the box of the other pose is free.
        bool CanCamelChangePose() const;
        // MC PathfinderMob.isPanicking for a brain mob: IS_PANICKING.
        bool IsCamelPanicking() const;
        // MC Mob.hasControllingPassenger (the steering player; server).
        bool HasControllingPassenger() const { return GetControllingPassenger() != nullptr; }

        // ── Riding (MC Camel / AbstractHorse riding half) ─────────────────
        // getControllingPassenger: a player in the first seat of a saddled
        // camel.
        bool CanBeSteeredBy(const RiderControl& rider) const override;
        glm::dvec3 GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) override;
        void  TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) override;
        float GetRiddenSpeed(const RiderControl& rider) const override;
        // A sitting camel's feet are planted (Camel.travel).
        void Travel(const glm::dvec3& input) override;
        // MC Camel.getPassengerAttachmentPoint: the driver 0.5 forward, the
        // second rider 0.7 back (an animal rider 0.2 further forward), at
        // the body's animated height.
        glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const override;
        glm::dvec3 GetPassengerAttachmentForSlot(int slot, int total) const override;
        // MC Camel.canAddPassenger: `getPassengers().size() <= 2`.
        bool CanAddPassenger(const Entity& passenger) const override {
            (void)passenger;
            return GetPassengers().size() <= 2;
        }
        // MC AbstractHorse.positionRider: a living rider's body turns with
        // the camel.
        void PositionRider(Entity& passenger) override;
        // MC AbstractHorse.getDismountLocationForPassenger.
        glm::dvec3 GetDismountLocationForPassenger(const LivingEntity& passenger) const override;

        // ── PlayerRideableJumping — the dash ──────────────────────────────
        // canJump: saddled and willing to move.
        bool CanJump() const override;
        // onPlayerJump: saddled, off cooldown, on the ground → the charge
        // (getPlayerJumpPendingScale), spent by the next tickRidden.
        void OnPlayerJump(int jumpAmount) override;
        // handleStartJump (server): the dash sound, ENTITY_ACTION, DASH on.
        void HandleStartJump(int jumpScale) override;
        void HandleStopJump() override {}

        // ── Sounds (MC Camel / CamelHusk) ─────────────────────────────────
        const char* GetDashingSound() const { return IsHusk() ? SoundEvents::CAMEL_HUSK_DASH : SoundEvents::CAMEL_DASH; }
        const char* GetDashReadySound() const {
            return IsHusk() ? SoundEvents::CAMEL_HUSK_DASH_READY : SoundEvents::CAMEL_DASH_READY;
        }
        const char* GetSitDownSound() const { return IsHusk() ? SoundEvents::CAMEL_HUSK_SIT : SoundEvents::CAMEL_SIT; }
        const char* GetStandUpSound() const { return IsHusk() ? SoundEvents::CAMEL_HUSK_STAND : SoundEvents::CAMEL_STAND; }
        const char* GetEatingSound() const { return IsHusk() ? SoundEvents::CAMEL_HUSK_EAT : SoundEvents::CAMEL_EAT; }
        // MC Camel.getSaddleSound — the SADDLE slot's equip sound.
        const char* GetSaddleSound() const { return IsHusk() ? SoundEvents::CAMEL_HUSK_SADDLE : SoundEvents::CAMEL_SADDLE; }

        // MC Camel.isTamed: always true — a camel needs no taming (it is
        // ridden as found; its AbstractHorse temper is never used), so it is
        // a tamed pet for Mob::IsTamedPet. The camel husk (a CamelHusk is a
        // Camel in MC too) is the husk's despawning mount — its explicit
        // removeWhenFarAway true wins, so it is left out.
        bool IsTamedPet() const override { return !IsHusk(); }

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

        // MC Camel.DASH — a synched boolean, raised by executeRidersJump on
        // the steering client and by handleStartJump on the server; it rides
        // the anim byte. MC's onSyncedDataUpdated arms the 55-tick cooldown
        // on every change of the flag, on both sides (SetDashing does).
        static constexpr int kDashCooldownTicks = 55;

        bool IsDashing() const { return m_dashing; }
        void SetDashing(bool v);

        // MC Camel.getJumpCooldown — the dash cooldown, read by the HUD's
        // jump bar and CamelRenderer.extractRenderState.
        int GetJumpCooldown() const override { return m_dashCooldown; }

        uint8_t GetAnimStateByte() const override { return m_dashing ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override;

    protected:
        // MC Camel.updateWalkAnimation: the walk cycle only while standing
        // and not dashing.
        void UpdateWalkAnimation(float distance) override;
        // MC Camel.actuallyHurt: a hurt camel is on its feet at once.
        void ActuallyHurt(MobDamageSource source, float amount, Entity* attacker) override;

    private:
        void ResetLastPoseChangeTick(int64_t syncedPoseTickTime);
        // MC Camel.executeRidersJump — the dash itself (steering side).
        void ExecuteRidersJump(float amount);
        // MC LivingEntity.getBlockSpeedFactor.
        float GetBlockSpeedFactor() const;

        // MC AbstractHorse.playerJumpPendingScale / eating / eatingCounter.
        float m_playerJumpPendingScale = 0.0f;
        bool  m_eating = false;
        int   m_eatingCounter = 0;

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

        // MC AbstractHorse.inventory (no chest: getInventoryColumns 0).
        MountInventory m_mountInventory;

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

        // MC Bat.getAmbientSound: a hanging bat squeaks a quarter as often.
        const char* GetAmbientSound() const override;

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
        // MC AbstractFish → WaterAnimal.isPushedByFluid: false.
        bool IsPushedByFluid() const override { return false; }
        // MC WaterAnimal.checkSpawnObstruction: level.isUnobstructed(this) only — the
        // base's no-liquid half would refuse every underwater spawn.
        bool CheckSpawnObstruction(EntityLevel& level) const override { return IsUnobstructed(level); }

    public:
        explicit Tadpole(EntityLevel* level);
        void UpdateBrainActivity() override;

        // MC Tadpole is an AbstractFish (WaterAnimal): out of water its air
        // drains and it suffocates — see HandleWaterAnimalAirSupply.
        void BaseTick() override;

        // ── Growing up (MC Tadpole.age / AGE_LOCKED) ──────────────────────
        // MC Tadpole.ticksToBeFrog: 24000 ticks of age make a frog.
        static constexpr int kTicksToBeFrog = 24000;
        // MC aiStep: the server counts age up unless locked; the age-lock
        // burst draws on the client.
        void AiStep() override;
        int  GetAge() const { return m_age; }
        // MC setAge: at kTicksToBeFrog the tadpole becomes a frog.
        void SetAge(int age);
        // MC AGE_LOCKED (synched, saved "AgeLocked"): a golden dandelion
        // stops the growth for good.
        bool IsAgeLocked() const override { return m_ageLocked; }
        void SetAgeLocked(bool locked) override { m_ageLocked = locked; }
        // The client's lock/unlock burst when the synched flag flips (the
        // engine's form of MC's server-sent PAUSE/RESET_MOB_GROWTH).
        void ArmAgeLockParticles() { m_ageLockParticleTimer = 40; }

        // MC Tadpole.mobInteract: slime ball (#frog_food) feeds, a golden
        // dandelion toggles the lock, a water bucket scoops it.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // ── MC Bucketable (Tadpole extends AbstractFish) ───────────────────
        // fromBucket() is always true — every tadpole is kept:
        // requiresCustomPersistence = super || fromBucket = true, and
        // removeWhenFarAway = !fromBucket && … = false.
        bool FromBucket() const { return true; }
        bool RequiresCustomPersistence() const override { return true; }
        bool RemoveWhenFarAway(double) const override { return false; }
        // MC saveToBucketTag: the default keys + Age + AgeLocked.
        void SaveToBucket(ItemStack& bucket) const;
        // MC loadFromBucketTag.
        void LoadFromBucket(const BucketEntityData& data);

        // MC Tadpole.shouldDropExperience: false.
        int GetXpReward() const override { return 0; }

    private:
        // MC ageUp(): become a frog (convertTo FROG, finalizeSpawn
        // CONVERSION, persistent, the grow-up sound). Server only.
        void BecomeFrog();

        int  m_age = 0;
        bool m_ageLocked = false;
        int  m_ageLockParticleTimer = 0;   // MC ageLockParticleTimer
    };

    class Goat : public GenericAnimal {
    public:
        explicit Goat(EntityLevel* level);
        // MC Goat.playEatingSound — bound to the goat, 0.8..1.2 pitch. (No
        // screaming goats here, so never the GOAT_SCREAMING_* set.)
        void PlayEatingSound() override;
        void UpdateBrainActivity() override;
    };

    // Animal, not Monster: MC's Hoglin extends Animal and merely implements
    // Enemy, so it breeds and takes the animal attribute set.
    class Hoglin : public GenericAnimal {
    public:
        explicit Hoglin(EntityLevel* level);
        void UpdateBrainActivity() override;
        // MC Hoglin.getAmbientSound → HoglinAi.getSoundForCurrentActivity
        // (server only): RETREAT while avoiding or converting, ANGRY while
        // fighting, RETREAT near a repellent, else AMBIENT.
        const char* GetAmbientSound() const override;

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

        // ── Zombification (MC Hoglin) ──────────────────────────────────────
        // DATA_IMMUNE_TO_ZOMBIFICATION (synced — bit 0 of the anim byte, so
        // the client's conversion shake leaves an immune hoglin still),
        // timeInOverworld and the CONVERSION_TIME of 300 ticks: outside the
        // Nether (PIGLINS_ZOMBIFY) a hoglin shakes for 15 seconds and turns
        // into a zoglin with 10 s of nausea.
        static constexpr int kConversionTime = 300;   // MC CONVERSION_TIME
        bool IsImmuneToZombification() const { return m_immuneToZombification; }
        void SetImmuneToZombification(bool v) { m_immuneToZombification = v; }
        int  GetTimeInOverworld() const { return m_timeInOverworld; }
        void SetTimeInOverworld(int ticks) { m_timeInOverworld = ticks; }
        // MC isConverting: not immune, AI on, and PIGLINS_ZOMBIFY (every
        // dimension but the Nether). Both sides — the renderer shakes it.
        bool IsConverting() const;
        uint8_t GetAnimStateByte() const override { return m_immuneToZombification ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_immuneToZombification = (v & 1) != 0; }

        // MC cannotBeHunted ("CannotBeHunted", NBT-only) and canBeHunted: an
        // adult piglins may hunt.
        bool CannotBeHunted() const { return m_cannotBeHunted; }
        void SetCannotBeHunted(bool v) { m_cannotBeHunted = v; }
        bool CanBeHunted() const { return !IsBaby() && !m_cannotBeHunted; }

        // MC Hoglin.finalizeSpawn: 20% a baby (PROBABILITY_OF_SPAWNING_AS_BABY).
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;
        // MC Hoglin.hurtServer → HoglinAi.wasHurtBy.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // MC Hoglin.getWalkTargetValue: -1 near the remembered repellent, 10
        // over crimson nylium, else 0.
        float GetWalkTargetValue(const glm::ivec3& pos) const override;
        // MC Hoglin.canFallInLove: not while pacified.
        bool CanFallInLove() const override;
        // MC Hoglin.getBreedOffspring: the piglet is persistent.
        std::unique_ptr<Animal> CreateBaby() override;
        // MC Hoglin.mobInteract: an interaction that consumed the action
        // (feeding) makes the hoglin persistent.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

    protected:
        // MC Hoglin.customServerAiStep: the zombification clock.
        void CustomServerAiStep() override;

    private:
        // MC Hoglin.ageBoundaryReached: ATTACK_DAMAGE 0.5 as a baby, 6 grown.
        void SyncAgeBoundary();
        void FinishConversion();

        int  m_attackAnimationRemainingTicks = 0;
        bool m_immuneToZombification = false;
        bool m_cannotBeHunted = false;
        int  m_timeInOverworld = 0;
        int  m_lastBabyState = -1;   // the age boundary last applied (-1 none)
    };

    // MC monster/Zoglin — the zombified hoglin, on MC's own small Brain
    // (idle / fight, ZoglinAi here), plus the headbutt animation machinery MC
    // keeps on the entity itself: doHurtTarget arms
    // attackAnimationRemainingTicks + entity event 4, exactly like the hoglin
    // it shares HoglinBase with in MC.
    class Zoglin : public GenericMonster {
    public:
        explicit Zoglin(EntityLevel* level);
        // MC Zoglin.getAmbientSound: ANGRY with a target, else AMBIENT
        // (server only).
        const char* GetAmbientSound() const override;

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

    // MC monster/piglin/Piglin on the ported PiglinAi brain, with the item
    // half: the spawn weapon (crossbow or golden sword / 1-in-10 golden
    // spear) and the 10%-a-piece gold armour, the 8-slot pocket inventory
    // ("Inventory"), loot pickup through PiglinAi (admire a loved item in the
    // off hand, barter a gold ingot for piglin_bartering loot, eat, equip
    // better gear, pocket the rest), the crossbow (CrossbowAttackMob — the
    // brain's CrossbowAttack), and the overworld zombification clock (the
    // Nether does not zombify — PIGLINS_ZOMBIFY).
    class Piglin : public GenericMonster, public CrossbowAttackMob {
    public:
        static constexpr int kInventorySize = 8;   // MC Piglin.INVENTORY_SIZE

        explicit Piglin(EntityLevel* level);
        // MC Piglin.getAmbientSound → PiglinAi.getSoundForCurrentActivity.
        const char* GetAmbientSound() const override;

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
        // MC AbstractPiglin.isConverting: not immune, AI on, and the level's
        // PIGLINS_ZOMBIFY (every dimension but the Nether).
        bool IsConverting() const;

        // Bit 0 DATA_IS_DANCING, bit 1 DATA_IS_CHARGING_CROSSBOW, bit 2
        // AbstractPiglin's DATA_IMMUNE_TO_ZOMBIFICATION (the client's
        // isConverting — the conversion shake).
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_dancing ? 1 : 0) | (m_chargingCrossbow ? 2 : 0) |
                                        (m_immuneToZombification ? 4 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_dancing = (v & 1) != 0;
            m_chargingCrossbow = (v & 2) != 0;
            m_immuneToZombification = (v & 4) != 0;
        }

        // MC AbstractPiglin.playAmbientSound: only while IDLE.
        void PlayAmbientSound() override;

        // MC Piglin.finalizeSpawn — outside a structure 20% baby, else an
        // adult gets its spawn weapon; initMemories, the gold armour, the
        // spawn enchantments, then Mob's.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Piglin.hurtServer → PiglinAi.wasHurtBy.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Piglin.mobInteract → PiglinAi.mobInteract: an adult that is not
        // admiring takes one gold ingot into its off hand to admire.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // ── CrossbowAttackMob ──────────────────────────────────────────────
        void SetChargingCrossbow(bool charging) override { m_chargingCrossbow = charging; }
        bool IsChargingCrossbow() const override { return m_chargingCrossbow; }
        void OnCrossbowAttackPerformed() override { ResetNoActionTime(); }
        // MC performRangedAttack → performCrossbowAttack(this, 1.6), aimed at
        // the brain's ATTACK_TARGET (AbstractPiglin.getTarget).
        void PerformRangedAttack(LivingEntity& target, float power) override;
        // MC Piglin.canUseNonMeleeWeapon: the crossbow and a KINETIC_WEAPON
        // spear.
        bool CanUseNonMeleeWeapon(const ItemStack& stack) const override;
        // The client's crossbow-draw clock (see Pillager).
        int GetClientChargeTicks() const override { return m_clientChargeTicks; }

        // ── Items (MC Piglin / PiglinAi) ───────────────────────────────────
        // MC Piglin.getPreferredWeaponType: piglin_preferred_weapons for an
        // adult, none for a baby.
        const char* GetPreferredWeaponType() const override {
            return IsBaby() ? nullptr : "minecraft:piglin_preferred_weapons";
        }
        // MC Piglin.wantsToPickUp: mobGriefing, canPickUpLoot and
        // PiglinAi.wantsToPickup.
        bool WantsToPickUp(const ItemStack& stack) const override;
        // MC Piglin.canReplaceCurrentItem: Curse of Binding pins the piece; a
        // loved or preferred item beats one that is neither, and loses to
        // one; otherwise Mob's comparison.
        bool CanReplaceCurrentItem(const ItemStack& newStack, const ItemStack& current,
                                   EquipmentSlot slot) const override;
        // MC Piglin.canReplaceCurrentItem(newItem) — against the slot the
        // item would go in.
        bool CanReplaceCurrentItem(const ItemStack& newStack) const;
        // MC Piglin.pickUpItem → onItemPickup + PiglinAi.pickUpItem.
        void PickUpItem(int32_t itemEntityId, const ItemStack& stack) override;
        // MC Piglin.holdInMainHand / holdInOffHand.
        void HoldInMainHand(const ItemStack& stack);
        void HoldInOffHand(const ItemStack& stack);
        // MC Piglin.addToInventory / canAddToInventory.
        ItemStack AddToInventory(const ItemStack& stack);
        bool CanAddToInventory(const ItemStack& stack) const;
        SimpleContainer&       GetInventory()       { return m_inventory; }
        const SimpleContainer& GetInventory() const { return m_inventory; }
        // MC Piglin.dropCustomDeathLoot: the pockets empty onto the ground
        // (after Mob's equipment drop, which the loot pass runs).
        void DropCustomDeathLoot(EntityLevel& level) override;

        // MC NEAREST_VISIBLE_WANTED_ITEM — item entities are not Entities in
        // this engine, so the NEAREST_ITEMS sensor's answer is the item
        // entity's id, kept here (the Allay pattern).
        std::optional<int32_t> GetWantedItemId() const { return m_wantedItemId; }
        void SetWantedItemId(std::optional<int32_t> id) { m_wantedItemId = id; }

        // MC Piglin.getArmPose (PiglinArmPose ordinals): DANCING, ADMIRING_ITEM
        // (a loved item in the off hand), ATTACKING_WITH_MELEE_WEAPON
        // (aggressive, a WEAPON in hand), CROSSBOW_CHARGE, CROSSBOW_HOLD (a
        // loaded crossbow), else DEFAULT. Both sides.
        int GetPiglinArmPose() const;

        void Tick() override;

    protected:
        // MC AbstractPiglin.customServerAiStep — the zombification clock.
        void CustomServerAiStep() override;
        // MC Piglin.populateDefaultEquipmentSlots: an adult's four 10% gold
        // armour rolls.
        void PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty) override;

    private:
        // MC Piglin.finishConversion: cancelAdmiring, the pockets dropped,
        // then AbstractPiglin's conversion (equipment kept).
        void FinishPiglinConversion();

        bool m_baby = false;
        bool m_cannotHunt = false;
        bool m_dancing = false;
        bool m_chargingCrossbow = false;
        bool m_immuneToZombification = false;
        int  m_timeInOverworld = 0;   // MC AbstractPiglin.timeInOverworld
        int  m_clientChargeTicks = -1;
        SimpleContainer m_inventory{ kInventorySize };
        std::optional<int32_t> m_wantedItemId;
    };

    // MC monster/piglin/PiglinBrute on the ported PiglinBruteAi brain — the
    // simpler always-hostile cousin: no baby form, no bartering, never flees,
    // and it patrols the HOME position it spawned at. Spawns with a golden
    // axe and only ever picks up another.
    class PiglinBrute : public GenericMonster {
    public:
        explicit PiglinBrute(EntityLevel* level);

        void UpdateBrainActivity() override;

        bool IsImmuneToZombification() const { return m_immuneToZombification; }
        void SetImmuneToZombification(bool v) { m_immuneToZombification = v; }
        // Bit 0: AbstractPiglin's DATA_IMMUNE_TO_ZOMBIFICATION (see Piglin).
        uint8_t GetAnimStateByte() const override { return m_immuneToZombification ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_immuneToZombification = (v & 1) != 0; }
        // MC AbstractPiglin.playAmbientSound: only while IDLE.
        void PlayAmbientSound() override;

        // MC AbstractPiglin.timeInOverworld — see the note on Piglin's copy.
        int  GetTimeInOverworld() const { return m_timeInOverworld; }
        void SetTimeInOverworld(int ticks) { m_timeInOverworld = ticks; }
        bool IsConverting() const;

        // MC PiglinBrute.finalizeSpawn — PiglinBruteAi.initMemories (HOME),
        // the golden axe, then Mob's.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC PiglinBrute.hurtServer → PiglinBruteAi.wasHurtBy.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC PiglinBrute.wantsToPickUp: a golden axe only.
        bool WantsToPickUp(const ItemStack& stack) const override;

        // MC PiglinBrute.getArmPose: ATTACKING_WITH_MELEE_WEAPON while
        // aggressive holding a melee weapon, else DEFAULT.
        int GetPiglinArmPose() const;

    protected:
        void CustomServerAiStep() override;
        // MC PiglinBrute.populateDefaultEquipmentSlots: the golden axe.
        void PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty) override;

    private:
        bool m_immuneToZombification = false;
        int  m_timeInOverworld = 0;
    };

    // ── Axolotl ────────────────────────────────────────────────────────────

    // MC animal/axolotl/Axolotl on the ported AxolotlAi brain, Bucketable
    // included: a water bucket scoops it (variant, age, age lock, hunting
    // cooldown and the default keys ride the bucket), and one released from
    // a bucket is kept (fromBucket persistence).
    class Axolotl : public GenericAnimal {
    public:
        // MC Axolotl.isPushedByFluid: false.
        bool IsPushedByFluid() const override { return false; }
        // MC Axolotl.checkSpawnObstruction: level.isUnobstructed(this) only — the
        // base's no-liquid half would refuse every underwater spawn.
        bool CheckSpawnObstruction(EntityLevel& level) const override { return IsUnobstructed(level); }
        // MC Axolotl.playAttackSound.
        void PlayAttackSound() override { PlaySound("entity.axolotl.attack", 1.0f, 1.0f); }

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

        // MC Axolotl.removeWhenFarAway: !fromBucket && !hasCustomName — an
        // axolotl despawns despite being an Animal, unless named or bucketed.
        bool RemoveWhenFarAway(double) const override { return !FromBucket() && !HasCustomName(); }
        // MC Axolotl.requiresCustomPersistence: super || fromBucket.
        bool RequiresCustomPersistence() const override {
            return GenericAnimal::RequiresCustomPersistence() || FromBucket();
        }

        // ── MC Bucketable ─────────────────────────────────────────────────
        // FROM_BUCKET / "FromBucket".
        bool FromBucket() const { return m_fromBucket; }
        void SetFromBucket(bool fromBucket) { m_fromBucket = fromBucket; }
        // MC Axolotl.mobInteract: bucketMobPickup, else Animal's (feeding —
        // with Axolotl.usePlayerItem's tropical-fish-bucket refund).
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // MC saveToBucketTag: the default keys, AXOLOTL_VARIANT, and Age /
        // AgeLocked / HuntingCooldown.
        void SaveToBucket(ItemStack& bucket) const;
        // MC loadFromBucketTag (the variant arrives separately, as the
        // bucket's AXOLOTL_VARIANT component — applyImplicitComponents).
        void LoadFromBucket(const BucketEntityData& data);

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
        bool    m_fromBucket = false;   // MC Axolotl.FROM_BUCKET

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

        // MC Breeze.playAmbientSound: a CLIENT-local idle voice (level.
        // playLocalSound(this, ...)), and only when not fighting on the ground.
        void PlayAmbientSound() override;
        // MC Breeze.causeFallDamage: a landing from over 3 blocks thumps.
        bool CauseFallDamage(double fallDist, float damageMultiplier) override;

    private:
        void ResetAnimations();
        // MC Breeze.soundTick — the whirl's 1..80-tick timer.
        int m_soundTick = 0;
        // MC jumpTrailStartedTick: the long jump's first 5 ticks trail dust.
        int m_jumpTrailStartedTick = 0;
        // MC emitGroundParticles / emitJumpTrailParticles (client copy).
        void EmitGroundParticles(int amount);
        void EmitJumpTrailParticles();
        BlockState GroundStateForParticles() const;
    };

    // ── Warden ─────────────────────────────────────────────────────────────

    // MC monster/warden/Warden. It HEARS through MC's vibration system (a
    // VibrationSystem user on an entity position source at its eyes, radius
    // 16, #warden_can_listen, registered through a DynamicGameEventListener
    // that follows it between sections), and angers by sniffing you out
    // (WardenEntitySensor feeds NEAREST_ATTACKABLE, TryToSniff/Sniffing raise
    // anger on proximity), by touch, and by being hit. The anger ladder,
    // roar, sonic boom and the emerge/dig lifecycle are the real MC structure
    // on the ported brain.
    class Warden : public GenericMonster, public VibrationSystem {
    public:
        // `type` lets a subclass register under its own id (SilentWarden,
        // HushMobs.hpp) — the CamelHusk-on-Camel precedent.
        explicit Warden(EntityLevel* level, EntityTypeId type = EntityTypeId::Warden);

        // MC Warden.checkSpawnObstruction: the base test plus no block
        // collision for the type's standing box at the spawn position.
        bool CheckSpawnObstruction(EntityLevel& level) const override;

        // MC SonicBoom's `10.0F` (ai/behavior/warden/SonicBoom.java) — the
        // armour-bypassing beam damage. Virtual so the Silent Warden can hit
        // harder without forking the behaviour.
        virtual float SonicBoomDamage() const { return 10.0f; }

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

        // MC Warden.getAmbientSound — the AngerLevel's voice.
        const char* GetAmbientSound() const override;

        // MC Warden.dampensVibrations: its own steps are never heard.
        bool DampensVibrations() const override { return true; }

        // MC Warden implements VibrationSystem.
        VibrationData& GetVibrationData() override { return m_vibrationData; }
        VibrationUser& GetVibrationUser() override;
        void SetVibrationData(VibrationData data) { m_vibrationData = std::move(data); }
        const VibrationData& SavedVibrationData() const { return m_vibrationData; }

    private:
        void TickAngerManagement();
        void SortAnger();

        // MC Warden.VibrationUser.
        class WardenVibrationUser : public VibrationUser {
        public:
            explicit WardenVibrationUser(Warden& warden);
            int GetListenerRadius() const override { return 16; }
            const PositionSource& GetPositionSource() const override { return m_source; }
            GameEvents::Tag GetListenableEvents() const override { return GameEvents::Tag::WardenCanListen; }
            bool CanTriggerAvoidVibration() const override { return true; }
            bool CanReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event,
                                     const GameEventContext& context) override;
            void OnReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event, Entity* sourceEntity,
                                    Entity* projectileOwner, float receivingDistance) override;

        private:
            Warden&        m_warden;
            PositionSource m_source;
        };
        WardenVibrationUser      m_vibrationUser;
        VibrationData            m_vibrationData;
        VibrationListener        m_vibrationListener;
        // Last, so it is destroyed first: its destructor unregisters the
        // listener above from the level's dispatcher.
        DynamicGameEventListener m_dynamicGameEventListener;

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
        const char* GetAmbientSound() const override;
        // MC Creaking.playAttackSound.
        void PlayAttackSound() override { MakeSound("entity.creaking.attack"); }
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

        // MC Sniffer.getAmbientSound: quiet while searching or digging.
        const char* GetAmbientSound() const override;
        // MC Sniffer.playEatingSound — bound to the sniffer, 0.8..1.2 pitch.
        void PlayEatingSound() override;

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

        // MC Armadillo.canStayRolledUp.
        bool CanStayRolledUp() const {
            return !IsPanicking() && !IsInLiquid() && !IsLeashed() && !IsPassenger() && !IsVehicle();
        }

        // MC Armadillo.isScaredBy — what makes an armadillo curl up.
        bool IsScaredBy(const LivingEntity& other) const;

        void RollUp();
        void RollOut();

        // MC Armadillo: quiet while scared; a rolled-up hurt is muffled.
        const char* GetAmbientSound() const override;
        const char* GetHurtSound(MobDamageSource source) const override;
        void PlayEatingSound() override { MakeSound("entity.armadillo.eat"); }

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

        // MC CopperGolemOxidationLevels — the voice of the golem's weather
        // stage. This port has no weathering, so it is always UNAFFECTED.
        const char* GetHurtSound(MobDamageSource) const override { return "entity.copper_golem.hurt"; }
        const char* GetDeathSound() const override { return "entity.copper_golem.death"; }
        void PlayStepSound(const glm::ivec3&, BlockState) override { PlaySound("entity.copper_golem.step", 1.0f, 1.0f); }

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

        // MC CopperGolem.setOpenedChestPos / clearOpenedChestPos. The reader
        // is hasContainerOpen — the chest lid's opener recheck counts a golem
        // whose opened chest is that chest (the server's chest user counter).
        void SetOpenedChestPos(const glm::ivec3& pos) { m_openedChestPos = pos; }
        void ClearOpenedChestPos() { m_openedChestPos.reset(); }
        const std::optional<glm::ivec3>& OpenedChestPos() const { return m_openedChestPos; }

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

    // MC animal/allay/Allay (implemented in Allay.cpp) on the AllayAi brain:
    // the item courier — hand it an item and it becomes that player's
    // (LIKED_PLAYER), picks up matching stacks into its one-slot inventory
    // and throws them to the player, or to the note block it last heard
    // (LIKED_NOTEBLOCK_POSITION, a 600-tick memory refreshed by each note);
    // the jukebox dance and the amethyst-shard duplication; the vibration
    // listener for note blocks — MC's VibrationSystem user (#allay_can_listen,
    // radius 16, at its eyes) on the level's game-event dispatcher, moved
    // between sections by a DynamicGameEventListener. The JukeboxListener
    // half (JUKEBOX_PLAY / _STOP_PLAY) polls the server's JukeboxSongRegistry
    // on MC's 20-tick play-event cadence instead.
    //
    // Wire: the anim byte carries hasItemInHand (bit 0), DATA_DANCING
    // (bit 1) and DATA_CAN_DUPLICATE (bit 2); the held stack itself rides
    // BodyArmorS2C with slot MAINHAND (MC ClientboundSetEquipmentPacket).
    class Allay : public GenericPathfinderMob, public VibrationSystem {
    public:
        // MC Allay constants.
        static constexpr int   kDuplicationCooldownTicks = 6000;  // DUPLICATION_COOLDOWN_TICKS
        static constexpr int   kMaxNoteblockDistance = 1024;      // MAX_NOTEBLOCK_DISTANCE
        static constexpr float kLiftingItemAnimationDuration = 5.0f;
        static constexpr float kDancingLoopDuration = 55.0f;
        static constexpr float kSpinningAnimationDuration = 15.0f;
        // VibrationUser.getListenerRadius (VIBRATION_EVENT_LISTENER_RANGE).
        static constexpr int   kVibrationListenerRange = 16;
        // GameEvent.JUKEBOX_PLAY / JUKEBOX_STOP_PLAY notification radius —
        // the JukeboxListener's radius too.
        static constexpr int   kJukeboxNotificationRadius = 10;

        explicit Allay(EntityLevel* level);

        // MC Allay.getAmbientSound: ALLAY_AMBIENT_WITH_ITEM while it holds
        // something in its main hand.
        const char* GetAmbientSound() const override;
        void UpdateBrainActivity() override;

        // MC Allay.removeWhenFarAway (Allay.java:384-386) — false: an allay
        // never distance-despawns. Mob's base default is true, which was
        // silently despawning them.
        bool RemoveWhenFarAway(double) const override { return false; }

        void Tick() override;
        void AiStep() override;
        // MC Allay.hurtServer: its liked player cannot hurt it.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // MC Allay.mobInteract: duplication, give, take back.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // MC Allay.canDispenserEquipIntoSlot: never.
        bool CanDispenserEquipIntoSlot(EquipmentSlot) const override { return false; }
        // MC Allay.dropEquipment: the inventory and the held stack.
        void DropEquipment(EntityLevel& level) override;
        // MC Allay.handleEntityEvent(18): three hearts (duplication).
        void HandleEntityEvent(uint8_t id) override;

        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;

        // MC's MAINHAND slot (the only one the allay fills).
        ItemStack* EquipmentInSlot(EquipmentSlot slot) override;
        bool HasEquipmentSlots() const override { return true; }

        // ── The held item (MAINHAND) ───────────────────────────────────────
        const ItemStack& GetMainHandItem() const { return m_handItem; }
        // Server: sets the slot and marks it for the tracker. Client: the
        // synched copy (BodyArmorS2C, slot MAINHAND).
        void SetMainHandItem(const ItemStack& stack);
        // MC hasItemInHand. Server: the slot; client: the synched bit.
        bool HasItemInHand() const;
        // The tracker's send-on-change latch for the held stack.
        bool ConsumeHandItemDirty() {
            const bool d = m_handItemDirty;
            m_handItemDirty = false;
            return d;
        }

        // ── InventoryCarrier (MC SimpleContainer(1)) ──────────────────────
        SimpleContainer&       GetInventory()       { return m_inventory; }
        const SimpleContainer& GetInventory() const { return m_inventory; }
        // MC SimpleContainer.canAddItem / addItem / removeItem(0, 1).
        bool      InventoryCanAddItem(const ItemStack& stack) const;
        ItemStack InventoryAddItem(const ItemStack& stack);

        // MC Allay.canPickUpLoot — not on pickup cooldown and holding
        // something. (Mob's CanPickUpLoot flag is not what the allay reads.)
        bool CanPickUpLootNow() const;
        // MC Allay.wantsToPickUp: the same item as the one in hand (potion
        // contents included), room for it, mobGriefing on.
        bool WantsToPickUp(const ItemStack& stack) const override;
        // MC Allay.canPickUpLoot (Mob.aiStep's looting gate, TickLooting):
        // CanPickUpLootNow.
        bool CanPickUpLoot() const override { return CanPickUpLootNow(); }
        // MC Allay.getPickupReach: ITEM_PICKUP_REACH (1, 1, 1).
        glm::ivec3 GetPickupReach() const override { return glm::ivec3(1, 1, 1); }
        // MC Allay.pickUpItem → InventoryCarrier.pickUpItem: room or nothing,
        // then the part that fits goes into the one-slot inventory.
        void PickUpItem(int32_t itemEntityId, const ItemStack& stack) override;
        // MC NEAREST_VISIBLE_WANTED_ITEM — item entities are not Entities in
        // this engine, so the sensor's answer is the item entity's id.
        std::optional<int32_t> GetWantedItemId() const { return m_wantedItemId; }
        void SetWantedItemId(std::optional<int32_t> id) { m_wantedItemId = id; }

        // ── LIKED_PLAYER ───────────────────────────────────────────────────
        // MC keeps the UUID in the brain; the brain's memory variant has no
        // UUID kind, so the value lives here and the LIKED_PLAYER memory
        // mirrors its presence.
        const std::optional<Uuid>& GetLikedPlayerUuid() const { return m_likedPlayer; }
        void SetLikedPlayerUuid(std::optional<Uuid> uuid);
        bool IsLikedPlayer(const Entity* other) const;
        // MC AllayAi.getLikedPlayer: the liked player when it is in this
        // level, not a spectator, and within 64 blocks.
        LivingEntity* GetLikedPlayer() const;

        // ── Dancing / duplication ──────────────────────────────────────────
        bool IsDancing() const { return m_dancing; }
        // MC setDancing: server, effective AI, and never INTO a dance while
        // panicking.
        void SetDancing(bool dancing);
        // MC setJukeboxPlaying (the JukeboxListener's two events).
        void SetJukeboxPlaying(const glm::ivec3& jukebox, bool playing);
        bool CanDuplicate() const { return m_canDuplicate; }
        int64_t GetDuplicationCooldown() const { return m_duplicationCooldown; }
        void SetDuplicationCooldown(int64_t ticks);

        // ── Client animation (MC Allay.tick's client half) ────────────────
        float GetHoldingItemAnimationProgress(float partialTick) const;
        bool  IsSpinning() const;
        float GetSpinningProgress(float partialTick) const;

        // ── The vibration listener (MC Allay implements VibrationSystem) ──
        VibrationData& GetVibrationData() override { return m_vibrationData; }
        VibrationUser& GetVibrationUser() override;
        void SetVibrationData(VibrationData data) { m_vibrationData = std::move(data); }
        const VibrationData& SavedVibrationData() const { return m_vibrationData; }

    private:
        bool IsBrainPanicking() const;
        bool ShouldStopDancing() const;
        void DuplicateAllay();
        void TickJukeboxListener();
        void TickVibrations();

        // MC Allay.VibrationUser.
        class AllayVibrationUser : public VibrationUser {
        public:
            explicit AllayVibrationUser(Allay& allay);
            int GetListenerRadius() const override { return 16; }
            const PositionSource& GetPositionSource() const override { return m_source; }
            GameEvents::Tag GetListenableEvents() const override { return GameEvents::Tag::AllayCanListen; }
            bool CanReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event,
                                     const GameEventContext& context) override;
            void OnReceiveVibration(World& level, const glm::ivec3& pos, GameEventId event, Entity* sourceEntity,
                                    Entity* projectileOwner, float receivingDistance) override;

        private:
            Allay&         m_allay;
            PositionSource m_source;
        };

        ItemStack       m_handItem;
        bool            m_handItemDirty = false;
        bool            m_clientHoldingItem = false;
        SimpleContainer m_inventory{ 1 };
        std::optional<int32_t> m_wantedItemId;
        std::optional<Uuid>    m_likedPlayer;

        bool m_dancing = false;          // DATA_DANCING
        bool m_canDuplicate = true;      // DATA_CAN_DUPLICATE
        std::optional<glm::ivec3> m_jukeboxPos;
        int64_t m_duplicationCooldown = 0;
        // The jukeboxes within earshot at the last listener pass — a song
        // that leaves the registry from this set is MC's JUKEBOX_STOP_PLAY.
        std::vector<glm::ivec3> m_heardJukeboxes;

        AllayVibrationUser       m_vibrationUser;
        VibrationData            m_vibrationData;
        VibrationListener        m_vibrationListener;
        // After the listener it registers, so it unregisters first.
        DynamicGameEventListener m_dynamicGameEventListener;

        float m_holdingItemAnimationTicks = 0.0f;
        float m_holdingItemAnimationTicks0 = 0.0f;
        float m_dancingAnimationTicks = 0.0f;
        float m_spinningAnimationTicks = 0.0f;
        float m_spinningAnimationTicks0 = 0.0f;
    };

    // ── HappyGhast ─────────────────────────────────────────────────────────

    // MC animal/ghast/HappyGhast, with MC's real AI split: the ADULT is
    // goal-driven (this port keeps the def-driven set — which carries the
    // flying wander — plus HappyGhastFloatGoal at MC's priority 3), and the
    // BABY ghastling runs HappyGhastAi's brain (tempt-follow, trailing
    // players and followable adults, flying wander, panic). The class swaps
    // setups at the age boundary as MC's adultGhastSetup/babyGhastSetup do.
    //
    // Riding (MC 26.3): a harness in the BODY slot makes an adult rideable
    // by up to four players (mobInteract → startRiding, four seats at the
    // harness corners); the first player steers it (getControllingPassenger)
    // unless it is on its still timeout, flying where the rider looks —
    // forward along the view pitch, backwards at half, jump to rise
    // (getRiddenInput), the body easing toward the rider's yaw at 8% a tick
    // (tickRidden). The still timeout (STAYS_STILL / serverStillTimeout)
    // freezes it in place while a player stands on it or boards it, and is
    // what makes its top a platform (canBeCollidedWith).
    class HappyGhast : public GenericAnimal {
    public:
        explicit HappyGhast(EntityLevel* level);

        void UpdateBrainActivity() override;

        // MC HappyGhast.MAX_PASSANGERS / BABY_SCALE / MAX_STILL_TIMEOUT /
        // STILL_TIMEOUT_ON_LOAD_GRACE_PERIOD / the restriction radii.
        static constexpr int   kMaxPassengers = 4;
        static constexpr float kBabyScale = 0.2375f;
        static constexpr int   kMaxStillTimeout = 10;
        static constexpr int   kStillTimeoutOnLoadGracePeriod = 60;
        static constexpr int   kSmallRestrictionRadius = 32;
        static constexpr int   kLargeRestrictionRadius = 64;
        static constexpr int   kRestrictionRadiusBuffer = 16;

        // MC HappyGhast.getMaxSpawnClusterSize (HappyGhast.java:201-203).
        int GetMaxSpawnClusterSize() const override { return 1; }

        // MC HappyGhast.isOnStillTimeout: the synched STAYS_STILL flag, or
        // (server) a still timeout still counting.
        bool IsOnStillTimeout() const { return m_staysStill || m_serverStillTimeout > 0; }
        // MC HappyGhast.staysStill (DATA STAYS_STILL).
        bool StaysStill() const { return m_staysStill; }
        // MC HappyGhast.isLeashHolder (DATA IS_LEASH_HOLDER) — it holds a
        // quad-leashed mob now (the RopesLayer's input).
        bool IsLeashHolder() const { return m_isLeashHolder; }
        // The still timeout MC saves as "still_timeout" (server).
        int  GetServerStillTimeout() const { return m_serverStillTimeout; }
        void SetServerStillTimeout(int ticks);

        // MC isWearingBodyArmor — for the happy ghast, the harness.
        bool IsWearingHarness() const;
        // ── The harness slot (MC HappyGhast.canUseSlot /
        //    canDispenserEquipIntoSlot) ─────────────────────────────────────
        // A harness (BODY) only on a live, grown ghast.
        bool CanUseSlot(EquipmentSlot slot) const override {
            if (slot != EquipmentSlot::BODY) return GenericAnimal::CanUseSlot(slot);
            return IsAlive() && !IsBaby();
        }
        // A dispenser only ever harnesses it.
        bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const override { return slot == EquipmentSlot::BODY; }
        // Any passenger aboard, on either side (MC isVehicle(): the server's
        // list holds the riders' views; a client knows players only by the
        // synched seat order) — the harness goggles are down while it holds
        // (HappyGhastRenderer: state.isRidden = isVehicle()).
        bool IsRidden() const;
        bool AreGogglesDown() const { return IsRidden(); }

        // ── Riding ────────────────────────────────────────────────────────
        // MC HappyGhast.mobInteract: a foal is Animal's; otherwise the held
        // item's interactLivingEntity first (the harness, a lead), then a
        // harnessed ghast seats a player who is not sneaking.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // MC getControllingPassenger: harnessed, not on the still timeout,
        // the first passenger a player.
        bool CanBeSteeredBy(const RiderControl& rider) const override;
        glm::dvec3 GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) override;
        void TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) override;
        // MC canAddPassenger: four seats.
        bool CanAddPassenger(const Entity& passenger) const override;
        // MC's four passengerAttachments (EntityTypes HAPPY_GHAST), picked by
        // seat (EntityAttachments.getClamped), turned by the yaw.
        glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const override;
        glm::dvec3 GetPassengerAttachmentForSlot(int slot, int total) const override;
        // MC getDismountLocationForPassenger: on top, at the centre.
        glm::dvec3 GetDismountLocationForPassenger(const LivingEntity& passenger) const override;
        // MC addPassenger / removePassenger: the goggles sounds, the still
        // timeout, the home dropped when the last rider leaves.
        void OnPassengerAdded(Entity& passenger, bool wasVehicle) override;
        void OnPassengerRemoved(Entity& passenger) override;

        // MC canBeCollidedWith(other) for a player `otherFeetY` tall at the
        // feet (client side: the local player's collision): an adult alive
        // ghast is solid to a player at or above its top, and wholly while
        // on its still timeout.
        bool CanBeCollidedWithPlayer(double otherFeetY) const;

        // MC HappyGhast.travel: travelFlying at FLYING_SPEED * 5/3 in every
        // medium — no gravity at all.
        void Travel(const glm::dvec3& input) override;

        // MC getAmbientSoundInterval: six times as long while ridden.
        int GetAmbientSoundInterval() const override;

        // MC notifyLeashHolder: a quad-leashed mob keeps the flag up 5 ticks.
        void NotifyLeashHolder(Mob& leashee) override;

        void Tick() override;

        // Anim byte: bit 0 STAYS_STILL, bit 1 IS_LEASH_HOLDER.
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_staysStill ? 1 : 0) | (m_isLeashHolder ? 2 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_staysStill = (v & 1) != 0;
            m_isLeashHolder = (v & 2) != 0;
        }

        std::unique_ptr<Animal> CreateBaby() override {
            return std::make_unique<HappyGhast>(m_level);
        }

    protected:
        // MC HappyGhast.ageBoundaryReached, detected by polling — the port's
        // AgeableMob has no boundary hook — then MC customServerAiStep's
        // checkRestriction and GhastMoveControl's shouldBeStopped.
        void CustomServerAiStep() override;
        // MC HappyGhastBodyRotationControl.clientTick: while ridden the head
        // and body follow the yaw.
        void TickHeadTurn(float yBodyRotTarget) override;

    private:
        void AdultSetup();
        void BabySetup();
        void RegisterAdultGoals();
        // MC checkRestriction / scanPlayerAboveGhast / syncStayStillFlag.
        void CheckRestriction();
        bool ScanPlayerAboveGhast() const;
        glm::dvec3 SeatAttachment(int slot) const;

        bool m_wasBaby = false;
        int  m_serverStillTimeout = 0;
        int  m_leashHolderTime = 0;
        bool m_staysStill = false;
        bool m_isLeashHolder = false;
    };

} // namespace Game
