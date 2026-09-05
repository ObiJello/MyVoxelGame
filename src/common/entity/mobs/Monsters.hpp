// File: src/common/entity/mobs/Monsters.hpp
//
// Zombie, Skeleton, Creeper and Spider.
//
// Each is a transcription of its MC class's createAttributes() and
// registerGoals(). The PRIORITY NUMBERS are the behaviour — they decide what a
// mob does when two goals both want to move it — so they are reproduced exactly
// and should not be renumbered for tidiness.
#pragma once

#include "common/entity/Monster.hpp"
#include "common/entity/NeutralMob.hpp"
#include "common/entity/RangedAttackMob.hpp"
#include "common/entity/mobs/GenericMobs.hpp"

#include <algorithm>   // std::min in Wither::GetAnimStateByte (MSVC: no transitive pull)

namespace Game {

    class RandomStrollGoal;
    class BreakDoorGoal;

    // MC Zombie.ZombieGroupData — the pack token: the FIRST zombie of a pack
    // rolls the 5% baby chance and every later member reuses the answer, which
    // is why baby zombies arrive alone or as a whole baby pack, not mixed.
    struct ZombieGroupData : SpawnGroupData {
        ZombieGroupData(bool baby, bool jockey) : isBaby(baby), canSpawnJockey(jockey) {}
        bool isBaby = false;
        bool canSpawnJockey = true;
    };

    // MC Zombie. Attributes: FOLLOW_RANGE 35, MOVEMENT_SPEED 0.23,
    // ATTACK_DAMAGE 3, ARMOR 2.
    class Zombie : public Monster {
    public:
        explicit Zombie(EntityLevel* level);

        bool IsBaby() const override { return m_baby; }
        void SetBaby(bool baby) override;

        // MC Zombie.getBaseExperienceReward (Zombie.java:165-170): a baby is
        // worth xpReward * 2.5 — 12 for the 5 the type table carries. Husk,
        // Drowned, ZombieVillager and ZombifiedPiglin inherit, as in MC.
        int GetXpReward() const override {
            const int base = TypeInfo().xpReward;
            return m_baby ? static_cast<int>(base * 2.5) : base;
        }

        // MC Zombie.canBreakDoors — rolled at spawn (difficulty * 0.1, and
        // always true for leader zombies). Toggling it on adds BreakDoorGoal
        // at priority 1 and teaches the pathfinder to walk at closed wooden
        // doors (navigation.setCanOpenDoors), exactly MC's setCanBreakDoors.
        bool CanBreakDoors() const { return m_canBreakDoors; }
        void SetCanBreakDoors(bool v);

        // MC Zombie.finalizeSpawn: baby odds via the group token, jockey
        // rolls, canBreakDoors, then handleAttributes (knockback resistance,
        // the leader roll, the zombie-specific follow-range bonus).
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Zombie.hurtServer — a hurt zombie on HARD can summon a
        // reinforcement zombie nearby.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Zombie.doHurtTarget — an on-fire, empty-handed zombie passes the
        // fire to whatever it hits.
        bool DoHurtTarget(Entity& target) override;

        // MC Zombie.tick — the in-water conversion clock, then super.tick():
        // eyes underwater for 600 straight ticks starts a 300-tick conversion
        // to a drowned (a husk runs the same clock into a plain zombie).
        // MC syncs DATA_DROWNED_CONVERSION_ID so the client shakes the model;
        // the shaking visual has no renderer hook, so the state is
        // server-only.
        void Tick() override;

        bool IsUnderWaterConverting() const { return m_underWaterConverting; }

        // MC Zombie's "InWaterTime" and "DrownedConversionTime" save keys.
        // Both are real clocks, not derived state: a zombie 500 ticks into
        // its 600-tick submersion has to resume there, and one already
        // converting has to keep converting.
        int  GetInWaterTime() const { return m_inWaterTime; }
        void SetInWaterTime(int ticks) { m_inWaterTime = ticks; }
        // -1 is MC's NOT_CONVERTING; anything >= 0 arms the clock.
        int  GetDrownedConversionTime() const { return m_conversionTime; }
        void SetDrownedConversionTime(int ticks) {
            m_conversionTime = ticks;
            m_underWaterConverting = ticks >= 0;
        }

        static void CreateAttributes(AttributeMap& out);

    protected:
        // The variant constructor — Husk, Drowned, ZombieVillager and
        // ZombifiedPiglin are zombies of a different type id (MC reuses
        // Zombie.createAttributes for all of them).
        Zombie(EntityTypeId type, EntityLevel* level);

        void RegisterGoals() override;
        virtual void AddBehaviourGoals();
        void HandleAttributes(float difficultyModifier, SpawnReason reason);
        // MC Zombie.randomizeReinforcementsChance — base = nextDouble() * 0.1.
        // Virtual because ZombifiedPiglin pins it to 0: piglins never call
        // reinforcements.
        virtual void RandomizeReinforcementsChance();

        // MC EntityTypeTags.BURN_IN_DAYLIGHT. The burn itself lives on Mob.
        bool BurnsInDaylight() const override { return true; }

        // ── In-water conversion (MC Zombie.tick + convertToZombieType) ─────
        // MC Zombie.convertsInWater: true for zombie, husk and zombie
        // villager; false for drowned and zombified piglin.
        virtual bool ConvertsInWater() const { return true; }
        // MC Zombie.doUnderWaterConversion — which zombie type this becomes
        // (zombie → drowned; the husk overrides to → zombie). The level
        // events (1040/1041, the conversion sound) wait on the sound system.
        virtual void DoUnderWaterConversion();
        void StartUnderWaterConversion(int time) {
            m_conversionTime = time;
            m_underWaterConverting = true;
        }
        // MC Zombie.convertToZombieType: the family conversion — the generic
        // copy plus the zombie bits (baby, canBreakDoors) and the
        // afterConversion handleAttributes re-roll, MC's callback.
        void ConvertToZombieType(std::unique_ptr<Zombie> replacement);

    private:
        bool m_baby = false;
        bool m_canBreakDoors = false;
        // The registered goal while canBreakDoors is on (MC keeps one
        // instance from the constructor; recreating on toggle is the same
        // observable behaviour — start() resets all its state).
        BreakDoorGoal* m_breakDoorGoal = nullptr;
        // MC Zombie.inWaterTime / conversionTime / DATA_DROWNED_CONVERSION_ID.
        int  m_inWaterTime = 0;
        int  m_conversionTime = -1;   // MC NOT_CONVERTING
        bool m_underWaterConverting = false;
        // The accumulated caller-charge on SPAWN_REINFORCEMENTS (MC keeps it
        // as the modifier's amount; the attribute map has no read-back, so it
        // is mirrored here).
        double m_reinforcementCallerCharge = 0.0;
    };

    // MC Husk — a zombie that shrugs off daylight and inflicts HUNGER on hit.
    // Underwater it converts to a plain ZOMBIE (which then runs its own clock
    // into a drowned) — the base Zombie's conversion machinery with a
    // different destination.
    class Husk : public Zombie {
    public:
        explicit Husk(EntityLevel* level) : Zombie(EntityTypeId::Husk, level) {
            RegisterGoals();
        }

        // MC Husk.doHurtTarget — a landed empty-handed hit applies HUNGER for
        // 140 * (int)effectiveDifficulty ticks.
        bool DoHurtTarget(Entity& target) override;

    protected:
        bool BurnsInDaylight() const override { return false; }
        // MC Husk.convertsInWater: true (inherited), destination overridden.
        void DoUnderWaterConversion() override;
    };

    // MC ZombieVillager — a zombie in shape. convertsInWater is FALSE (a
    // zombie villager keeps its villager data underwater); the golden-apple
    // CURING flow back to a villager is item-interaction-gated and waits on
    // the item system.
    class ZombieVillager : public Zombie {
    public:
        explicit ZombieVillager(EntityLevel* level)
            : Zombie(EntityTypeId::ZombieVillager, level) {
            RegisterGoals();
        }

    protected:
        bool ConvertsInWater() const override { return false; }
    };

    // MC Drowned — the waterborne zombie: no daylight burn, water is free to
    // path through. (Its full day/night go-to-water goal set needs the
    // beach/sea-level system; the amphibious navigation is what makes a
    // drowned chase you INTO the water instead of pacing the shore.)
    //
    // Ranged: MC gives 6.25% of drowned a trident (populateDefaultEquipment-
    // Slots: 10% roll a weapon, 10-in-16 of those a trident) and gates
    // DrownedTridentAttackGoal on holding it. No mob equipment system exists,
    // so the roll lives in FinalizeSpawn as a flag — same odds, same
    // behaviour, no item.
    class Drowned : public Zombie, public RangedAttackMob {
    public:
        explicit Drowned(EntityLevel* level);

        bool HasTrident() const { return m_hasTrident; }

        // MC Drowned.performRangedAttack — a trident from eye height minus
        // 0.1, aimed a third up the target's box, velocity 1.6, inaccuracy
        // 14 - difficultyId * 4 (identical numbers to the skeleton's arrow).
        void PerformRangedAttack(LivingEntity& target, float power) override;

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

    protected:
        bool BurnsInDaylight() const override { return false; }
        bool ConvertsInWater() const override { return false; }
        void AddBehaviourGoals() override;

    private:
        bool m_hasTrident = false;
    };

    // MC ZombifiedPiglin — fire-immune, and NEUTRAL (implements NeutralMob):
    // it targets players only while ANGRY at them. A hit starts a 400..780
    // tick grudge; while it has a target the whole herd is alerted every
    // 80..120 ticks within follow-range (the group anger that makes hitting
    // one piglin a terrible idea), and anger adds a +0.05 speed modifier.
    class ZombifiedPiglin : public Zombie, public NeutralMob {
    public:
        explicit ZombifiedPiglin(EntityLevel* level);
        bool FireImmune() const override { return true; }

        // MC ZombifiedPiglin.setTarget — a fresh target arms the alert clock
        // (and MC's first-anger-sound delay, skipped with sounds).
        void SetTarget(LivingEntity* target) override;

        // MC PERSISTENT_ANGER_TIME = TimeUtil.rangeOfSeconds(20, 39).
        void StartPersistentAngerTimer() override;

        void ClearReferenceTo(const Entity* entity) override {
            Zombie::ClearReferenceTo(entity);
            ClearAngerReferenceTo(entity);
        }

