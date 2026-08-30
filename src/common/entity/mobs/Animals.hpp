// File: src/common/entity/mobs/Animals.hpp
//
// Cow, Pig, Sheep and Chicken.
//
// All four share the same goal skeleton — float, panic, breed, tempt, follow
// parent, stroll, look at player, look around — differing only in priorities,
// speeds and food. That is MC's structure and it is kept, because the
// priorities are where the personality is: a pig panics at 1.25 and a chicken
// at 1.4, so chickens visibly scatter faster.
#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <string_view>

#include "common/entity/Animal.hpp"
#include "common/entity/NeutralMob.hpp"
#include "common/entity/RangedAttackMob.hpp"
#include "common/entity/TamableAnimal.hpp"
#include "common/entity/mobs/GenericMobs.hpp"

namespace Game {

    // MC Cow. MAX_HEALTH 10, MOVEMENT_SPEED 0.2.
    class Cow : public Animal {
    public:
        explicit Cow(EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
    };

    // MC animal/cow/MushroomCow, promoted from the generic path for the
    // shear conversion: shears turn it into a COW (Mob::ConvertTo) and pop
    // five mushrooms — the interaction the Pig-comment's conversion note
    // waited on. It keeps the def's attributes and goal set (a mooshroom is
    // behaviourally a cow). Skipped at their sites: the bowl → mushroom stew
    // milking (bowls have no fill-result flow here), the brown-mooshroom
    // flower-feeding stew effects (needs the variant + suspicious stew), and
    // the red/brown variant itself (the renderer draws red; the def path
    // never rolled one).
    class Mooshroom : public GenericAnimal {
    public:
        explicit Mooshroom(EntityLevel* level)
            : GenericAnimal(EntityTypeId::Mooshroom, level) {}

        // MC MushroomCow.mobInteract — the shears branch; bowl/stew skipped.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC MushroomCow.readyForShearing: alive and not a calf.
        bool ReadyForShearing() const { return IsAlive() && !IsBaby(); }

        // MC MushroomCow.shear — convert to Cow, drop 5 mushrooms.
        void Shear();
    };

    // MC Pig. MAX_HEALTH 10, MOVEMENT_SPEED 0.25.
    //
    // MC Pig.thunderHit (pig → ZOMBIFIED_PIGLIN via Mob.convertTo when
    // lightning strikes) is SKIPPED: weather has no lightning strikes in this
    // engine, so the thunderHit hook that starts it never fires. The
    // conversion machinery itself is live — see Mob::ConvertTo, and
    // Mooshroom::Shear above for the interaction-driven conversion that
    // landed with the mobInteract wave.
    class Pig : public Animal {
    public:
        explicit Pig(EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
    };

    // MC Sheep. MAX_HEALTH 8, MOVEMENT_SPEED 0.23.
    //
    // The wool byte packs colour in the low four bits and "sheared" in bit 16,
    // exactly as MC's DATA_WOOL_ID does, so the renderer and the wire format
    // both stay one byte.
    class Sheep : public Animal {
    public:
        explicit Sheep(EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        uint8_t GetColor() const { return m_woolData & 0x0F; }
        void    SetColor(uint8_t color);
        bool    IsSheared() const { return (m_woolData & 0x10) != 0; }
        void    SetSheared(bool sheared);
        uint8_t GetWoolData() const { return m_woolData; }
        void    SetWoolData(uint8_t v) { m_woolData = v; }

        // The wool byte IS the sheep's wire variant.
        uint8_t GetVariantByte() const override { return m_woolData; }
        void    SetVariantByte(uint8_t v) override { m_woolData = v; }

        // Grazing regrows wool, and grows a lamb toward adulthood.
        void OnEatBlock() override;

        // 0..1 head-down amount for the renderer, driven by the eat goal.
        float GetHeadEatPositionScale(float partialTick) const;
        float GetHeadEatAngleScale(float partialTick) const;

        void CustomServerAiStep() override;

        // The grazing goal, for the /sheepeat debug command.
        class EatBlockGoal* GetEatBlockGoal() const { return m_eatBlockGoal; }

        // MC Sheep.handleEntityEvent(10) — start the 40-tick graze animation.
        void HandleEntityEvent(uint8_t id) override;

        // MC Sheep.aiStep — counts the animation down CLIENT-side.
        void AiStep() override;

        static void CreateAttributes(AttributeMap& out);

        // MC SheepColorSpawnRules.getSheepColor — biome-dependent, so a
        // savanna sheep is usually brown and a snowy one usually black.
        static uint8_t RandomSpawnColor(class JavaRandom& rng, std::string_view biome);

        // MC Sheep.finalizeSpawn: roll the wool colour. Called for every spawn
        // reason, which is why spawn eggs give coloured sheep in vanilla.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Sheep.mobInteract — the shears branch.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Sheep.readyForShearing: alive, unsheared, and NOT A LAMB.
        bool ReadyForShearing() const;

        // MC Sheep.shear — drop the wool and set the sheared flag.
        void Shear();

        // The wool item matching a DyeColor ordinal. Static because the death
        // drop needs it from the server's loot path as well as the shear does.
        static uint32_t WoolItemForColor(uint8_t color);

    protected:
        void RegisterGoals() override;

    private:
        uint8_t m_woolData = 0;
        int     m_eatAnimationTick = 0;
        class EatBlockGoal* m_eatBlockGoal = nullptr;
    };

    // MC Chicken. MAX_HEALTH 4, MOVEMENT_SPEED 0.25.
    class Chicken : public Animal {
    public:
        explicit Chicken(EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // MC Chicken.aiStep — the wing flap, which is both the animation and
        // the slow-fall: descending motion is scaled by 0.6 every tick, so a
        // chicken never takes fall damage.
        void AiStep() override;

        float GetFlap(float partialTick) const;
        float GetFlapSpeed(float partialTick) const;

        static void CreateAttributes(AttributeMap& out);

        // MC Chicken.isChickenJockey / setChickenJockey — set by the zombie
        // side when a baby zombie spawns riding this chicken. It flips two
        // rules, both MC's: a jockey chicken IS eligible for far-away despawn
        // (the one exception to "animals never despawn" — the ride despawns
        // with its rider), and it never lays eggs.
        bool IsChickenJockey() const { return m_isChickenJockey; }
        void SetChickenJockey(bool v) { m_isChickenJockey = v; }

        // MC Chicken.removeWhenFarAway: `return this.isChickenJockey();`
        bool RemoveWhenFarAway(double) const override { return m_isChickenJockey; }

        // MC Chicken.getBaseExperienceReward (Chicken.java:166-168): a jockey
        // chicken pays 10; a plain chicken pays Animal's 1..3.
        int GetXpReward() const override {
            return m_isChickenJockey ? 10 : Animal::GetXpReward();
        }

    protected:
        void RegisterGoals() override;

    private:
        float m_flap = 0.0f, m_oFlap = 0.0f;
        float m_flapSpeed = 0.0f, m_oFlapSpeed = 0.0f;
        float m_flapping = 1.0f;
        int   m_eggTime = 0;
        bool  m_isChickenJockey = false;   // MC DEFAULT_CHICKEN_JOCKEY = false
    };

    // MC Parrot. MAX_HEALTH 6, FLYING_SPEED 0.4, MOVEMENT_SPEED 0.2,
    // ATTACK_DAMAGE 3.
    //
    // The parts that make a parrot a parrot here: FlyingMoveControl(10, false)
    // + the flying navigation, the flap state machine (calculateFlapping —
    // also the slow fall: descending motion is scaled by 0.6, and
    // checkFallDamage is a no-op), never breeding, and — taming landed —
    // seed-taming (1/10), sit-on-command, follow-owner (it may perch on
    // leaves: canFlyToOwner), and the cookie poison-kill. Not modelled, each
    // named at its site: LandOnOwnersShoulderGoal (shoulder riding needs the
    // player render), the five-colour variant (needs per-variant textures;
    // the wire byte exists but the renderer draws red_blue), the jukebox
    // party dance, and the mob-sound imitation.
    class Parrot : public Animal, public TamableAnimal {
    public:
        explicit Parrot(EntityLevel* level);

        // MC Parrot.mobInteract — seeds tame (1/10), cookies kill, a tame
        // grounded parrot toggles sitting.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Parrot.canFlyToOwner: true — the teleport may land on leaves.
        bool CanFlyToOwner() const override { return true; }

        // MC Parrot.isFlying (Parrot.java:362): airborne. Deliberately NOT
        // virtual and deliberately unrelated to Entity::IsAbilityFlying — see
        // the note there.
        bool IsFlying() const { return !onGround; }

        // MC TamableAnimal.canAttack — never the owner.
        bool CanAttack(const LivingEntity& target) const override {
            return TamableCanAttack(target) && Animal::CanAttack(target);
        }

        // MC TamableAnimal.handleEntityEvent: 7 = taming hearts, 6 = taming
        // smoke; everything else to Animal.
        void HandleEntityEvent(uint8_t id) override {
            if (!HandleTamableEntityEvent(id)) Animal::HandleEntityEvent(id);
        }

        // The tamable byte (bit 0 sitting pose, bit 1 tame) IS the parrot's
        // anim state byte — its first user.
        uint8_t GetAnimStateByte() const override { return GetTamableAnimByte(); }
        void    SetAnimStateByte(uint8_t v) override { SetTamableAnimByte(v); }

        // MC Parrot.isFood: false — seeds TAME, they never breed.
        bool IsFood(uint32_t itemId) const override { return false; }
        // MC Parrot.getBreedOffspring returns null; canMate is false.
        std::unique_ptr<Animal> CreateBaby() override { return nullptr; }
        bool CanMate(const Animal& other) const override { return false; }
        // MC Parrot.isBaby: false — parrots have no baby form.
        bool IsBaby() const override { return false; }

        bool IsFlyingAnimal() const override { return true; }

        // MC Parrot.aiStep — super, then calculateFlapping.
        void AiStep() override;

        // MC ParrotRenderer.extractRenderState: flapAngle =
        // (sin(lerp(flap)) + 1) * lerp(flapSpeed).
        float GetFlapAngle(float partialTick) const;

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;

        // MC Parrot.checkFallDamage is empty — a parrot neither accumulates
        // fall distance nor takes fall damage.
        void CheckFallDamage(double dy, bool onGroundNow) override {
            (void)dy; (void)onGroundNow;
        }

    private:
        // MC Parrot.calculateFlapping — the flap fields verbatim.
        void CalculateFlapping();

        float m_flap = 0.0f, m_oFlap = 0.0f;
        float m_flapSpeed = 0.0f, m_oFlapSpeed = 0.0f;
        float m_flapping = 1.0f;
    };

    // MC animal/rabbit/Rabbit. MAX_HEALTH 3, MOVEMENT_SPEED 0.3,
    // ATTACK_DAMAGE 3.
    //
    // The parts that make a rabbit a rabbit here: the hop machinery —
    // RabbitJumpControl + RabbitMoveControl (Controls.hpp), the landing-delay
    // / face-then-jump plan in customServerAiStep, the speed-scaled jump
    // power, and the jumpTicks/jumpDuration animation clock the renderer
    // reads (armed by entity event 1). Not modelled, each named at its site:
    // the variant system (every rabbit renders brown; the EVIL killer-bunny
    // branch with it), RaidGardenGoal (needs crops + mob griefing),
    // ClimbOnTopOfPowderSnowGoal (no powder snow), and the jump/sprint
    // particles and sounds.
    class Rabbit : public Animal {
    public:
        explicit Rabbit(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ItemTags.RABBIT_FOOD: carrot, golden carrot, dandelion.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // MC Rabbit.getJumpPower — 0.2 while ambling, 0.3 by default, 0.5
        // when the path climbs, all relative to the 0.42 base impulse.
        float GetJumpPower() const override;

        // MC Rabbit.jumpFromGround — super, a forward nudge if barely moving,
        // then entity event 1 to arm every watcher's animation clock.
        void JumpFromGround() override;

        // MC RabbitRenderer.extractRenderState input.
        float GetJumpCompletion(float partialTick) const;

        // MC Rabbit.setSpeedModifier — feeds BOTH the navigation and the move
        // control. Public because RabbitMoveControl calls it.
        void SetSpeedModifier(double speed);

        // MC Rabbit.startJumping. Public because RabbitJumpControl calls it.
        void StartJumping();

        // MC Rabbit.aiStep — the jump animation clock, both sides.
        void AiStep() override;

        // MC Rabbit.handleEntityEvent(1) — start the jump animation.
        void HandleEntityEvent(uint8_t id) override;

        // MC Rabbit.wantsMoreFood / moreCarrotTicks — RaidGardenGoal's
        // appetite gate: 40 ticks of satiety per raided carrot, decayed by
        // rand(3) per server tick in CustomServerAiStep.
        bool WantsMoreFood() const { return m_moreCarrotTicks <= 0; }
        void SetMoreCarrotTicks(int t) { m_moreCarrotTicks = t; }
        // WantsMoreFood collapses the counter to a bool, which cannot restore
        // the remaining satiety; the save layer needs the tick count itself.
        int  GetMoreCarrotTicks() const { return m_moreCarrotTicks; }

    protected:
        void RegisterGoals() override;

        // MC Rabbit.customServerAiStep — the whole hop planner.
        void CustomServerAiStep() override;

    private:
        void SetJumping(bool jump);
        void FacePoint(double x, double z);
        void EnableJumpControl();
        void DisableJumpControl();
        void SetLandingDelay();
        void CheckLandingDelay();

        int  m_jumpTicks = 0;
        int  m_jumpDuration = 0;
        bool m_wasOnGround = false;
        int  m_jumpDelayTicks = 0;
        int  m_moreCarrotTicks = 0;   // MC Rabbit.moreCarrotTicks
    };