    protected:
        bool BurnsInDaylight() const override { return false; }
        bool ConvertsInWater() const override { return false; }
        void AddBehaviourGoals() override;
        // MC ZombifiedPiglin.randomizeReinforcementsChance: setBaseValue(0) —
        // an angry herd is its own reinforcement.
        void RandomizeReinforcementsChance() override;

        // MC ZombifiedPiglin.customServerAiStep — angry speed modifier,
        // updatePersistentAnger(true), and the herd alert.
        void CustomServerAiStep() override;

    private:
        void MaybeAlertOthers();
        void AlertOthers();

        // MC ALERT_INTERVAL = TimeUtil.rangeOfSeconds(4, 6) → 80..120 ticks.
        int m_ticksUntilNextAlert = 0;
    };

    // MC Skeleton / AbstractSkeleton. MOVEMENT_SPEED 0.25; health and attack
    // damage stay at the Monster defaults (20 / 2).
    //
    // Ranged: the skeleton is a RangedAttackMob driven by RangedBowAttackGoal
    // (1.0, 20-or-40, 15.0F). MC's reassessWeaponGoal swaps between the bow
    // goal and MeleeAttackGoal(1.2, false) based on the held item; with no mob
    // equipment system the bow is permanent, which is also MC's steady state —
    // every naturally spawned skeleton holds one.
    //
    // MC Skeleton.doFreezeConversion (skeleton → STRAY after 140 ticks inside
    // powder snow) is SKIPPED: no powder snow block exists, so the freeze
    // clock that starts it can never run. The Mob::ConvertTo machinery it
    // would use is live — see Zombie's water conversion.
    class Arrow;

    class Skeleton : public Monster, public RangedAttackMob {
    public:
        explicit Skeleton(EntityLevel* level);

        // MC AbstractSkeleton.performRangedAttack: an arrow from eye height
        // minus 0.1, aimed at a third up the target's box, velocity 1.6,
        // inaccuracy 14 - difficultyId * 4.
        void PerformRangedAttack(LivingEntity& target, float power) override;

        static void CreateAttributes(AttributeMap& out);

        // MC AbstractSkeleton attack intervals: 20 on HARD, 40 otherwise.
        static constexpr int kHardAttackInterval   = 20;
        static constexpr int kNormalAttackInterval = 40;

    protected:
        // The variant constructor — Stray and Bogged are skeletons of a
        // different type id, exactly as MC's `extends AbstractSkeleton`.
        Skeleton(EntityTypeId type, EntityLevel* level);

        // MC AbstractSkeleton.getHardAttackInterval / getAttackInterval —
        // virtual so the Bogged can slow its bow to 50/70.
        virtual int GetHardAttackInterval() const { return kHardAttackInterval; }
        virtual int GetAttackInterval() const { return kNormalAttackInterval; }

        // MC AbstractSkeleton.getArrow — the hook the Stray uses to tip its
        // arrows. Called on the freshly built arrow before it is shot.
        virtual void CustomizeArrow(Arrow& arrow) { (void)arrow; }

        void RegisterGoals() override;
        bool BurnsInDaylight() const override { return true; }
    };

    // MC WitherSkeleton — promoted from the generic path for its melee wither
    // touch only: a landed hit applies WITHER for 200 ticks (10 s). The
    // def-driven GenericMonster goal set stands in for AbstractSkeleton's,
    // which is truthful here because a wither skeleton fights with its stone
    // sword — melee — not a bow (MC's reassessWeaponGoal lands on
    // MeleeAttackGoal for it). Fire immunity is EntityType.fireImmune in MC's
    // type builder.
    class WitherSkeleton : public GenericMonster {
    public:
        explicit WitherSkeleton(EntityLevel* level)
            : GenericMonster(EntityTypeId::WitherSkeleton, level) {}

        bool FireImmune() const override { return true; }

        // MC WitherSkeleton.doHurtTarget.
        bool DoHurtTarget(Entity& target) override;
    };

    // MC Stray — a skeleton whose arrows carry SLOWNESS. Attributes and goals
    // are the skeleton's own (MC adds nothing on top of AbstractSkeleton).
    // Its powder-snow freeze immunity (Stray.canFreeze == false via the
    // FREEZE_IMMUNE_ENTITY_TYPES tag) has no home yet — no powder snow or
    // freezing exists in this engine.
    class Stray : public Skeleton {
    public:
        explicit Stray(EntityLevel* level);

    protected:
        // MC Stray.getArrow: addEffect(new MobEffectInstance(SLOWNESS, 600)) —
        // 30 seconds, amplifier 0.
        void CustomizeArrow(Arrow& arrow) override;
    };

    // MC Bogged — the swamp skeleton: MAX_HEALTH 16, POISON-tipped arrows,
    // and a slower bow (50/70-tick intervals against the skeleton's 20/40).
    // Shearing its mushrooms off (mobInteract + Shearable) waits on the
    // shears-on-mob flow.
    class Bogged : public Skeleton {
    public:
        explicit Bogged(EntityLevel* level);

        // MC Bogged.createAttributes: AbstractSkeleton's + MAX_HEALTH 16.
        static void CreateAttributes(AttributeMap& out);

    protected:
        // MC Bogged.getArrow: POISON for 100 ticks (5 s), amplifier 0.
        void CustomizeArrow(Arrow& arrow) override;

        int GetHardAttackInterval() const override { return 50; }
        int GetAttackInterval() const override { return 70; }
    };

    // MC Creeper. MOVEMENT_SPEED 0.25; the fuse lives here rather than in
    // SwellGoal so that a creeper lit by other means still detonates.
    class Creeper : public Monster {
    public:
        explicit Creeper(EntityLevel* level);

        static constexpr int kMaxSwell = 30;
        static constexpr int kExplosionRadius = 3;

        int  GetSwellDir() const { return m_swellDir; }
        void SetSwellDir(int dir);

        // 0..1, for the renderer's flash-and-inflate. MC divides by
        // (maxSwell - 2) so the creeper reaches full white slightly BEFORE it
        // explodes, which is the visual tell players react to.
        float GetSwelling(float partialTick) const;

        bool IsIgnited() const { return m_ignited; }
        void Ignite() { m_ignited = true; }

        // MC Creeper.doHurtTarget returns true WITHOUT dealing damage —
        // creepers never melee, they only explode.
        bool DoHurtTarget(Entity& target) override { return true; }

        // MC Creeper.setTarget ignores goats (they ram creepers for sport;
        // retaliating would detonate the ram target).
        void SetTarget(LivingEntity* target) override;

        // MC Creeper.getMaxFallDistance: with a target it will drop until one
        // HP — falling onto the player is a valid attack plan.
        int GetMaxFallDistance() const override {
            return GetTarget() ? GetComfortableFallDistance(GetHealth() - 1.0f)
                               : GetComfortableFallDistance(0.0f);
        }

        // MC Creeper.causeFallDamage: a hard landing advances the fuse by
        // 1.5x the fall distance, capped 5 ticks short of detonation.
        bool CauseFallDamage(double fallDist, float damageMultiplier) override {
            const bool damaged = Monster::CauseFallDamage(fallDist, damageMultiplier);
            m_swell += static_cast<int>(fallDist * 1.5);
            if (m_swell > kMaxSwell - 5) m_swell = kMaxSwell - 5;
            return damaged;
        }

        void Tick() override;

        // The explosion visual: MC carries it in ClientboundExplodePacket;
        // this port broadcasts kEntityEventExplosionEmitter instead (see
        // EntityLevel.hpp) and answers it here client-side.
        void HandleEntityEvent(uint8_t id) override;

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;

    private:
        void Explode();
        // MC Creeper.spawnLingeringCloud — the death cloud that carries any
        // active effects the creeper had. Empty-effect creepers (the usual
        // case) spawn nothing.
        void SpawnLingeringCloud();

        int  m_swellDir = -1;
        int  m_swell = 0;
        int  m_oldSwell = 0;
        bool m_ignited = false;
    };

    // MC EnderMan. MAX_HEALTH 40, MOVEMENT_SPEED 0.3, ATTACK_DAMAGE 7,
    // FOLLOW_RANGE 64, STEP_HEIGHT 1.0.
    //
    // The parts that matter for endermen being endermen: the stare-aggro
    // machine (EndermanGoals.hpp), teleporting — away when stared at up
    // close, toward a distant target, on projectile hits, out of daylight and
    // rain — water sensitivity, block carrying, and the persistent-anger
    // system (NeutralMob: isAngryAt feeds the look-for-player goal alongside
    // the stare, and ResetUniversalAngerTargetGoal sits at target priority
    // 4). Not modelled, each named at its site: thrown potions, the carved-
    // pumpkin disguise, and syncing the carried block to the client (the wire
    // carries one variant byte; a BlockID needs sixteen).
    class Enderman : public Monster, public NeutralMob {
    public:
        explicit Enderman(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        BlockID GetCarriedBlock() const { return m_carriedBlock; }
        void    SetCarriedBlock(BlockID block) { m_carriedBlock = block; }

        // MC requiresCustomPersistence: a carrying enderman never despawns —
        // otherwise the block it stole would vanish with it.
        bool RequiresCustomPersistence() const override {
            return m_carriedBlock != BlockID::Air;
        }

        // MC isBeingStaredBy: the player's view ray within a distance-scaled
        // 0.025 tolerance of the enderman's eyes, with line of sight. (The
        // carved-pumpkin exemption needs player equipment.)
        bool IsBeingStaredBy(LivingEntity& player);

        // MC teleport(): up to +-32 blocks each axis, landing on the first
        // motion-blocking, dry surface below the roll.
        bool Teleport();
        // MC teleportTowards: 16 blocks along the direction TO the entity.
        bool TeleportTowards(const Entity& target);

        // MC hurtServer: projectiles never damage — 64 teleport attempts
        // instead; environmental damage teleports 90% of the time.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC setTarget also drives the creepy flag (mapped onto the wire's
        // aggressive bit — it is what the renderer's angry face reads).
        void SetTarget(LivingEntity* target) override;

        void AiStep() override;

        void DropCustomDeathLoot(EntityLevel& level) override;

        // MC EnderMan.startPersistentAngerTimer — rangeOfSeconds(20, 39).
        void StartPersistentAngerTimer() override;

        void ClearReferenceTo(const Entity* entity) override {
            Monster::ClearReferenceTo(entity);
            ClearAngerReferenceTo(entity);
        }

        static constexpr int kMinDeaggressionTime = 600;

    protected:
        void RegisterGoals() override;
        void CustomServerAiStep() override;

    private:
        bool TeleportTo(double x, double y, double z);

        BlockID m_carriedBlock = BlockID::Air;
        int     m_targetChangeTime = 0;
    };