    // MC animal/polarbear/PolarBear. MAX_HEALTH 30, FOLLOW_RANGE 20,
    // MOVEMENT_SPEED 0.25, ATTACK_DAMAGE 6.
    //
    // The parts that make a polar bear a polar bear here: the rear-up —
    // DATA_STANDING synced on the wire's anim byte, driven by
    // PolarBearMeleeAttackGoal (AttackGoals.hpp), lerped client-side into the
    // stand scale the renderer reads — plus the protect-the-cub target goals
    // (TargetGoals.hpp) and the persistent-anger system (NeutralMob: the
    // isAngryAt-gated player hunt at target 3, ResetUniversalAngerTargetGoal
    // at 5). Not modelled, each named at its site: sounds (warning growl
    // included) and the 0.98 water slow-down (LivingEntity's water drag has
    // no per-mob hook).
    class PolarBear : public Animal, public NeutralMob {
    public:
        explicit PolarBear(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC PolarBear.isFood: false — polar bears cannot be fed or bred.
        bool IsFood(uint32_t itemId) const override { return false; }
        std::unique_ptr<Animal> CreateBaby() override;

        // MC DATA_STANDING_ID — synced on the wire's one anim state byte,
        // the Guardian `moving` pattern. Public because the melee goal
        // drives it (MC's is a private inner class of PolarBear).
        bool IsStanding() const { return m_standing; }
        void SetStanding(bool v) { m_standing = v; }
        uint8_t GetAnimStateByte() const override { return m_standing ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_standing = (v & 1) != 0; }

        // MC PolarBear.playWarningSound — keeps MC's 40-tick cadence; the
        // growl itself waits on the sound system.
        void PlayWarningSound();

        // MC PolarBear.tick — the client-side 0..6 stand animation lerp.
        void Tick() override;

        // MC PolarBear.getDefaultDimensions — the hitbox grows with the
        // stand animation (client-side only, where the animation runs).
        float GetBbHeight() const override;

        // MC PolarBearRenderer.extractRenderState: the RAW 0..1 lerp — the
        // MODEL is what squares it.
        float GetStandingAnimationScale(float partialTick) const;

        // MC PolarBear.startPersistentAngerTimer — rangeOfSeconds(20, 39).
        void StartPersistentAngerTimer() override;

        // MC PolarBear.aiStep tail: updatePersistentAnger(level, true).
        void AiStep() override;

        void ClearReferenceTo(const Entity* entity) override {
            Animal::ClearReferenceTo(entity);
            ClearAngerReferenceTo(entity);
        }

    protected:
        void RegisterGoals() override;

    private:
        bool  m_standing = false;
        // MC clientSideStandAnimation(O) — written by Tick on the client only.
        float m_clientSideStandAnimation = 0.0f;
        float m_clientSideStandAnimationO = 0.0f;
        int   m_warningSoundTicks = 0;
    };

    // MC animal/wolf/Wolf, promoted from the generic path for the
    // persistent-anger system (NeutralMob): a wild wolf targets a player only
    // while ANGRY at them — being hit starts a 400..780-tick grudge, the
    // whole pack is alerted (HurtByTargetGoal.setAlertOthers), and the angry
    // state drives the red-eyed texture and raised tail. It keeps
    // GenericAnimal's def-driven goal set (float/panic/breed/tempt/
    // follow-parent/stroll/looks + the def's attributes) and layers MC's
    // combat goals on top: LeapAtTargetGoal(0.4) at 4, MeleeAttackGoal(1.0,
    // true) at 5 — the generic wolf could acquire targets but had no attack
    // goal at all.
    //
    // MC synchronises DATA_ANGER_END_TIME so the CLIENT's isAngry() picks the
    // angry texture; this wire has no per-mob long, so the angry state is
    // mapped onto the aggressive bit each server tick (which is also what the
    // renderer's wolf branch already reads for the 1.5393804 tail angle) —
    // overriding MeleeAttackGoal's own start/stop writes one tick later,
    // deliberately: MC's wolf visual keys on anger, not on mid-swing.
    //
    // Taming landed (TamableAnimal mixin): bone-taming, sit-on-command,
    // follow-owner with the teleport, owner defence (OwnerHurtBy/
    // OwnerHurtTargetGoal), the 40-health tame boost, and the
    // NonTameRandomTargetGoal prey hunts. Still skipped at their sites:
    // BegGoal (needs the held-item render), WolfAvoidEntityGoal (needs the
    // llama's strength stat), collar/tame texture (renderer texture table is
    // per-type), and wolf armor/variants (ride items).
    class Wolf : public GenericAnimal, public NeutralMob, public TamableAnimal {
    public:
        explicit Wolf(EntityLevel* level);

        // MC Wolf.startPersistentAngerTimer — rangeOfSeconds(20, 39).
        void StartPersistentAngerTimer() override;

        // MC Wolf.aiStep tail: updatePersistentAnger(level, true); plus the
        // aggressive-bit mapping described above. (The wet-shake machinery —
        // isWet/isShaking, entity events 8/56 — waits on a rain query and
        // the shake render pass.)
        void AiStep() override;

        // MC Wolf.mobInteract — bone-taming for the wild, feed/sit-toggle
        // for the tame (dye collar and wolf armor branches skipped: no
        // collar render layer, no wolf armor item).
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Wolf.applyTamingSideEffects: MAX_HEALTH 40 tame, 8 wild.
        void ApplyTamingSideEffects() override;

        // MC Wolf.hurtServer: a hit wolf stands up.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Wolf.getMaxHeadXRot: 20 while sitting.
        int GetMaxHeadXRot() const override {
            return IsInSittingPose() ? 20 : GenericAnimal::GetMaxHeadXRot();
        }

        // MC Wolf.getMaxSpawnClusterSize (Wolf.java:508-510) — a pack of 8,
        // double the Mob default of 4.
        int GetMaxSpawnClusterSize() const override { return 8; }

        // MC TamableAnimal.canAttack — never the owner.
        bool CanAttack(const LivingEntity& target) const override {
            return TamableCanAttack(target) && GenericAnimal::CanAttack(target);
        }

        // MC Wolf.handleEntityEvent: 8 (begin shake) / 56 (cancel shake)
        // wait on the wet-shake machinery (see AiStep note); the inherited
        // TamableAnimal half — 7 taming hearts / 6 taming smoke — lands
        // here.
        void HandleEntityEvent(uint8_t id) override {
            if (!HandleTamableEntityEvent(id))
                GenericAnimal::HandleEntityEvent(id);
        }

        // MC Wolf.wantsToAttack — no creepers/ghasts, no tame animals, no
        // tamed horses. (ArmorStand and the owner-PvP canHarmPlayer test have
        // no equivalents here — no armor stands, PvP is always on.)
        bool WantsToAttack(const LivingEntity& target,
                           const LivingEntity& owner) const override;

        // MC Wolf.canMate: both tame, partner not sitting, both in love.
        bool CanMate(const Animal& other) const override;

        // MC Wolf.getBreedOffspring — a tame parent's pup inherits the tame
        // flag and the owner (the collar-colour mix rides the collar).
        std::unique_ptr<Animal> CreateBaby() override;