    // MC Spider. MAX_HEALTH 16, MOVEMENT_SPEED 0.3.
    class Spider : public Monster {
    public:
        explicit Spider(EntityLevel* level);

        // MC Spider.onClimbable — spiders climb walls by treating any
        // horizontal collision as a ladder.
        bool IsClimbing() const { return m_climbing; }

        void Tick() override;
        void Travel(const glm::dvec3& input) override;

        // MC Spider.finalizeSpawn: the 1-in-100 skeleton jockey and the HARD
        // pack-effect roll — 10%-of-difficulty odds that the whole pack shares
        // one PERMANENT effect (speed / strength / regeneration / invisibility).
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC SpiderTargetGoal / SpiderAttackGoal: a spider is passive in
        // bright light (magic value >= 0.5, i.e. brightness >= 13). Both the
        // acquire and the keep tests consult this; public because the local
        // attack-goal subclass in the .cpp does too.
        static bool IsDarkEnoughToHunt(Mob& mob);

        static void CreateAttributes(AttributeMap& out);

    protected:
        // The variant constructor — CaveSpider is a spider of a different
        // type id, exactly as MC's `CaveSpider extends Spider`.
        Spider(EntityTypeId type, EntityLevel* level);

        void RegisterGoals() override;

    private:
        bool m_climbing = false;
    };

    // MC CaveSpider — the spider's attributes at MAX_HEALTH 12, plus the
    // poison bite: a landed hit applies POISON for 7s on NORMAL / 15s on HARD
    // (nothing on EASY). Its finalizeSpawn override drops the parent's jockey
    // and pack-effect rolls entirely, as MC's does.
    class CaveSpider : public Spider {
    public:
        explicit CaveSpider(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC CaveSpider.doHurtTarget.
        bool DoHurtTarget(Entity& target) override;

        // MC CaveSpider.finalizeSpawn: returns groupData untouched — no
        // jockey, no group effect, and none of Mob.finalizeSpawn's rolls.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override {
            return groupData;
        }
    };

    // MC Guardian. ATTACK_DAMAGE 6, MOVEMENT_SPEED 0.5, MAX_HEALTH 30.
    //
    // The parts that make a guardian a guardian: the GuardianMoveControl
    // hover-swim (Controls.hpp), the water-bound navigation, the water-first
    // walk-target value, the beached hop, the client-side tail/spikes
    // animation state the renderer reads, GuardianAttackGoal's 80-tick
    // (elder 60) beam charge with its difficulty/elder damage scaling
    // (GuardianGoals.hpp), the target selector that feeds it, and
    // MoveTowardsRestrictionGoal (dormant until something sets a home —
    // exactly MC, where no vanilla path calls a guardian's restrictTo).
    // Not modelled, named at its site: the beam VISUAL (MC's renderer draws
    // DATA_ID_ATTACK_TARGET as a coloured ray; no beam render pipeline
    // exists — the damage, timing and target-hold behaviour are all in).
    // The elder's mining-fatigue aura IS modelled — see
    // ElderGuardian::CustomServerAiStep.
    class Guardian : public Monster {
    public:
        explicit Guardian(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC DATA_ID_MOVING — set by GuardianMoveControl on the server,
        // mapped onto bit 0 of the wire's one state byte so the client's tail
        // speed can key on it exactly as MC's does. Public because the move
        // control writes it (MC's is a private inner class of Guardian).
        bool IsMoving() const { return m_moving; }
        void SetMoving(bool v) { m_moving = v; }

        // MC Guardian.getAttackDuration — the beam charge time. 80 for the
        // guardian, 60 for the elder.
        virtual int GetAttackDuration() const { return 80; }

        // MC DATA_ID_ATTACK_TARGET, reduced to the boolean the client needs:
        // MC syncs the target's entity id so the renderer can draw the beam
        // AT it; with the beam visual skipped only "is charging a target"
        // crosses the wire (bit 1 of the state byte). The attack-time ramp is
        // client-derived from it, exactly as MC's clientSideAttackTime is.
        bool HasActiveAttackTarget() const { return m_hasActiveAttackTarget; }
        void SetActiveAttackTarget(bool v) { m_hasActiveAttackTarget = v; }

        // MC Guardian.getAttackAnimationScale — 0..1 across the charge.
        float GetAttackAnimationScale(float partialTick) const {
            return (static_cast<float>(m_clientSideAttackTime) + partialTick) /
                   static_cast<float>(GetAttackDuration());
        }

        // MC GuardianAttackGoal.stop calls randomStrollGoal.trigger(); the
        // goal lives in GuardianGoals.cpp, outside the class, so the member
        // gets a public door (hurtServer already used it internally).
        // Out of line: RandomStrollGoal is only forward-declared here.
        void TriggerRandomStroll();

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_moving ? 1 : 0) |
                                        (m_hasActiveAttackTarget ? 2 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_moving = (v & 1) != 0;
            const bool active = (v & 2) != 0;
            // MC onSyncedDataUpdated resets clientSideAttackTime whenever
            // DATA_ID_ATTACK_TARGET changes — same reset, keyed on the bit.
            if (active != m_hasActiveAttackTarget) m_clientSideAttackTime = 0;
            m_hasActiveAttackTarget = active;
        }

        // MC Guardian.getWalkTargetValue: water scores 10 + the light cost, so
        // a wandering guardian aims for water first.
        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        void AiStep() override;
        void Travel(const glm::dvec3& input) override;

        // MC Guardian.hurtServer — the thorns retaliation while its spikes
        // are out (not moving), plus the flee-stroll trigger.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Guardian.checkSpawnObstruction: only the entity-overlap half of
        // the base test — a guardian spawns IN water, so the base's
        // no-liquid rejection would veto every legal spawn.
        bool CheckSpawnObstruction(EntityLevel& level) const override;

        int GetMaxHeadXRot() const override { return 180; }
        int GetAmbientSoundInterval() const override { return 160; }

        // MC GuardianRenderer.extractRenderState.
        float GetTailAnimation(float partialTick) const {
            return m_tailAnimationO +
                   partialTick * (m_tailAnimation - m_tailAnimationO);
        }
        float GetSpikesAnimation(float partialTick) const {
            return m_spikesAnimationO +
                   partialTick * (m_spikesAnimation - m_spikesAnimationO);
        }

    protected:
        // The variant constructor — ElderGuardian is a guardian of a
        // different type id, exactly as MC's extends.
        Guardian(EntityTypeId type, EntityLevel* level);

        void RegisterGoals() override;

        // MC Guardian.randomStrollGoal — kept so hurtServer can trigger it
        // and the elder can slow its interval. Owned by the goal selector.
        RandomStrollGoal* m_randomStrollGoal = nullptr;

    private:
        // MC's clientSide* fields — written by AiStep on the client only.
        float m_tailAnimation = 0.0f;
        float m_tailAnimationO = 0.0f;
        float m_tailAnimationSpeed = 0.0f;
        float m_spikesAnimation = 0.0f;
        float m_spikesAnimationO = 0.0f;
        int   m_clientSideAttackTime = 0;
        bool  m_moving = false;
        bool  m_hasActiveAttackTarget = false;
    };

    // MC ElderGuardian — guardian attributes overridden to MOVEMENT_SPEED
    // 0.3, ATTACK_DAMAGE 8, MAX_HEALTH 80; persistent; wanders on a 400-tick
    // interval. Its larger body comes from the entity-type table
    // (GeneratedEntityTypes), not the class. The mining-fatigue aura runs in
    // CustomServerAiStep, which also anchors the 16-block home the
    // restriction goal patrols.
    class ElderGuardian : public Guardian {
    public:
        explicit ElderGuardian(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ElderGuardian.getAttackDuration — a faster beam than the
        // guardian's 80.
        int GetAttackDuration() const override { return 60; }

    protected:
        // MC ElderGuardian.customServerAiStep — every 1200 ticks (staggered by
        // entity id), Mining Fatigue III for 6000 ticks on every survival
        // player within 50 blocks.
        void CustomServerAiStep() override;
    };

    // MC animal/golem/IronGolem (AbstractGolem -> PathfinderMob, so it lives
    // on the pathfinder base, not Monster — golems neither burn nor prefer
    // the dark). MAX_HEALTH 100, MOVEMENT_SPEED 0.25, KNOCKBACK_RESISTANCE
    // 1.0, ATTACK_DAMAGE 15, STEP_HEIGHT 1.0.
    //
    // The parts that make an iron golem an iron golem here: the randomised
    // half-to-double melee damage with the 0.4 vertical launch (doHurtTarget),
    // the attack/offer-flower animation clocks and their entity events,
    // hunting hostile mobs while never touching creepers, and the
    // persistent-anger system (NeutralMob: the isAngryAt-gated player hunt at
    // target priority 3 + ResetUniversalAngerTargetGoal at 4 — a golem only
    // turns on players it is ANGRY at). Not modelled, each named at its site:
    // the village layer (MoveBackToVillageGoal, GolemRandomStrollInVillageGoal,
    // OfferFlowerGoal, DefendVillageTargetGoal), player construction
    // (PlayerCreated flag), crackiness, and the iron-ingot repair interact.
    class IronGolem : public PathfinderMob, public NeutralMob {
    public:
        explicit IronGolem(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC AbstractGolem.removeWhenFarAway — golems never despawn.
        bool RemoveWhenFarAway(double) const override { return false; }
        // MC AbstractGolem.getAmbientSoundInterval — 120, like animals.
        int GetAmbientSoundInterval() const override { return 120; }

        // MC IronGolem.canAttack: never a creeper (the playerCreated half
        // needs the construction system).
        bool CanAttack(const LivingEntity& target) const override;

        // MC IronGolem.doHurtTarget — attackAnimationTick=10, entity event 4,
        // damage attackDamage/2 + nextInt(attackDamage), 0.4 vertical launch
        // scaled by the target's knockback resistance.
        bool DoHurtTarget(Entity& target) override;

        // MC IronGolem.aiStep — count both animation clocks down (both sides).
        void AiStep() override;

        // MC IronGolem.handleEntityEvent: 4 attack, 11/34 offer flower.
        void HandleEntityEvent(uint8_t id) override;

        // MC IronGolemRenderer.extractRenderState inputs.
        int GetAttackAnimationTick() const { return m_attackAnimationTick; }
        int GetOfferFlowerTick() const { return m_offerFlowerTick; }

        // MC IronGolem.startPersistentAngerTimer — rangeOfSeconds(20, 39).
        void StartPersistentAngerTimer() override;

        // MC IronGolem.decreaseAirSupply: a golem never loses air.
        int DecreaseAirSupply(int currentSupply) override { return currentSupply; }

        void ClearReferenceTo(const Entity* entity) override {
            PathfinderMob::ClearReferenceTo(entity);
            ClearAngerReferenceTo(entity);
        }

    protected:
        void RegisterGoals() override;

    private:
        int m_attackAnimationTick = 0;
        int m_offerFlowerTick = 0;
    };

    // MC monster/Blaze. ATTACK_DAMAGE 6, MOVEMENT_SPEED 0.23, FOLLOW_RANGE 48.
    //
    // The parts that make a blaze a blaze: BlazeAttackGoal's 3-shot
    // SmallFireball burst with its 60-tick charged wind-up, the hover
    // (customServerAiStep rises toward a target above it; aiStep damps every
    // fall to 60%), fire immunity, water sensitivity (1 damage per wet tick),
    // and the charged flag — MC syncs it as entity data; here it rides the
    // wire's anim byte and doubles as IsOnFire so the on-fire wire bit (and
    // any future flame render) follows it, exactly as MC's isOnFire does.
    // MoveTowardsRestrictionGoal(5) is registered per MC and stays dormant
    // until something sets a home, exactly as in vanilla.
    class Blaze : public Monster {
    public:
        explicit Blaze(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool FireImmune() const override { return true; }
        bool IsOnFire() const override { return m_charged; }

        bool IsCharged() const { return m_charged; }
        void SetCharged(bool v) { m_charged = v; }

        uint8_t GetAnimStateByte() const override { return m_charged ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_charged = (v & 1) != 0; }

        // MC Blaze.aiStep — the falling damp (both sides, like MC).
        void AiStep() override;

        // MC Blaze has no magic-value dimming — it glows.
        // (getLightLevelDependentMagicValue 1.0 has no renderer hook yet.)

    protected:
        void RegisterGoals() override;
        void CustomServerAiStep() override;
        bool IsSensitiveToWater() const override { return true; }

    private:
        float m_allowedHeightOffset = 0.5f;
        int   m_nextHeightOffsetChangeTick = 0;
        bool  m_charged = false;
    };

    // MC monster/Ghast — extends Mob directly (no pathfinding at all): the
    // GhastMoveControl impulse-flight, RandomFloatAroundGoal wander,
    // GhastLookGoal facing, and the 20-tick fireball wind-up. MAX_HEALTH 10,
    // FOLLOW_RANGE 100, FLYING_SPEED 0.06. The charging flag rides the anim
    // byte (MC DATA_IS_CHARGING). Not modelled, named at its site: the
    // reflected-fireball 1000-damage kill (no projectile deflection exists).
    class Ghast : public Mob {
    public:
        explicit Ghast(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool FireImmune() const override { return true; }

        bool IsCharging() const { return m_charging; }
        void SetCharging(bool v) { m_charging = v; }
        uint8_t GetAnimStateByte() const override { return m_charging ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_charging = (v & 1) != 0; }

        int GetExplosionPower() const { return m_explosionPower; }
        void SetExplosionPower(int power) { m_explosionPower = power; }

        int GetMaxSpawnClusterSize() const override { return 1; }

        // MC Ghast.checkFallDamage is empty — a ghast never lands hard.
        bool CauseFallDamage(double, float) override { return false; }

        // MC Ghast.travel — travelFlying(input, 0.02F): no gravity, air drag
        // 0.91 (0.8 in water, 0.5 in lava).
        void Travel(const glm::dvec3& input) override;

        // The registerGoals target predicate |targetY - y| <= 4, expressed
        // through canAttack. DEVIATION NOTE: MC applies the filter only at
        // target SELECTION; through canAttack it also drops a held target
        // that climbs away, which reads the same in play (the ghast simply
        // re-acquires when you level out).
        bool CanAttack(const LivingEntity& target) const override;

        // MC Ghast.faceMovementDirection (static) — shared with GhastLookGoal.
        static void FaceMovementDirection(Mob& ghast);

    protected:
        void RegisterGoals() override;

    private:
        bool m_charging = false;
        int  m_explosionPower = 1;   // MC DEFAULT_EXPLOSION_POWER
    };

    // MC animal/golem/SnowGolem (AbstractGolem -> PathfinderMob). MAX_HEALTH
    // 4, MOVEMENT_SPEED 0.2. Throws 0-damage snowballs (3 vs blazes) via
    // RangedAttackGoal(1.25, 20, 10) at any hostile, and melts — 1 fire
    // damage per tick — in any biome whose base temperature exceeds 1.0 (MC's
    // SNOW_GOLEM_MELTS environment attribute) and in water (isSensitiveTo-
    // Water). Shears take the pumpkin off (mobInteract → shear: the
    // carved-pumpkin drop at eye height, DATA_PUMPKIN_ID cleared, the
    // renderer's SnowGolemHeadLayer stops drawing it). Not modelled, named
    // at its site: the snow trail (no snow layer block).
    class SnowGolem : public PathfinderMob, public RangedAttackMob {
    public:
        explicit SnowGolem(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC SnowGolem.hasPumpkin / setPumpkin — DATA_PUMPKIN_ID's 0x10 bit,
        // carried to the client in the wire's variant byte, saved as
        // "Pumpkin". True from construction, as MC defines it (byte 16).
        bool HasPumpkin() const { return m_hasPumpkin; }
        void SetPumpkin(bool pumpkin) { m_hasPumpkin = pumpkin; }
        uint8_t GetVariantByte() const override { return m_hasPumpkin ? 0x10 : 0x00; }
        void    SetVariantByte(uint8_t v) override { m_hasPumpkin = (v & 0x10) != 0; }

        // MC SnowGolem.readyForShearing: alive and still wearing it.
        bool ReadyForShearing() const { return IsAlive() && HasPumpkin(); }
        // MC SnowGolem.shear: SNOW_GOLEM_SHEAR, setPumpkin(false), the
        // shearing/snow_golem loot table (one carved pumpkin) dropped at
        // eye height.
        void Shear();
        // MC SnowGolem.mobInteract: shears + readyForShearing → shear, SUCCESS;
        // anything else PASS.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC AbstractGolem: never despawns, animal ambient-sound cadence.
        bool RemoveWhenFarAway(double) const override { return false; }
        int GetAmbientSoundInterval() const override { return 120; }

        // MC SnowGolem.performRangedAttack — snowball from eye height minus
        // 0.1, aimed at the target's eye minus 1.1 with a 0.2-per-block loft,
        // velocity 1.6, inaccuracy 12.
        void PerformRangedAttack(LivingEntity& target, float power) override;

        void AiStep() override;

    protected:
        void RegisterGoals() override;
        bool IsSensitiveToWater() const override { return true; }

    private:
        bool m_hasPumpkin = true;
    };

    // MC monster/Witch. MAX_HEALTH 26, MOVEMENT_SPEED 0.25. Throws splash
    // potions via RangedAttackGoal(1.0, 60, 10), picking the potion from the
    // target's state (Witch.performRangedAttack), and drinks its own —
    // water breathing / fire resistance / healing / swiftness on MC's exact
    // triggers and odds, as a 32-tick timed state with the −0.25 drinking
    // slowdown. The item visuals (the bottle in hand, DATA_USING_ITEM sync)
    // are commented away at their sites: no mob equipment or client sync
    // exists. The raid-layer goals (NearestHealableRaiderTargetGoal, the
    // raider-heal throw branch, the Raider base) wait on raids.
    class Witch : public Monster, public RangedAttackMob {
    public:
        explicit Witch(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC Witch.performRangedAttack — leads the target by one tick of its
        // velocity, aims at eye height minus 1.1 with a 0.2-per-block loft,
        // velocity 0.75, inaccuracy 8. No-ops mid-drink, as MC's does.
        void PerformRangedAttack(LivingEntity& target, float power) override;

        // MC Witch.isDrinkingPotion (DATA_USING_ITEM, server truth).
        bool IsDrinkingPotion() const { return m_isDrinking; }

        // MC Witch.aiStep — the drink state machine, run before super.
        void AiStep() override;

        // MC Witch.handleEntityEvent(15) — the ambient WITCH-particle burst.
        void HandleEntityEvent(uint8_t id) override;

    protected:
        void RegisterGoals() override;

        // MC Witch.getDamageAfterMagicAbsorb: self-inflicted splash damage is
        // zeroed, and WITCH_RESISTANT_TO (magic) damage is cut to 15%.
        float GetDamageAfterMagicAbsorb(MobDamageSource source, float amount,
                                        Entity* attacker) const override;

    private:
        bool m_isDrinking = false;
        int  m_usingTime = 0;
        // The potion being drunk — applied to self when the timer runs out
        // (MC reads it back off the main-hand ItemStack; no item system).
        std::vector<MobEffectInstance> m_drinkPotion;
    };