        // The tamable byte (bit 0 sitting pose, bit 1 tame) IS the wolf's
        // anim state byte; bit 2 carries MC's DATA_INTERESTED_ID (the beg
        // head-tilt).
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(GetTamableAnimByte() |
                                        (m_interested ? 4 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            SetTamableAnimByte(v);
            m_interested = (v & 4) != 0;
        }

        // MC Wolf.setIsInterested / isInterested — BegGoal's head tilt.
        void SetIsInterested(bool v) { m_interested = v; }
        bool IsInterested() const { return m_interested; }

        // MC Wolf.getHeadRollAngle — what WolfRenderer.extractRenderState
        // feeds the model's headRollAngle.
        float GetHeadRollAngle(float partialTick) const;

        // MC Wolf.getTailAngle: angry 1.5393804; tame scales with health
        // ((0.55 - damageRatio*0.4)*PI); wild idle PI/5.
        float GetTailAngle() const;

        void ClearReferenceTo(const Entity* entity) override {
            GenericAnimal::ClearReferenceTo(entity);
            ClearAngerReferenceTo(entity);
        }

    private:
        void RegisterWolfGoals();

        // MC Wolf.tryToTame — the 1/3 bone roll.
        void TryToTame(LivingEntity& player);

        // MC Wolf.interestedAngle(+O) — the beg tilt spring, ticked in
        // AiStep on both sides.
        bool  m_interested = false;
        float m_interestedAngle = 0.0f;
        float m_interestedAngleO = 0.0f;
    };

    // MC animal/equine/Llama, promoted from the generic base for the SPIT:
    // RangedAttackGoal(1.25, 40, 20) plus LlamaHurtByTargetGoal (spit once,
    // stand down) and LlamaAttackWolfGoal, layered ON TOP of the generic
    // animal goal set the base constructor already registered — the caravan,
    // chest, strength/variant and rider systems this port does not model are
    // exactly the parts the generic base never carried either.
    class Llama : public GenericAnimal, public RangedAttackMob {
    public:
        explicit Llama(EntityLevel* level) : Llama(EntityTypeId::Llama, level) {}

        // MC Llama.didSpit — read/consumed by LlamaHurtByTargetGoal.
        bool DidSpit() const { return m_didSpit; }
        void SetDidSpit(bool v) { m_didSpit = v; }

        // MC Llama.performRangedAttack — spit(target).
        void PerformRangedAttack(LivingEntity& target, float power) override;

    protected:
        // The variant constructor — TraderLlama is a llama of a different
        // type id, exactly as MC's extends.
        Llama(EntityTypeId type, EntityLevel* level);

        // MC Llama.spit — a LlamaSpit from just ahead of the mouth, aimed a
        // third up the target with the 0.2 loft, velocity 1.5, inaccuracy 10.
        void Spit(LivingEntity& target);

    private:
        bool m_didSpit = false;
    };

    // MC animal/equine/TraderLlama. Its one extra goal —
    // TraderLlamaDefendWanderingTraderGoal — needs the wandering trader
    // leash/ownership link and is skipped with it.
    class TraderLlama : public Llama {
    public:
        explicit TraderLlama(EntityLevel* level)
            : Llama(EntityTypeId::TraderLlama, level) {}
    };

    // MC animal/fox/Fox. MAX_HEALTH 10, MOVEMENT_SPEED 0.3, ATTACK_DAMAGE 2,
    // SAFE_FALL_DISTANCE 5, FOLLOW_RANGE 32.
    //
    // The parts that make a fox a fox here: the seven DATA_FLAGS bits
    // (sitting / crouching / interested / pouncing / sleeping / faceplanted /
    // defending — MC's bit values kept, and the whole byte IS the wire's anim
    // state byte), the stalk → full-crouch → pounce arc with the snow
    // faceplant, the day-sleep schedule with its alertable-entity sensor, the
    // variant-ordered prey target goals, and the red/snow biome variant on
    // the wire's variant byte. Not modelled, each named at its site: the
    // whole mouth-item layer (pickup, eating, spit, equipment rolls — no mob
    // item system), trust (rides the item layer; DefendTrustedTargetGoal and
    // the avoid-player trust exemption are inert with it), villages
    // (FoxStrollThroughVillageGoal, SeekShelterGoal's isVillage term), berry
    // bushes (FoxEatBerriesGoal needs block-state AGE), and sounds.
    class Fox : public Animal {
    public:
        // MC Fox.Variant.
        enum class Variant : uint8_t { Red = 0, Snow = 1 };

        explicit Fox(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ItemTags.FOX_FOOD: sweet berries, glow berries.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // ── MC DATA_FLAGS_ID, bit values verbatim ──────────────────────────
        bool IsSitting()     const { return GetFlag(0x01); }
        bool IsFoxCrouching() const { return GetFlag(0x04); }
        bool IsInterested()  const { return GetFlag(0x08); }
        bool IsPouncing()    const { return GetFlag(0x10); }
        bool IsSleeping()    const { return GetFlag(0x20); }
        bool IsFaceplanted() const { return GetFlag(0x40); }
        bool IsDefending()   const { return GetFlag(0x80); }

        void SetSitting(bool v)      { SetFlag(0x01, v); }
        void SetIsCrouching(bool v)  { SetFlag(0x04, v); }
        void SetIsInterested(bool v) { SetFlag(0x08, v); }
        void SetIsPouncing(bool v)   { SetFlag(0x10, v); }
        void SetSleeping(bool v)     { SetFlag(0x20, v); }
        void SetFaceplanted(bool v)  { SetFlag(0x40, v); }
        void SetDefending(bool v)    { SetFlag(0x80, v); }

        // The flag byte IS the anim state byte — one byte on the wire, the
        // meaning private to this class on both sides (the Bat pattern).
        uint8_t GetAnimStateByte() const override { return m_flags; }
        void    SetAnimStateByte(uint8_t v) override { m_flags = v; }

        // The variant byte carries MC's DATA_TYPE_ID (0 red, 1 snow). The
        // textures exist (assets/textures/entity/fox/); the renderer's
        // per-type texture table does not switch on the variant byte yet, so
        // every fox draws red until it does.
        Variant GetVariant() const { return m_variant; }
        void    SetVariant(Variant v) { m_variant = v; }
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_variant); }
        void    SetVariantByte(uint8_t v) override {
            m_variant = v == 1 ? Variant::Snow : Variant::Red;
        }

        // ── MC Fox state helpers, verbatim ─────────────────────────────────
        bool CanMove() const {
            return !IsSleeping() && !IsSitting() && !IsFaceplanted();
        }
        void WakeUp() { SetSleeping(false); }
        void ClearStates();

        // MC Fox.isFullyCrouched — crouchAmount saturates at 3.0.
        bool IsFullyCrouched() const { return m_crouchAmount == 3.0f; }
        // MC FoxPounceGoal.stop zeroes both crouch fields directly.
        void ResetCrouchAmount() { m_crouchAmount = 0.0f; m_crouchAmountO = 0.0f; }

        // MC Fox.getHeadRollAngle / getCrouchAmount — renderer inputs.
        float GetHeadRollAngle(float partialTick) const;
        float GetCrouchAmount(float partialTick) const;

        // MC Fox.isPathClear — the pounce arc test: 6 sample columns toward
        // the target must be air for 3 blocks above the fox's height.
        static bool IsPathClear(const Fox& fox, const LivingEntity& target);

        // MC Fox.setTarget — dropping the target drops the defence.
        void SetTarget(LivingEntity* target) override;

        // MC Fox.tick — wake-up conditions, the faceplant particle roll
        // (skipped: no block-crack particle path), and both client-visible
        // ramps (interestedAngle, crouchAmount).
        void Tick() override;

        // MC Fox.aiStep — the mouth-item eating half is skipped with the item
        // system; the target-loss state clear and the sleeping input freeze
        // are kept.
        void AiStep() override;