    // MC monster/Shulker (AbstractGolem -> PathfinderMob). MAX_HEALTH 30.
    //
    // Ported: the peek machine (raw peek 0/30/100 synced — here over the anim
    // byte — with the client's 0.05-per-tick lid lerp the model reads), the
    // attach face (over the variant byte), block-centre snapping, staying
    // attached / re-attaching / teleporting up to 8 blocks when the support
    // block goes or when badly hurt, the closed-lid armor bonus (+20), and
    // ShulkerAttackGoal's bullet cadence. Not modelled, each named at its
    // site: riding/pistons, the bullet-hit split into a new shulker, the
    // closed-lid ARROW bounce (approximated to all projectiles — the wire
    // does not say what kind of projectile hit), the dyed-color variants, and
    // MC's do-nothing look/body controls (approximated by zeroing yBodyRot).
    class Shulker : public PathfinderMob {
    public:
        explicit Shulker(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool RemoveWhenFarAway(double) const override { return false; }

        // ── Peek (MC DATA_PEEK_ID, 0-100) ─────────────────────────────────
        int  GetRawPeekAmount() const { return m_peekAmount; }
        void SetRawPeekAmount(int amount);
        bool IsClosed() const { return m_peekAmount == 0; }
        // MC Shulker.getClientPeekAmount — what ShulkerRenderer feeds the
        // model (0..1).
        float GetClientPeekAmount(float partialTick) const {
            return m_currentPeekAmountO +
                   partialTick * (m_currentPeekAmount - m_currentPeekAmountO);
        }

        // ── Attach face (MC DATA_ATTACH_FACE_ID, Direction ordinal) ───────
        int  GetAttachFace() const { return m_attachFace; }
        int  GetAttachAxis() const;   // 0 = X, 1 = Y, 2 = Z
        bool CanStayAt(const glm::ivec3& pos, int face) const;
        bool TeleportSomewhere();

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(m_peekAmount);
        }
        void SetAnimStateByte(uint8_t v) override {
            m_peekAmount = static_cast<int>(v);
        }
        uint8_t GetVariantByte() const override {
            return static_cast<uint8_t>(m_attachFace);
        }
        void SetVariantByte(uint8_t v) override {
            m_attachFace = v <= 5 ? static_cast<int>(v) : 0;
        }
        // Same clamp, named for what it is. Vanilla's AttachFace is a
        // Direction 3D-data value, so anything above 5 is corrupt and
        // vanilla's own decoder falls back to DOWN.
        void SetAttachFace(int face) { m_attachFace = (face >= 0 && face <= 5) ? face : 0; }

        // A shulker never walks — MC roots it by zeroing every setDeltaMovement.
        void Travel(const glm::dvec3&) override {}

        void Tick() override;

        // MC Shulker.hurtServer — closed-lid projectile bounce, the
        // half-health teleport escape.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

    protected:
        void RegisterGoals() override;

    private:
        int FindAttachableSurface(const glm::ivec3& pos) const;
        void FindNewAttachment();
        void UpdateCoveredArmor();

        int   m_attachFace = 0;   // MC DEFAULT: DOWN — sitting on the floor
        int   m_peekAmount = 0;
        float m_currentPeekAmount = 0.0f;
        float m_currentPeekAmountO = 0.0f;
    };

    // MC monster/Ravager. MAX_HEALTH 100, MOVEMENT_SPEED 0.3,
    // KNOCKBACK_RESISTANCE 0.75, ATTACK_DAMAGE 12, ATTACK_KNOCKBACK 1.5,
    // FOLLOW_RANGE 32, STEP_HEIGHT 1.0.
    //
    // The parts that make a ravager a ravager here: the attack/stun/roar
    // clocks with their immobilisation, the speed lerp toward 0.35 while
    // hunting, the roar's area damage + strong knockback, and entity events
    // 4/39/69. Not modelled, each named at its site: the raid layer (Raider
    // base, its goals, applyRaidBuffs, the celebrate sound), shields
    // (blockedByItem is the only thing that STARTS a stun), leaf-destruction
    // on collision (needs mob griefing), and the stun/roar particles.
    class Ravager : public Monster {
    public:
        explicit Ravager(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        int GetMaxHeadYRot() const override { return 45; }

        // MC Ravager.aiStep — speed lerp, collision jump, and the three
        // clocks. Runs AFTER super.aiStep(), as MC does.
        void AiStep() override;

        // MC Ravager.isImmobile: any active clock roots it in place.
        bool IsImmobile() const override {
            return Monster::IsImmobile() || m_attackTick > 0 ||
                   m_stunnedTick > 0 || m_roarTick > 0;
        }

        // MC Ravager.doHurtTarget — attackTick=10 + entity event 4, then the
        // base hit (ATTACK_KNOCKBACK 1.5 rides the base's extra knockback).
        bool DoHurtTarget(Entity& target) override;

        // MC Ravager.handleEntityEvent: 4 attack, 39 stunned, 69 roar.
        void HandleEntityEvent(uint8_t id) override;

        // MC RavagerRenderer.extractRenderState inputs.
        int GetAttackTick() const { return m_attackTick; }
        int GetStunnedTick() const { return m_stunnedTick; }
        int GetRoarTick() const { return m_roarTick; }

        // Restore-only. MC persists all three so a ravager saved mid-stun
        // reloads still stunned instead of immediately charging again.
        void SetAttackTick(int t)  { m_attackTick = t; }
        void SetStunnedTick(int t) { m_stunnedTick = t; }
        void SetRoarTick(int t)    { m_roarTick = t; }

    protected:
        void RegisterGoals() override;

    private:
        // MC Ravager.roar — the 4-block area damage + knockback, fired when
        // the roar clock hits 10.
        void Roar();
        // MC Ravager.strongKnockback — 4/d horizontal shove, 0.2 up.
        void StrongKnockback(Entity& entity);
        // MC Ravager.applyRoarKnockbackClient — the client-side half of
        // event 69.
        void ApplyRoarKnockbackClient();

        int m_attackTick = 0;   // MC ATTACK_DURATION 10
        int m_stunnedTick = 0;  // MC STUN_DURATION 40
        int m_roarTick = 0;     // 20, set when a stun expires
    };

    // ── Phantom ────────────────────────────────────────────────────────────

    // MC Phantom.AttackPhase — the two-state machine the three move goals
    // arbitrate through. Named at namespace scope because the goals live in
    // PhantomGoals.hpp rather than as nested classes.
    enum class PhantomAttackPhase : uint8_t { Circle, Swoop };

    // MC monster/Phantom — extends Mob directly (no pathfinding: the
    // PhantomMoveControl steers on raw velocity). Plain Monster attributes
    // (MC registers Monster.createMonsterAttributes for it); ATTACK_DAMAGE is
    // re-based to 6 + size whenever the size changes.
    //
    // Ported: the anchor point, the CIRCLE/SWOOP state machine and its four
    // goals (PhantomGoals.hpp), the three custom controls, size scaling
    // (variant byte -> health/attack/box/render scale), the daylight burn,
    // and travelFlying(0.2). Not modelled, named at its site: the flap sound
    // and myclium trail particles (client tick), and the cat hiss the sweep
    // goal triggers (the SCARE itself is in).
    class Phantom : public Mob {
    public:
        explicit Phantom(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // ── Size (MC ID_SIZE, synched) ────────────────────────────────────
        int  GetPhantomSize() const { return m_size; }
        void SetPhantomSize(int size);
        uint8_t GetVariantByte() const override {
            return static_cast<uint8_t>(m_size);
        }
        void SetVariantByte(uint8_t v) override { SetPhantomSize(v); }

        // MC getDefaultDimensions: the base box scaled by 1 + 0.15 * size.
        float BaseBbWidth() const override {
            return Mob::BaseBbWidth() * (1.0f + 0.15f * static_cast<float>(m_size));
        }
        float BaseBbHeight() const override {
            return Mob::BaseBbHeight() * (1.0f + 0.15f * static_cast<float>(m_size));
        }

        // ── The shared state the goals and controls read ──────────────────
        const glm::dvec3& GetMoveTargetPoint() const { return m_moveTargetPoint; }
        void SetMoveTargetPoint(const glm::dvec3& p) { m_moveTargetPoint = p; }
        bool HasAnchorPoint() const { return m_hasAnchorPoint; }
        const glm::ivec3& GetAnchorPoint() const { return m_anchorPoint; }
        void SetAnchorPoint(const glm::ivec3& p) {
            m_anchorPoint = p;
            m_hasAnchorPoint = true;
        }
        PhantomAttackPhase GetAttackPhase() const { return m_attackPhase; }
        void SetAttackPhase(PhantomAttackPhase p) { m_attackPhase = p; }

        // MC Phantom.travel: travelFlying(input, 0.2F).
        void Travel(const glm::dvec3& input) override;

        // MC Phantom.checkFallDamage is empty — a swooping phantom never
        // lands hard.
        bool CauseFallDamage(double, float) override { return false; }

        // MC Phantom.finalizeSpawn: anchor five blocks up, size 0.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

    protected:
        void RegisterGoals() override;
        // MC EntityTypeTags.BURN_IN_DAYLIGHT membership.
        bool BurnsInDaylight() const override { return true; }

    private:
        glm::dvec3 m_moveTargetPoint{0.0};
        glm::ivec3 m_anchorPoint{0};
        bool       m_hasAnchorPoint = false;
        PhantomAttackPhase m_attackPhase = PhantomAttackPhase::Circle;
        int m_size = 0;
    };

    // ── Vex ────────────────────────────────────────────────────────────────

    // MC monster/Vex — the evoker's summon: MAX_HEALTH 14, ATTACK_DAMAGE 4 on
    // the monster base; no gravity, VexMoveControl impulse flight
    // (EvokerGoals.hpp), an owner link whose target it copies, and a limited
    // life that starves it down once expired. The charging flag rides the
    // anim byte (MC DATA_FLAGS_ID bit 1); the renderer swaps to the charging
    // texture and sets isCharging from it. Not modelled, each named at its
    // site: noPhysics wall-phasing (the engine's mover has no ghost mode —
    // a vex respects walls), the iron-sword equipment roll, and the raid
    // roster exemption on HurtByTargetGoal.
    class Vex : public Monster {
    public:
        explicit Vex(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool IsCharging() const { return m_charging; }
        void SetIsCharging(bool v) { m_charging = v; }
        uint8_t GetAnimStateByte() const override { return m_charging ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_charging = (v & 1) != 0; }

        Mob* GetVexOwner() const { return m_owner; }
        void SetVexOwner(Mob* owner) { m_owner = owner; }

        bool HasBoundOrigin() const { return m_hasBoundOrigin; }
        const glm::ivec3& GetBoundOrigin() const { return m_boundOrigin; }
        void SetBoundOrigin(const glm::ivec3& p) {
            m_boundOrigin = p;
            m_hasBoundOrigin = true;
        }

        // MC Vex.setLimitedLife — armed by EvokerSummonSpellGoal.
        void SetLimitedLife(int lifeTicks) {
            m_hasLimitedLife = true;
            m_limitedLifeTicks = lifeTicks;
        }

        // The save side of the same pair. Vanilla writes "life_ticks" only
        // when the clock is armed, so the flag has to be readable too.
        bool HasLimitedLife() const { return m_hasLimitedLife; }
        int  GetLimitedLifeTicks() const { return m_limitedLifeTicks; }

        // MC Vex.tick: noGravity every tick, and the expired-life starvation.
        void Tick() override;

        void ClearReferenceTo(const Entity* entity) override {
            Monster::ClearReferenceTo(entity);
            if (m_owner == entity) m_owner = nullptr;
        }

        // MC Vex.getLightLevelDependentMagicValue returns 1.0F (full-bright
        // render) — no renderer hook for per-mob brightness yet.

    protected:
        void RegisterGoals() override;

    private:
        Mob*       m_owner = nullptr;
        glm::ivec3 m_boundOrigin{0};
        bool       m_hasBoundOrigin = false;
        bool       m_hasLimitedLife = false;
        int        m_limitedLifeTicks = 0;
        bool       m_charging = false;
    };

    // ── Evoker ─────────────────────────────────────────────────────────────

    class Sheep;

    // MC monster/illager/SpellcasterIllager — the casting-state machinery the
    // Evoker (and one day the Illusioner) runs on. The current spell id rides
    // the wire's anim byte (MC DATA_SPELL_CASTING_ID), which is also what the
    // renderer keys the SPELLCASTING arm pose on. The AbstractIllager /
    // Raider layers above it in MC are raid machinery and are skipped with
    // raids; this port derives Monster directly, as the other promotions do.
    class SpellcasterIllager : public Monster {
    public:
        // MC SpellcasterIllager.IllagerSpell (ids are wire-visible via the
        // anim byte; the colours are the skipped hand particles').
        enum class IllagerSpell : uint8_t {
            None = 0,
            SummonVex = 1,
            Fangs = 2,
            Wololo = 3,
            Disappear = 4,
            Blindness = 5,
        };

        SpellcasterIllager(EntityTypeId type, EntityLevel* level);

        // MC isCastingSpell: server truth is the tick counter, client truth
        // is the synced spell id. Out of line — EntityLevel is incomplete
        // here.
        bool IsCastingSpell() const;
        IllagerSpell GetCurrentSpell() const;
        void SetIsCastingSpell(IllagerSpell spell) {
            m_currentSpell = spell;
            m_clientSpellId = static_cast<uint8_t>(spell);
        }

        int  GetSpellCastingTime() const { return m_spellCastingTickCount; }
        void SetSpellCastingTime(int ticks) { m_spellCastingTickCount = ticks; }

        uint8_t GetAnimStateByte() const override { return m_clientSpellId; }
        void    SetAnimStateByte(uint8_t v) override { m_clientSpellId = v; }

    protected:
        // MC SpellcasterIllager.customServerAiStep: count the cast down.
        void CustomServerAiStep() override;

        // MC SpellcasterIllager.tick's client half is the two hand-particle
        // streams — no particle system to land them in.

    private:
        int m_spellCastingTickCount = 0;
        IllagerSpell m_currentSpell = IllagerSpell::None;
        // The synced byte's client-side copy (server keeps it mirrored).
        uint8_t m_clientSpellId = 0;
    };

    // MC monster/illager/Evoker. MOVEMENT_SPEED 0.5, FOLLOW_RANGE 12,
    // MAX_HEALTH 24.
    //
    // Ported: the whole spell battery (EvokerGoals.hpp) — the fang attack's
    // two close-range rings and 16-fang line (EvokerFangs entities), the
    // 3-vex summon with limited life and owner link, the wololo blue->red
    // sheep recolour, the casting-goal look lock — plus the player/creaking
    // avoidance and the villager/golem targets. Not modelled, each named at
    // its site: raids (Raider base, applyRaidBuffs, celebrate), the
    // considersEntityAsAlly team logic, and mobGriefing gating on wololo
    // (no game rules — treated as ON, MC's default).
    class Evoker : public SpellcasterIllager {
    public:
        explicit Evoker(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        Sheep* GetWololoTarget() const { return m_wololoTarget; }
        void   SetWololoTarget(Sheep* sheep) { m_wololoTarget = sheep; }

        void ClearReferenceTo(const Entity* entity) override;

    protected:
        void RegisterGoals() override;

    private:
        Sheep* m_wololoTarget = nullptr;
    };

    // ── Illusioner ─────────────────────────────────────────────────────────

    // MC monster/illager/Illusioner. MOVEMENT_SPEED 0.5, FOLLOW_RANGE 18,
    // MAX_HEALTH 32.
    //
    // Ported: the two spells (IllusionerGoals.hpp — blindness on the target
    // on HARD, self-invisibility for the mirror trick's server half) and the
    // bow AI — RangedBowAttackGoal(0.5, 20, 15.0F) with the skeleton's exact
    // arrow numbers (performRangedAttack is ProjectileUtil.getMobArrow +
    // shoot(1.6F, 14 - difficulty*4), the same math AbstractSkeleton uses).
    // Not modelled, each named at its site: the raid layer (Raider base,
    // applyRaidBuffs, celebrate), the four client-side mirror images
    // (clientSideIllusionOffsets — render trickery for a renderer with no
    // multi-instance draw), and the bow ITEM in hand (finalizeSpawn's
    // setItemSlot(BOW) — no mob equipment system; the bow goal treats the bow
    // as permanent, the skeleton precedent).
    class Illusioner : public SpellcasterIllager, public RangedAttackMob {
    public:
        explicit Illusioner(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC Illusioner.performRangedAttack — identical numbers to
        // AbstractSkeleton's: arrow from eye height minus 0.1, aimed a third
        // up the target's box with the 0.2-per-block loft, velocity 1.6,
        // inaccuracy 14 - difficultyId * 4.
        void PerformRangedAttack(LivingEntity& target, float power) override;

    protected:
        void RegisterGoals() override;
    };

    // ── Vindicator ─────────────────────────────────────────────────────────