        // MC Fox.finalizeSpawn: variant by biome, pack members beyond the
        // second spawn as cubs (FoxGroupData), and the variant-ordered
        // target goals.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

    protected:
        void RegisterGoals() override;

    private:
        bool GetFlag(uint8_t bit) const { return (m_flags & bit) != 0; }
        void SetFlag(uint8_t bit, bool v) {
            m_flags = v ? static_cast<uint8_t>(m_flags | bit)
                        : static_cast<uint8_t>(m_flags & ~bit);
        }
        void SetTargetGoals();

        uint8_t m_flags = 0;
        Variant m_variant = Variant::Red;
        float   m_interestedAngle = 0.0f, m_interestedAngleO = 0.0f;
        float   m_crouchAmount = 0.0f, m_crouchAmountO = 0.0f;
        bool    m_targetGoalsSet = false;
    };

    // MC animal/turtle/Turtle. MAX_HEALTH 30, MOVEMENT_SPEED 0.25,
    // STEP_HEIGHT 1.
    //
    // The parts that make a turtle a turtle here: the home-beach position set
    // at spawn, the egg cycle (breeding sets HAS_EGG on the goal's turtle;
    // TurtleGoHomeGoal carries her back; TurtleLayEggGoal digs on sand for
    // ~200 ticks with LAYING_EGG driving the dig pose, then places the
    // turtle_egg block), the water-biased travel goals, the amphibious
    // TurtleMoveControl, and both synced booleans on the wire's anim byte.
    // Not modelled, each named at its site: scute drops on growing up (loot
    // tables handle death only), the lightning insta-kill, the per-state
    // sounds, and the 0.3 baby scale (the renderer's baby scale is global).
    class Turtle : public Animal {
    public:
        explicit Turtle(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ItemTags.TURTLE_FOOD: seagrass. A block item, resolved by slug.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // ── MC's two synced booleans, on the anim byte ─────────────────────
        bool HasEgg() const { return m_hasEgg; }
        void SetHasEgg(bool v) { m_hasEgg = v; }
        bool IsLayingEgg() const { return m_layingEgg; }
        // MC Turtle.setLayingEgg also arms/clears layEggCounter.
        void SetLayingEgg(bool v) { m_layingEgg = v; m_layEggCounter = v ? 1 : 0; }

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_hasEgg ? 1 : 0) | (m_layingEgg ? 2 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_hasEgg = (v & 1) != 0;
            m_layingEgg = (v & 2) != 0;
        }

        // ── Home / travel state the goals share ────────────────────────────
        const glm::ivec3& HomePos() const { return m_homePos; }
        void SetHomePos(const glm::ivec3& pos) { m_homePos = pos; }
        bool IsGoingHome() const { return m_goingHome; }
        void SetGoingHome(bool v) { m_goingHome = v; }
        const std::optional<glm::ivec3>& TravelPos() const { return m_travelPos; }
        void SetTravelPos(std::optional<glm::ivec3> pos) { m_travelPos = std::move(pos); }

        int  GetLayEggCounter() const { return m_layEggCounter; }
        void IncrementLayEggCounter() { ++m_layEggCounter; }

        // MC Turtle.getWalkTargetValue: water scores 10 unless heading home;
        // sand scores 10; everything else the light cost.
        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        // MC Turtle.canFallInLove: not while carrying an egg — encoded in
        // CanMate since the port's love entry points do not consult a
        // canFallInLove hook.
        bool CanMate(const Animal& other) const override;

        // MC TurtleBreedGoal.breed: no baby — the goal's turtle gets the egg,
        // both parents cool down. (The XP orb and BRED_ANIMALS trigger ride
        // systems that do not exist.)
        void SpawnChildFromBreeding(Animal& partner) override;

        // MC Turtle.finalizeSpawn: home is where you hatched.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Turtle.getAmbientSoundInterval: 200.
        int GetAmbientSoundInterval() const override { return 200; }

        // MC TurtleEggBlock.isSand — BlockTags.SAND.
        static bool IsSandBlock(BlockID id);

        // MC Turtle.BABY_ON_LAND_SELECTOR, shared by fox/cat/ocelot targets.
        static bool IsBabyOnLand(const LivingEntity& e);

    protected:
        void RegisterGoals() override;

    private:
        glm::ivec3 m_homePos{0};
        std::optional<glm::ivec3> m_travelPos;
        bool m_goingHome = false;
        bool m_hasEgg = false;
        bool m_layingEgg = false;
        int  m_layEggCounter = 0;
    };

    // MC animal/panda/Panda. MOVEMENT_SPEED 0.15, ATTACK_DAMAGE 6 (weak gene
    // drops MAX_HEALTH to 10, lazy drops speed to 0.07).
    //
    // The parts that make a panda a panda here: the main+hidden gene pair
    // with MC's inheritance and mutation rolls, every personality goal that
    // does not need items (worried flees and sits out thunderstorms, lazy
    // lies on its back, weak babies sneeze, playful and baby pandas roll,
    // aggressive pandas hold their grudge while the rest shake it), the
    // sit/on-back/roll render ramps, and the unhappy counter. The EFFECTIVE
    // gene rides the wire's variant byte so the renderer can pick the gene
    // texture (all seven exist in assets/textures/entity/panda/; the
    // renderer's per-type texture table does not switch on it yet). Not
    // modelled, each named at its site: everything riding mob-held items
    // (PandaSitGoal's eat-what-you-hold loop, eating particles/sounds,
    // pickUpItem), and the sneeze slime-ball gift drop.
    class Panda : public Animal {
    public:
        // MC Panda.Gene — ids verbatim; BROWN and WEAK are recessive.
        enum class Gene : uint8_t {
            Normal = 0, Lazy, Worried, Playful, Brown, Weak, Aggressive,
        };
        static bool IsRecessive(Gene g) {
            return g == Gene::Brown || g == Gene::Weak;
        }
        // MC Panda.Gene.getRandom — 1/16 lazy, worried, playful; 1/16
        // aggressive; 4/16 weak; 2/16 brown; 5/16 normal.
        static Gene RandomGene(class JavaRandom& rng);
        // MC Gene.getVariantFromGenes — a recessive main gene only shows
        // when the hidden gene matches.
        static Gene EffectiveGene(Gene main, Gene hidden) {
            if (IsRecessive(main)) return main == hidden ? main : Gene::Normal;
            return main;
        }

        explicit Panda(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ItemTags.PANDA_FOOD: bamboo. A block item, resolved by slug.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        Gene GetMainGene() const { return m_mainGene; }
        Gene GetHiddenGene() const { return m_hiddenGene; }
        void SetMainGene(Gene g) { m_mainGene = g; }
        void SetHiddenGene(Gene g) { m_hiddenGene = g; }
        Gene GetEffectiveGene() const { return EffectiveGene(m_mainGene, m_hiddenGene); }

        bool IsLazy()    const { return GetEffectiveGene() == Gene::Lazy; }
        bool IsWorried() const { return GetEffectiveGene() == Gene::Worried; }
        bool IsPlayful() const { return GetEffectiveGene() == Gene::Playful; }
        bool IsBrown()   const { return GetEffectiveGene() == Gene::Brown; }
        bool IsWeak()    const { return GetEffectiveGene() == Gene::Weak; }
        // MC Panda.isAggressive — the GENE, distinct from Mob's synced
        // aggressive flag, hence the name.
        bool IsAggressiveGene() const { return GetEffectiveGene() == Gene::Aggressive; }

        // ── MC DATA_ID_FLAGS bits (2/4/8/16 verbatim) + this port's extras ──
        bool IsSneezing() const { return GetFlag(0x02); }
        bool IsRolling()  const { return GetFlag(0x04); }
        bool IsSitting()  const { return GetFlag(0x08); }
        bool IsOnBack()   const { return GetFlag(0x10); }
        void Sneeze(bool v) { SetFlag(0x02, v); if (!v) m_sneezeCounter = 0; }
        void Roll(bool v)   { SetFlag(0x04, v); }
        void Sit(bool v)    { SetFlag(0x08, v); }
        void SetOnBack(bool v) { SetFlag(0x10, v); }

        // The anim byte: MC's four flag bits in MC's positions, plus bit 0
        // for unhappy (MC syncs UNHAPPY_COUNTER as an int; the pose only
        // needs the boolean) and bit 5 for scared (worried + thunder — the
        // client's level cannot answer IsThundering, so the server's verdict
        // rides the byte). Meaning private to this class on both sides.
        uint8_t GetAnimStateByte() const override {
            uint8_t b = static_cast<uint8_t>(m_flags & 0x1E);
            if (m_unhappyCounter > 0) b |= 0x01;
            if (IsScared()) b |= 0x20;
            return b;
        }
        void SetAnimStateByte(uint8_t v) override {
            m_flags = static_cast<uint8_t>(v & 0x1E);
            // Client-side stand-ins for the two derived bits: the unhappy
            // head-shake pose and the scared sit both read these directly.
            m_clientUnhappy = (v & 0x01) != 0;
            m_clientScared = (v & 0x20) != 0;
        }

        // The variant byte carries the EFFECTIVE gene (what the renderer
        // would pick a texture by); genes themselves are server-side.
        uint8_t GetVariantByte() const override {
            return static_cast<uint8_t>(GetEffectiveGene());
        }
        void SetVariantByte(uint8_t v) override {
            m_mainGene = v <= 6 ? static_cast<Gene>(v) : Gene::Normal;
            m_hiddenGene = m_mainGene;
        }

        int  GetUnhappyCounter() const { return m_unhappyCounter; }
        void SetUnhappyCounter(int v) { m_unhappyCounter = v; }
        bool IsUnhappy() const {
            return m_unhappyCounter > 0 || m_clientUnhappy;
        }

        int  GetSneezeCounter() const { return m_sneezeCounter; }

        // MC Panda.isScared — worried gene in a thunderstorm. The client
        // reads the synced bit (its level always answers "not thundering").
        bool IsScared() const;

        // MC Panda.isEating — gated on the mouth item, which the item layer
        // does not provide; stays false and the renderer field with it.
        bool IsEatingPanda() const { return false; }

        // MC Panda.canPerformAction.
        bool CanPerformAction() const {
            return !IsOnBack() && !IsScared() && !IsEatingPanda() && !IsRolling()
                && !IsSitting();
        }

        // MC Panda.tryToSit — used by the worried thunder path.
        void TryToSit();

        // MC Panda.mobInteract — bamboo feeding (the item exists as a block
        // item): stand a rolled-over panda up, age a cub, court an adult, and
        // sit a fed one down. The mouth-item/eat half of the else-branch is
        // skipped at its site (no mob-held-item system).
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC render-state ramps (updateSitAmount & friends run both sides).
        float GetSitAmount(float partialTick) const;
        float GetLieOnBackAmount(float partialTick) const;
        float GetRollAmount(float partialTick) const;

        // MC Panda.doHurtTarget — a non-aggressive panda remembers it bit
        // back (didBite) and its grudge goal stands down.
        bool DoHurtTarget(Entity& target) override;
        bool DidBite() const { return m_didBite; }
        // MC Panda.gotBamboo — set by feeding an angry panda; the grudge
        // goal stands down on it, exactly like didBite.
        bool GotBamboo() const { return m_gotBamboo; }

        // MC Panda.hurtServer — a hit panda stops sitting.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Panda.tick — worried thunder sit, the unhappy countdown, the
        // sneeze clock, the roll driver and all three ramps.
        void Tick() override;

        // MC Panda.setAttributes — the weak/lazy stat penalties.
        void ApplyGeneAttributes();

        // MC Panda.setGeneFromParents, verbatim including the 1/32 mutations.
        void SetGeneFromParents(const Panda& parent1, const Panda* parent2);

        // MC Panda.finalizeSpawn: both genes rolled, stats applied, 20% of a
        // pack spawns as cubs (AgeableMobGroupData(0.2)).
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // Breeding passes both parents' genes — the base path only knows one
        // parent, so the override threads the partner through.
        void SpawnChildFromBreeding(Animal& partner) override;

        // Exposed for PandaRollGoal / the roll driver.
        int GetRollCounter() const { return m_rollCounter; }

        // MC Panda.lookAtPlayerGoal — kept as a raw pointer (the selector
        // owns the goal) so PandaBreedGoal can point an unhappy panda's
        // glare at the nearest player, exactly as MC does.
        class PandaLookAtPlayerGoal* lookAtPlayerGoal = nullptr;

    protected:
        void RegisterGoals() override;

    private:
        bool GetFlag(uint8_t bit) const { return (m_flags & bit) != 0; }
        void SetFlag(uint8_t bit, bool v) {
            m_flags = v ? static_cast<uint8_t>(m_flags | bit)
                        : static_cast<uint8_t>(m_flags & ~bit);
        }
        void HandleRoll();
        void UpdateRamps();

        Gene m_mainGene = Gene::Normal;
        Gene m_hiddenGene = Gene::Normal;
        uint8_t m_flags = 0;
        int  m_unhappyCounter = 0;
        int  m_sneezeCounter = 0;
        int  m_rollCounter = 0;
        glm::dvec3 m_rollDelta{0.0};
        bool m_didBite = false;
        bool m_gotBamboo = false;
        bool m_clientUnhappy = false;
        bool m_clientScared = false;
        float m_sitAmount = 0.0f, m_sitAmountO = 0.0f;
        float m_onBackAmount = 0.0f, m_onBackAmountO = 0.0f;
        float m_rollAmount = 0.0f, m_rollAmountO = 0.0f;
    };