    // MC monster/illager/Vindicator. MOVEMENT_SPEED 0.35, FOLLOW_RANGE 12,
    // MAX_HEALTH 24, ATTACK_DAMAGE 5.
    //
    // Promoted for its DOOR BREAKING: VindicatorBreakDoorGoal (DoorGoals.hpp)
    // at priority 2, with the pathfinder taught to walk at closed wooden
    // doors. DEVIATION, documented at both sites: MC gates the goal and the
    // per-tick canOpenDoors on hasActiveRaid()/isRaided(); with no raid
    // system those gates would leave the behaviour permanently dead, so they
    // are treated as satisfied — the door pounding itself (240 ticks,
    // NORMAL/HARD only, mobGriefing-gated) is MC's in-raid behaviour
    // exactly. Not modelled, each named at its site: the raid layer
    // (Raider base, HoldGroundAttackGoal, RaiderOpenDoorGoal, applyRaidBuffs,
    // celebrate), VindicatorJohnnyAttackGoal (armed by naming the mob
    // "Johnny" — no custom-name system), and the iron axe in hand
    // (populateDefaultEquipmentSlots — no mob equipment; the model's
    // ATTACKING arm pose rides the aggressive flag as MC's does).
    class Vindicator : public Monster {
    public:
        explicit Vindicator(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC Vindicator.finalizeSpawn: navigation.setCanOpenDoors(true) (plus
        // the equipment rolls, skipped — no equipment system).
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

    protected:
        void RegisterGoals() override;
    };

    // ── Wither ─────────────────────────────────────────────────────────────

    // MC boss/wither/WitherBoss. MAX_HEALTH 300, MOVEMENT_SPEED 0.6,
    // FLYING_SPEED 0.6, FOLLOW_RANGE 40, ARMOR 4.
    //
    // Ported: the 220-tick invulnerable spawn phase (health ramp, closing
    // burst — entity-damage-only per project policy), three heads with
    // independent targets and MC's exact volley cadences (10+rand(10) idle
    // timers, the >15 idle-shot rule on NORMAL/HARD, 40+rand(20) after a
    // shot, the 0.1% dangerous-skull roll), the half-health "powered" armor
    // state with its projectile immunity, the 1 HP/s regen (10/s while
    // spawning), the undead-exclusion targeting, hover flight toward the
    // primary target, total status-effect immunity, and the nether star
    // drop. Not modelled, each named at its site: the boss bar, block
    // destruction (destroyBlocksTick — mob griefing of terrain is not
    // modelled), the side-head visual aim (the model renders the small heads
    // forward; syncing two extra rotation pairs is the skipped piece — the
    // BEHAVIOUR, per-head volleys at per-head targets, is fully in), the
    // armor overlay layer, and the soul-sand spawn ritual (/summon and the
    // spawn egg call MakeInvulnerable instead, standing in for the ritual's
    // makeInvulnerable call).
    class Wither : public Monster, public RangedAttackMob {
    public:
        explicit Wither(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        int  GetInvulnerableTicks() const { return m_invulnerableTicks; }
        void SetInvulnerableTicks(int t) { m_invulnerableTicks = t; }

        // MC Wither.isPowered — the half-health armor state.
        bool IsPowered() const { return GetHealth() <= GetMaxHealth() / 2.0f; }

        // MC makeInvulnerable: 220 ticks, health at a third (the ramp heals
        // it to full as the timer runs out).
        void MakeInvulnerable();

        // The client's halves of DATA_ID_INV / powered: bit 0 = spawning
        // (invulnerable), bit 1 = powered, bits 2..7 = invulnerableTicks / 5
        // (0..44 — fits with room to spare). Five-tick resolution is exactly
        // what the renderer consumes: WitherBossRenderer's blue flicker keys
        // on `ticks / 5 % 2` (carried EXACTLY) and its 1.5→2.0 growth ramp on
        // `ticks / 220` (within 4 ticks ≈ 0.009 scale — invisible).
        uint8_t GetAnimStateByte() const override {
            const int q = std::min(m_invulnerableTicks / 5, 63);
            return static_cast<uint8_t>((m_invulnerableTicks > 0 ? 1 : 0) |
                                        (IsPowered() ? 2 : 0) |
                                        (q << 2));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_clientInvulnerable = (v & 1) != 0;
            m_clientPowered = (v & 2) != 0;
            m_clientInvulnerableTicks = ((v >> 2) & 63) * 5;
        }
        bool IsInvulnerablePhaseClient() const { return m_clientInvulnerable; }
        // MC WitherRenderState.invulnerableTicks, at the wire's 5-tick grain.
        // Zero once the charge ends even though bit 0's last quantum may
        // still be in flight — bit 0 is the authority on "spawning at all".
        int GetClientInvulnerableTicks() const {
            return m_clientInvulnerable ? m_clientInvulnerableTicks : 0;
        }

        // MC Wither.aiStep — the hover flight, head-timer bookkeeping, and
        // the client particle tail (head smoke / powered aura / charge-up
        // column).
        void AiStep() override;

        // The spawn-burst explosion visual — answers this port's
        // kEntityEventExplosionEmitter stand-in for MC's explosion packet
        // (see EntityLevel.hpp).
        void HandleEntityEvent(uint8_t id) override;

        // MC RangedAttackMob.performRangedAttack — head 0's volley, driven by
        // RangedAttackGoal(1.0, 40, 20).
        void PerformRangedAttack(LivingEntity& target, float power) override;

        // MC Wither.hurtServer — immunities and the head-timer advance.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Wither.checkDespawn: never despawns; peaceful discards.
        void CheckDespawn() override;

        // MC Wither.addEffect returns false — nothing sticks to a wither.
        bool CanBeAffected(const MobEffectInstance& effect) const override {
            (void)effect;
            return false;
        }

        // MC Wither.dropCustomDeathLoot — the nether star.
        void DropCustomDeathLoot(EntityLevel& level) override;

        // The egg//summon spawn path charges the wither up (MakeInvulnerable),
        // standing in for the soul-sand ritual's call.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void ClearReferenceTo(const Entity* entity) override;

    protected:
        void RegisterGoals() override;
        void CustomServerAiStep() override;

    private:
        // MC's alternativeTarget(1)/(2) — the side heads' own targets. MC
        // syncs entity ids; this port keeps pointers (ClearReferenceTo drops
        // them), since the side-head visual that would need them client-side
        // is skipped.
        void PerformRangedAttack(int head, LivingEntity& target);
        void PerformRangedAttack(int head, double tx, double ty, double tz,
                                 bool dangerous);
        double GetHeadX(int head) const;
        double GetHeadY(int head) const;
        double GetHeadZ(int head) const;
        void SpawnBurstExplosion();

        LivingEntity* m_headTargets[2] = { nullptr, nullptr };
        int m_nextHeadUpdate[2] = { 0, 0 };
        int m_idleHeadUpdates[2] = { 0, 0 };
        int m_invulnerableTicks = 0;
        // MC destroyBlocksTick — armed by Hurt, counts down in
        // customServerAiStep; the block destruction it would fire is the
        // skipped mob-griefing piece, the timer is kept so the wiring reads
        // like MC's.
        int m_destroyBlocksTick = 0;
        bool m_clientInvulnerable = false;
        bool m_clientPowered = false;
        int  m_clientInvulnerableTicks = 0;
    };

    // ── Strider ────────────────────────────────────────────────────────────

    // MC Strider.StriderGroupData stand-in: MC uses AgeableMob.AgeableMobGroupData
    // (a per-pack baby chance); our AgeableMob has no group token, so the
    // consumption is inlined in Strider::FinalizeSpawn.
    struct StriderGroupData : SpawnGroupData {
        explicit StriderGroupData(float chance) : babyChance(chance) {}
        float babyChance = 0.0f;
    };

    // MC monster/Strider — an Animal in MC's hierarchy (it breeds), placed
    // here with the other nether promotions. Animal attributes +
    // MOVEMENT_SPEED 0.175.
    //
    // Ported: StriderGoToLavaGoal (StriderGoals.hpp), the lava-walk
    // approximation (see Travel — the engine's mover has no fluid-standing
    // support, so the strider bobs to the lava surface and is snapped onto
    // it), the suffocating/shivering cold state (variant byte -> the cold
    // texture swap + the -34% speed), water sensitivity, the lava-first walk
    // target, and the spawn jockeys (zombified-piglin 1/30, baby-strider
    // 1/10 — riding is live). Not modelled, each named at its site:
    // saddle/riding-by-player (ItemBasedSteering, warped fungus on a stick),
    // TemptGoal/breeding food (no warped fungus item exists), and the
    // STRIDER_WARM_BLOCKS tag beyond lava itself.
    class Strider : public Animal {
    public:
        explicit Strider(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool IsSuffocating() const { return m_suffocating; }
        void SetSuffocating(bool v);

        uint8_t GetVariantByte() const override { return m_suffocating ? 1 : 0; }
        void SetVariantByte(uint8_t v) override { m_suffocating = (v & 1) != 0; }

        // No warped fungus item exists — nothing tempts or breeds a strider
        // yet (MC: ItemTags.STRIDER_FOOD / STRIDER_TEMPT_ITEMS).
        bool IsFood(uint32_t) const override { return false; }
        std::unique_ptr<Animal> CreateBaby() override {
            return std::make_unique<Strider>(m_level);
        }

        // Lava contact, sampled from the blocks the box overlaps — the
        // engine's Entity::IsInLava has no fluid tracking to answer from.
        bool IsInLava() const override;

        // MC Strider.isSensitiveToWater / isOnFire.
        bool IsOnFire() const override { return false; }

        // MC Strider.checkFallDamage: falling into lava is free.
        bool CauseFallDamage(double fallDist, float damageMultiplier) override {
            if (IsInLava()) {
                ResetFallDistance();
                return false;
            }
            return Animal::CauseFallDamage(fallDist, damageMultiplier);
        }

        // MC Strider.getWalkTargetValue: lava 10, else -inf while in lava.
        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        // MC Strider.checkSpawnObstruction: isUnobstructed only — the base's
        // no-liquid test would veto every lava spawn.
        bool CheckSpawnObstruction(EntityLevel& level) const override;

        // MC Strider.tick: the suffocating/warm-block check, then the float.
        void Tick() override;

        // The lava-surface support half of floatStrider (see the .cpp).
        void Travel(const glm::dvec3& input) override;

        // MC Strider.finalizeSpawn — the jockey rolls.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

    protected:
        void RegisterGoals() override;
        bool IsSensitiveToWater() const override { return true; }

    private:
        void FloatStrider();

        bool m_suffocating = false;
    };

    // ── Ender dragon ───────────────────────────────────────────────────────

    // MC boss/enderdragon/DragonFlightHistory — the 64-sample ring of
    // {y, yRot} the dragon records every tick. The renderer and the model's
    // neck/tail kinematics read it at fixed delays; recording on BOTH sides
    // (the client samples its interpolated copy) is what makes the tail trail
    // through the dragon's actual flight path.
    struct DragonFlightHistory {
        static constexpr int kLength = 64;   // MC LENGTH (MASK = 63)

        struct Sample {
            double y = 0.0;
            float  yRot = 0.0f;
        };

        Sample samples[kLength];
        int head = -1;

        // MC record(y, yRot): the FIRST record floods the whole ring, so a
        // fresh dragon has a flat history rather than 63 zeros.
        void Record(double y, float yRot);

        Sample Get(int delay) const {
            return samples[(head - delay) & (kLength - 1)];
        }
        // The partial-tick lerped read (MC get(delay, partialTicks)).
        Sample Get(int delay, float partialTick) const;
    };

    // MC EnderDragonPhase registration ids, verbatim — the anim byte carries
    // one, so the numbering is wire-visible. All eleven phases are
    // implemented, the perch cycle (LandingApproach, Landing, SittingFlaming,
    // SittingScanning, SittingAttacking) included — it landed with the End
    // dragon-fight layer (EndDragonFight, crystals, the podium).
    enum class DragonPhase : uint8_t {
        HoldingPattern  = 0,
        StrafePlayer    = 1,
        LandingApproach = 2,
        Landing         = 3,
        Takeoff         = 4,
        SittingFlaming  = 5,
        SittingScanning = 6,
        SittingAttacking= 7,
        ChargingPlayer  = 8,
        Dying           = 9,
        Hovering        = 10,
        Count
    };

    class EnderDragon;
    class EndCrystal;
    class IDragonFight;

    // MC DragonPhaseInstance / AbstractDragonPhaseInstance — the per-phase
    // strategy the dragon's aiStep flight consults. Concrete phases live in
    // Monsters.cpp.
    class DragonPhaseInstance {
    public:
        explicit DragonPhaseInstance(EnderDragon* dragon) : m_dragon(dragon) {}
        virtual ~DragonPhaseInstance() = default;

        virtual DragonPhase GetPhase() const = 0;
        virtual bool IsSitting() const { return false; }
        virtual void DoServerTick() {}
        virtual void Begin() {}
        virtual void End() {}
        virtual float GetFlySpeed() const { return 0.6f; }
        // Returns false when the phase has no fly target (the dragon coasts).
        virtual bool GetFlyTargetLocation(glm::dvec3& out) const {
            (void)out;
            return false;
        }
        // MC onHurt(DamageSource, float). `direct` is MC's
        // source.getDirectEntity() — the projectile itself, not its shooter —
        // which the sitting phases inspect for the arrow/wind-charge void.
        virtual float OnHurt(MobDamageSource source, float damage,
                             Entity* direct) {
            (void)source;
            (void)direct;
            return damage;
        }
        // MC onCrystalDestroyed(crystal, pos, source, player) — `player` is
        // the blamed player (may be null). Only the holding pattern reacts.
        virtual void OnCrystalDestroyed(Entity* player) { (void)player; }
        // MC AbstractDragonPhaseInstance.getTurnSpeed — defined out of line
        // (reads the dragon's velocity).
        virtual float GetTurnSpeed() const;
        // The strafe phase caches its attack target — same lifetime contract
        // as Goal::ClearReferenceTo.
        virtual void ClearReferenceTo(const Entity* entity) { (void)entity; }

    protected:
        EnderDragon* m_dragon;
    };

    // MC boss/enderdragon/EnderDragon — extends Mob implements Enemy, NO
    // goals: the phase machine + the aiStep flight physics ARE the AI.
    // Attributes: MAX_HEALTH 200 (CAMERA_DISTANCE 16 has no attribute here).
    //
    // Ported: DragonFlightHistory (both sides), the phase manager over the
    // anim byte (MC DATA_PHASE), the reduced phase set above, the full
    // aiStep flight physics (yaw bank via yRotA, the dot-product speed
    // scaling, the 0.91 vertical drag), the 24-node End-pillar graph and its
    // A* — anchored on the dragon's SPAWN POINT as the "podium" origin
    // (DEVIATION: MC's graph is absolute around the End's 0,0 and floors
    // node height at y=73, the island top; here nodes centre on the fight
    // origin and floor at its height, so a summoned dragon circles where it
    // was summoned) — the wing-buffet knockback + head-bite sweeps (boxes
    // derived from the whole-box layout below), damage gating (player-only,
    // body-hit reduction, the DYING pin at 1 HP, the sitting 25% takeoff
    // rule), knockback suppressed while sitting, total effect immunity, and
    // never despawning.
    //
    // Ported with the dragon-fight layer (2026-09): crystal healing
    // (checkCrystals + nearestCrystal), the fight hooks (updateDragon /
    // setDragonKilled / onCrystalDestroyed via EntityLevel::DragonFight),
    // the crystal-aware node gating and phase odds, the full perch cycle,
    // and the 200-tick death cinematic with the XP shower (TickDeath
    // override; GetXpReward returns 0 so the standard death-loot award
    // cannot pay the 12,000 a second time).
    //
    // Not modelled, each named at its site: the EIGHT SUB-ENTITY hitboxes
    // (head/neck/body/3 tails/2 wings — one whole-box entity here; every hit
    // takes MC's non-head reduction because MC routes plain hurtServer
    // through the body part — except the crystal-explosion hit, which MC
    // routes through the HEAD and so does HurtFromCrystal), the growl/flap
    // sounds, and noPhysics (the mover has no ghost mode — CheckWalls carves
    // the dragon's tunnel instead, which is also MC's mobGriefing
    // behaviour).
    class EnderDragon : public Mob {
    public:
        explicit EnderDragon(EntityLevel* level);
        ~EnderDragon() override;   // out of line: unique_ptr<DragonPhaseInstance>

        static void CreateAttributes(AttributeMap& out);

        // ── MC's public fields, kept public for the renderer ──────────────
        DragonFlightHistory flightHistory;
        float oFlapTime = 0.0f;
        float flapTime = 0.0f;
        bool  inWall = false;
        float yRotA = 0.0f;
        // MC nearestCrystal — the healing beam's far end. Maintained on BOTH
        // sides (the client scans its own mirror so the renderer can draw the
        // beam, exactly as MC's client-side checkCrystals does).
        EndCrystal* nearestCrystal = nullptr;

        // ── Fight origin (MC fightOrigin) ─────────────────────────────────
        // BlockPos.ZERO by default, exactly as in MC: only EndDragonFight's
        // createNewDragon sets it. It anchors the PERCH podium (and the
        // dying dive); the flight-node ring is absolute (see EnsureNodes).
        const glm::ivec3& GetFightOrigin() const { return m_fightOrigin; }
        void SetFightOrigin(const glm::ivec3& pos) { m_fightOrigin = pos; }

        // ── Phase manager (MC EnderDragonPhaseManager) ────────────────────
        void SetPhase(DragonPhase phase);
        DragonPhase GetPhase() const;
        DragonPhaseInstance& GetPhaseInstance(DragonPhase phase);
        bool IsPhaseSitting() const;
        bool IsLandingOrTakingOff() const {
            const DragonPhase p = GetPhase();
            return p == DragonPhase::Landing || p == DragonPhase::Takeoff;
        }

        // The wire: MC DATA_PHASE, on the anim byte.
        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;

        // ── The node graph (MC findClosestNode / findPath) ────────────────
        int FindClosestNode();
        int FindClosestNode(double x, double y, double z);

        // MC's Path, reduced to what the phases consume.
        struct FlightPath {
            std::vector<glm::ivec3> nodes;
            int nextIndex = 0;
            bool IsDone() const {
                return nextIndex >= static_cast<int>(nodes.size());
            }
            void Advance() { ++nextIndex; }
            const glm::ivec3& NextNodePos() const { return nodes[nextIndex]; }
        };
        // A* over the 24-node graph (adjacency masks verbatim). `finalTarget`
        // is MC's finalNode — appended past the end node when given.
        bool FindPath(int startIndex, int endIndex, const glm::ivec3* finalTarget,
                      FlightPath& out);

        // ── Geometry helpers the phases and sweeps share ──────────────────
        // The head part's position per MC's tickPart layout (6.5 blocks ahead
        // along the corrected facing, tilted by the flight history).
        glm::dvec3 GetHeadPosition() const;
        // MC getHeadLookVector(1.0F) — the pitch-adjusted aim vector.
        glm::dvec3 GetHeadLookVector() const;
        // The "podium": the surface block of the fight-origin column (the
        // EndPodiumFeature.getLocation stand-in).
        glm::ivec3 GetPodiumPos() const;
        double DistanceToPodiumSqr() const;

        // ── Overrides (MC EnderDragon) ────────────────────────────────────
        void AiStep() override;

        // MC hurt(part, ...): every hit is a body hit here (multipart
        // hitboxes skipped) — quarter damage + min(damage, 1), players and
        // explosions only (MC ALWAYS_HURTS_ENDER_DRAGONS is the explosion
        // damage types), the DYING pin, the sitting takeoff rule.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // The DYING pin lives here — see the .cpp.
        void Die(MobDamageSource source, Entity* attacker) override;
        // MC tickDeath — the 200-tick cinematic: float up 0.1/tick, the XP
        // shower (12,000 on a first kill, 500 after), setDragonKilled at 200.
        void TickDeath() override;
        // The cinematic pays the XP itself; zero here keeps MobManager's
        // standard death award from paying the table's 12,000 AGAIN.
        int GetXpReward() const override { return 0; }

        // ── Dragon-fight hooks (MC EnderDragon.onCrystalDestroyed) ────────
        // Called by the fight when a crystal pops: the head takes the 10.0
        // explosion hit if it was the healing crystal, and the current phase
        // gets its reaction (the holding pattern strafes the culprit).
        void OnCrystalDestroyed(EndCrystal& crystal, Entity* player);
        // MC knockback: suppressed while sitting.
        void Knockback(double power, double dx, double dz) override;
        // MC addEffect returns false — nothing sticks to a dragon.
        bool CanBeAffected(const MobEffectInstance& effect) const override {
            (void)effect;
            return false;
        }
        // MC checkDespawn is empty — a dragon never despawns.
        void CheckDespawn() override {}
        bool RemoveWhenFarAway(double) const override { return false; }
        // MC EnderDragon.isPickable: false — the dragon's own 16x8 box is
        // not targetable; only the eight PART boxes are. This port has no
        // part ENTITIES, but the pick paths reproduce the geometry: the
        // client raycasts ComputePartBoxes' output (PlayerController::
        // PickEntity's dragon branch), the part index rides InteractC2S, and
        // the server re-validates it in HandleInteract before routing the
        // hit through HurtPart. Leaving this true instead would make the
        // whole 16-block box clickable, which vanilla never allows.
        bool IsPickable() const override { return false; }
        // MC Entity.kill, as the /kill command reaches it: instant removal
        // plus the fight's victory hand-off — no DYING dive and no XP shower
        // (vanilla's kill() removes before tickDeath ever runs too). Without
        // this the command's discard fallback bypassed setDragonKilled and
        // the fight simply respawned a fresh dragon.
        void KillFromCommand() override;

        // ── The eight sub-entity boxes (MC EnderDragonPart) ───────────────
        //
        // MC's parts are real entities repositioned every aiStep by tickPart;
        // here the same layout is computed on demand from the dragon's own
        // synced state (position, yRot, yRotA, flight history — all present
        // on the client mirror too, which is what lets the client pick
        // against the true geometry). Order and sizes are MC's constructor
        // order: head 1x1, neck 3x3, body 5x3, tail1..3 2x2, wing1/2 4x2.
        static constexpr int kDragonPartCount = 8;
        static constexpr int kDragonPartHead  = 0;
        void ComputePartBoxes(AABB out[kDragonPartCount]) const;

        // MC EnderDragon.hurt(part, source, damage): a head hit takes full
        // damage, everything else the quarter+min(1) body reduction. Public
        // because the server's interact handler routes the picked part here
        // (MC reaches it through the part entity's hurtServer). `direct` is
        // the source's direct entity — a projectile passes itself so the
        // sitting phases can void and ignite it (MC's
        // source.getDirectEntity() instanceof AbstractArrow check).
        bool HurtPart(MobDamageSource source, float amount, Entity* attacker,
                      bool headHit, Entity* direct = nullptr);
        // MC EntityType.ENDER_DRAGON is fireImmune.
        bool FireImmune() const override { return true; }

        // Spawn: fight origin = spawn point, then HOLDING_PATTERN — standing
        // in for EndDragonFight's spawn cycle (DEVIATION, documented in the
        // .cpp: a bare vanilla /summon hovers forever; this port's summoned
        // dragon FLIES).
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void ClearReferenceTo(const Entity* entity) override;

    private:
        // MC getHeadYOffset.
        float GetHeadYOffset() const;
        // MC ServerLevel.getDragonFight() as the dragon sees it — null on the
        // client and outside the End.
        IDragonFight* Fight() const;
        // MC checkCrystals — nearestCrystal upkeep + the 1 HP / 10 ticks heal.
        void CheckCrystals();
        // MC checkWalls over one box: destroys every non-immune block the box
        // overlaps (mobGriefing), returns whether an immune block resisted.
        bool CheckWalls(const AABB& box);
        // MC knockBack / hurt — the wing buffet and the head bite.
        void KnockBackNearby(const AABB& box);
        void HurtNearby(const AABB& box);
        void EnsureNodes();

        glm::ivec3 m_fightOrigin{0};

        // Phase machinery (lazily created instances, MC's phases[] array).
        std::unique_ptr<DragonPhaseInstance>
            m_phases[static_cast<size_t>(DragonPhase::Count)];
        DragonPhaseInstance* m_currentPhase = nullptr;
        uint8_t m_clientPhaseId = static_cast<uint8_t>(DragonPhase::Hovering);

        // MC sittingDamageReceived — the 25%-of-max takeoff accumulator.
        float m_sittingDamageReceived = 0.0f;

        // The 24-node graph (MC nodes/nodeAdjacency), built on first use.
        bool m_nodesBuilt = false;
        glm::ivec3 m_nodes[24];

        // The client's flap-speed movement proxy (the server reads velocity;
        // the client's copy is position-synced, so it measures its own
        // per-tick displacement instead).
        glm::dvec3 m_prevPosForFlap{0.0};
        bool m_prevPosForFlapValid = false;
    };

} // namespace Game