    // MC animal/feline/Ocelot. MAX_HEALTH 10, MOVEMENT_SPEED 0.3,
    // ATTACK_DAMAGE 3.
    //
    // The parts that make an ocelot an ocelot here: the three-speed stalk
    // (OcelotAttackGoal's 0.6 creep / 0.8 walk / 1.33 sprint, surfaced as
    // the CROUCHING pose + sprint flag by customServerAiStep), fleeing
    // players, hunting chickens and beached baby turtles, and the 2400-tick
    // despawn grace — and, the interaction system landed, trusting: fed fish
    // roll 1/3 setTrusting, and a trusting ocelot stops fleeing players
    // (reassessTrustingGoals) and never scares off the tempt. Sounds still
    // wait on the sound system.
    class Ocelot : public Animal {
    public:
        explicit Ocelot(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ItemTags.OCELOT_FOOD: raw cod, raw salmon.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // MC Ocelot's synced DATA_TRUSTING. Server-side only here: nothing
        // the client renders keys on trust (a trusting ocelot looks the
        // same in MC), so the boolean does not spend a wire byte.
        bool IsTrusting() const { return m_trusting; }
        void SetTrusting(bool trusting);

        // MC Ocelot.mobInteract — feeding fish at close range rolls trust.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Ocelot.handleEntityEvent: 41 = trust gained (7 hearts),
        // 40 = the roll failed (7 smoke).
        void HandleEntityEvent(uint8_t id) override;

        // MC Ocelot.removeWhenFarAway: !trusting && tickCount > 2400.
        bool RemoveWhenFarAway(double) const override {
            return !m_trusting && tickCount > 2400;
        }

        // MC Ocelot.getAmbientSoundInterval: 900.
        int GetAmbientSoundInterval() const override { return 900; }

        // MC Ocelot.customServerAiStep — surface the move-control speed as
        // the crouch pose / sprint flag the renderer reads.
        void CustomServerAiStep() override;

    protected:
        void RegisterGoals() override;

    private:
        // MC Ocelot.reassessTrustingGoals — an untrusting ocelot carries the
        // avoid-players goal, a trusting one does not.
        void ReassessTrustingGoals();

        bool m_trusting = false;
        // Owned by the goal selector; tracked so ReassessTrustingGoals can
        // remove it (MC keeps the same field).
        class OcelotAvoidEntityGoal* m_ocelotAvoidPlayersGoal = nullptr;
    };

    // MC animal/feline/Cat. MAX_HEALTH 10, MOVEMENT_SPEED 0.3,
    // ATTACK_DAMAGE 3.
    //
    // Taming landed (TamableAnimal mixin): fish-taming (1/3), sit-on-command,
    // follow-owner, tame-gated breeding, and reassessTameGoals swapping the
    // wild avoid-players goal out on tame. Still bed-gated at their sites:
    // CatRelaxOnOwnerGoal (the lieDown/relax ramp writers — the ramps tick
    // exactly as MC ticks them and idle at 0), CatLieOnBedGoal,
    // CatSitOnBlockGoal, and the morning gift (loot + sleep). The variant
    // byte carries the 11-texture variant id (all textures exist in
    // assets/textures/entity/cat/; the renderer's per-type texture table
    // does not switch on it yet — the collar layer with it).
    class Cat : public Animal, public TamableAnimal {
    public:
        static constexpr int kVariantCount = 11;

        explicit Cat(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ItemTags.CAT_FOOD: raw cod, raw salmon.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // MC Cat.canMate: both cats tame + the base love test.
        bool CanMate(const Animal& other) const override;

        // MC Cat.mobInteract — feed/sit-toggle when owned, fish-taming when
        // wild (the dye collar branch is skipped with the collar layer).
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Cat.setTame → reassessTameGoals. The hook rides
        // applyTamingSideEffects — the only live SetTame paths here (taming,
        // and the pup copy) pass includeSideEffects.
        void ApplyTamingSideEffects() override { ReassessTameGoals(); }

        // MC TamableAnimal.canAttack — never the owner.
        bool CanAttack(const LivingEntity& target) const override {
            return TamableCanAttack(target) && Animal::CanAttack(target);
        }

        // MC TamableAnimal.handleEntityEvent: 7 = taming hearts, 6 = taming
        // smoke; everything else to Animal.
        void HandleEntityEvent(uint8_t id) override {
            if (!HandleTamableEntityEvent(id)) Animal::HandleEntityEvent(id);
        }

        // The tamable byte (bit 0 sitting pose, bit 1 tame) IS the cat's
        // anim state byte — its first user.
        uint8_t GetAnimStateByte() const override { return GetTamableAnimByte(); }
        void    SetAnimStateByte(uint8_t v) override { SetTamableAnimByte(v); }

        // MC Cat.removeWhenFarAway: !tame && tickCount > 2400.
        bool RemoveWhenFarAway(double) const override {
            return !IsTame() && tickCount > 2400;
        }

        uint8_t GetVariantByte() const override { return m_variant; }
        void    SetVariantByte(uint8_t v) override {
            m_variant = v < kVariantCount ? v : 0;
        }

        // MC's IS_LYING / RELAX_STATE_ONE — only CatRelaxOnOwnerGoal writes
        // them and it is tame-gated, so they stay false; kept (with their
        // ramps) so the renderer wiring is real the day taming lands.
        bool IsLying() const { return m_lying; }
        bool IsRelaxStateOne() const { return m_relaxStateOne; }

        float GetLieDownAmount(float partialTick) const;
        float GetLieDownAmountTail(float partialTick) const;
        float GetRelaxStateOneAmount(float partialTick) const;

        // MC Cat.tick — handleLieDown's ramps (the purr and the
        // lying-on-player scan wait on sounds/beds).
        void Tick() override;

        // MC Cat.customServerAiStep — same pose surface as the ocelot.
        void CustomServerAiStep() override;

        // MC CatVariants — 11 variants; MC picks by structure/moon-phase
        // weights (black cats in witch huts, all_black on full moons). No
        // structure or moon-phase context exists, so the roll is uniform.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

    protected:
        void RegisterGoals() override;

    private:
        // MC Cat.reassessTameGoals — a wild cat carries the avoid-players
        // goal, a tame one does not. Called from RegisterGoals (initial
        // state) and ApplyTamingSideEffects (tame flips).
        void ReassessTameGoals();

        // MC Cat.tryToTame — the 1/3 fish roll.
        void TryToTame(LivingEntity& player);

        uint8_t m_variant = 0;
        // Owned by the goal selector; tracked so ReassessTameGoals can
        // remove it (MC keeps the same field).
        class CatAvoidEntityGoal* m_avoidPlayersGoal = nullptr;
        bool m_lying = false;
        bool m_relaxStateOne = false;
        float m_lieDownAmount = 0.0f, m_lieDownAmountO = 0.0f;
        float m_lieDownAmountTail = 0.0f, m_lieDownAmountOTail = 0.0f;
        float m_relaxStateOneAmount = 0.0f, m_relaxStateOneAmountO = 0.0f;
    };

    // MC animal/equine/AbstractHorse — the shared equine base, promoted for
    // the stand/eat/tail animation machinery: the EATING and STANDING flags
    // (synced on the anim byte), the eatAnim/standAnim ramps ticked in
    // tick() on both sides exactly as MC writes them, the 1-in-200 tail
    // swish, the grass-eating roll, RandomStandGoal, and MC's goal table at
    // MC's priorities. Built on GenericAnimal so the def's attributes
    // (JUMP_STRENGTH and friends) still apply. Not modelled, each named at
    // its site: taming/riding (MountPanicGoal's rider branch,
    // RunAroundLikeCrazyGoal, temper, saddles, inventories — PanicGoal
    // stands in for MountPanicGoal's panic half), the open-mouth flag (its
    // only writers are eating-from-hand and rider interactions), and the
    // per-breed variant rolls (the wire byte exists; the renderer draws one
    // texture per type).
    class AbstractHorse : public GenericAnimal {
    public:
        AbstractHorse(EntityTypeId type, EntityLevel* level);

        // ── MC DATA_ID_FLAGS: FLAG_EATING 16, FLAG_STANDING 32 ─────────────
        bool IsEating()   const { return m_eating; }
        bool IsStanding() const { return m_standing; }
        void SetEating(bool v) { m_eating = v; }

        // MC AbstractHorse.setStanding(ticks) / clearStanding.
        void SetStanding(int ticks) {
            SetEating(false);
            m_standing = true;
            m_standCounter = ticks;
        }
        void ClearStanding() { m_standing = false; m_standCounter = 0; }

        // MC AbstractHorse.standIfPossible — rear for 20 ticks.
        void StandIfPossible() {
            if (CanPerformRearing() && IsEffectiveAi()) SetStanding(20);
        }

        // MC AbstractHorse.canPerformRearing — true for the whole family
        // except the llama (a separate class here).
        virtual bool CanPerformRearing() const { return true; }

        // MC AbstractHorse.getAmbientSoundInterval: 400 (Animal's is 120).
        int GetAmbientSoundInterval() const override { return 400; }

        // MC AbstractHorse.getMaxSpawnClusterSize (AbstractHorse.java:373-375)
        // — herds of 6, for the whole family (horse, donkey, mule, skeleton
        // and zombie horse).
        int GetMaxSpawnClusterSize() const override { return 6; }

        // MC AbstractHorse.getAmbientStandInterval — the ambient interval.
        int GetAmbientStandInterval() const { return GetAmbientSoundInterval(); }

        // Anim byte: bit 0 eating, bit 1 standing.
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_eating ? 1 : 0) | (m_standing ? 2 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_eating = (v & 1) != 0;
            m_standing = (v & 2) != 0;
        }

        // MC AbstractHorse.hurtServer: 1-in-3 hits make the horse rear.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC AbstractHorse.isImmobile — an eating or rearing horse plants
        // its feet (the isVehicle && isSaddled half rides the riding system).
        bool IsImmobile() const override {
            return Animal::IsImmobile() || IsEating() || IsStanding();
        }

        // MC AbstractHorse.tick — the counters and all three ramps (the
        // mouth ramp's writers are interaction-gated and skipped).
        void Tick() override;

        // MC AbstractHorse.aiStep — the tail roll (both sides, MC's own
        // arrangement: each side rolls its own 1-in-200) and the server's
        // grass-eating roll. (The 1-in-900 self-heal waits on nothing and is
        // kept; followMommy needs the bred flag, which taming sets.)
        void AiStep() override;

        // MC AbstractHorse.canEatGrass.
        virtual bool CanEatGrass() const { return true; }

        // ── Taming / temper (interaction wave) ─────────────────────────────
        // MC tames equines through riding attempts (temper vs a random roll
        // in RunAroundLikeCrazyGoal / tameWithName) — player mounting does
        // not exist, so ONLY the feeding half of temper is live: golden
        // apples/carrots and the rest of handleEating raise it, and the
        // stored value is ready the day riding lands. isTamed stays false
        // until then (nothing else can set it).
        bool IsTamedHorse() const { return m_tamedHorse; }
        void SetTamedHorse(bool v) { m_tamedHorse = v; }
        int  GetTemper() const { return m_temper; }
        void SetTemper(int temper) { m_temper = temper; }
        int  ModifyTemper(int amount);
        // MC AbstractHorse.getMaxTemper: 100.
        virtual int GetMaxTemper() const { return 100; }

        // MC Horse/AbstractChestedHorse.mobInteract's shared shape: food →
        // fedFood; a non-food click on an untamed horse → makeMad (the
        // mount-to-tame attempt is skipped with riding, commented in the
        // .cpp). SkeletonHorse overrides (untamed → Pass, per its source).
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC AbstractHorse.fedFood / handleEating — the per-item
        // heal/ageUp/temper table, verbatim.
        UseResult FedFood(LivingEntity& player, ItemStack& held);
        bool HandleEating(LivingEntity& player, const ItemStack& held);

        // MC AbstractHorse.makeMad — rear up (the angry sound waits on the
        // sound system).
        void MakeMad() {
            if (!IsStanding()) StandIfPossible();
        }

        // ── Renderer inputs (MC HorseRenderer/extractRenderState) ─────────
        float GetEatAnim(float partialTick) const;
        float GetStandAnim(float partialTick) const;
        bool  IsAnimatingTail() const { return m_tailCounter > 0; }

    private:
        void RegisterHorseGoals();

        // MC AbstractHorse.temper / tamed — see the taming block above.
        int  m_temper = 0;
        bool m_tamedHorse = false;

        bool m_eating = false;
        bool m_standing = false;
        int  m_standCounter = 0;
        int  m_eatingCounter = 0;
        int  m_tailCounter = 0;
        float m_eatAnim = 0.0f, m_eatAnimO = 0.0f;
        float m_standAnim = 0.0f, m_standAnimO = 0.0f;
    };

    // The concrete equines. Each is one MC class; the bespoke pieces beyond
    // the shared base (horse variants, chests, skeleton-trap, conversion)
    // ride systems named in the base comment. CreateBaby is per-type so a
    // foal is the promoted class, not a GenericAnimal.
    class Horse : public AbstractHorse {
    public:
        explicit Horse(EntityLevel* level)
            : AbstractHorse(EntityTypeId::Horse, level) {}
        std::unique_ptr<Animal> CreateBaby() override {
            return std::make_unique<Horse>(m_level);
        }
    };

    class Donkey : public AbstractHorse {
    public:
        explicit Donkey(EntityLevel* level)
            : AbstractHorse(EntityTypeId::Donkey, level) {}
        std::unique_ptr<Animal> CreateBaby() override {
            return std::make_unique<Donkey>(m_level);
        }
    };

    // MC mules are horse x donkey and infertile; same-species breeding is
    // the port's only pairing, so a mule simply cannot mate.
    class Mule : public AbstractHorse {
    public:
        explicit Mule(EntityLevel* level)
            : AbstractHorse(EntityTypeId::Mule, level) {}
        bool CanMate(const Animal&) const override { return false; }
        std::unique_ptr<Animal> CreateBaby() override { return nullptr; }
    };

    class SkeletonHorse : public AbstractHorse {
    public:
        explicit SkeletonHorse(EntityLevel* level)
            : AbstractHorse(EntityTypeId::SkeletonHorse, level) {}
        // MC SkeletonHorse.mobInteract: an untamed skeleton horse ignores
        // every click (no feeding, no mounting) — and untamed is the only
        // kind that exists until riding lands.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override {
            if (!IsTamedHorse()) return UseResult::Pass;
            return AbstractHorse::MobInteract(player, held);
        }
        std::unique_ptr<Animal> CreateBaby() override {
            return std::make_unique<SkeletonHorse>(m_level);
        }
    };

    class ZombieHorse : public AbstractHorse {
    public:
        explicit ZombieHorse(EntityLevel* level)
            : AbstractHorse(EntityTypeId::ZombieHorse, level) {}
        // MC ZombieHorse.removeWhenFarAway: true — the one equine that
        // despawns (it only exists via /summon or a rider).
        bool RemoveWhenFarAway(double) const override { return true; }
        std::unique_ptr<Animal> CreateBaby() override {
            return std::make_unique<ZombieHorse>(m_level);
        }
    };

} // namespace Game
