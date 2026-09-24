// File: src/common/entity/mobs/TwilightHostiles.hpp
//
// The Twilight Forest's landmark hostiles (docs/mod-ports.md, pass two),
// ported from the mod's own classes (mods_reference/twilightforest,
// entity/monster). Numbers come from each class's registerAttributes and
// TFEntities' builder rows; goal sets are the mod's registerGoals rebuilt from
// the engine's goal classes, with the TF goals that have no engine
// counterpart ported beside them (TwilightHostiles.cpp) or named at their
// site.
//
//   HedgeSpider        — MC Spider that hunts in daylight (hedge maze).
//   SwarmSpider        — small spider, 1-in-4 bites land, packs of 6, a rare
//                        skeleton-druid jockey.
//   Wraith             — flying no-clip ghost, home-bound when placed by a
//                        structure/spawner.
//   FireBeetle         — flame breath (BreathAttackGoal) that sets targets
//                        alight.
//   SlimeBeetle        — keeps its distance and lobs slime (SlimeProjectile).
//   PinchBeetle        — charges and clamps its victim in its jaws (a mob
//                        victim rides it; see the class).
//   HelmetCrab         — armoured crab, 1-in-10000 blue shell.
//   Troll              — pries stone out of the ground and throws it.
//   TowerwoodBorer     — the Dark Tower's silverfish.
//   MazeSlime          — slime with triple health that always hurts.
//   Minotaur           — charges (ChargeAttackGoal), swings a golden axe.
//   RedcapSapper       — the armoured redcap (TNT goals left out, like the
//                        redcap's).
//   BlockChainGoblin   — swings a spiked ball on a chain around itself and
//                        throws it.
//   UpperGoblinKnight  — rides the lower knight; shield, breastplate, heavy
//                        spear slam.
//   LowerGoblinKnight  — the mount, spawned with its rider.
//
// None has a MobDef row (no MC class for the def generator to read); all are
// built through MakeTwilightHostile, which MakeGenericMob's promotion step
// calls before the def check.
//
// Synched state rides the anim byte / variant byte (Mob.hpp); each class
// names the MC EntityDataAccessor it carries.
#pragma once

#include "common/entity/ModMobNbt.hpp"
#include "common/entity/Monster.hpp"
#include "common/entity/RangedAttackMob.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Slime.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>

namespace Game {

    class Goal;
    class MeleeAttackGoal;
    class RangedAttackGoal;

    // This file's factory — MakeGenericMob's promotion switch for the
    // landmark hostiles. Null for any other type.
    std::unique_ptr<Mob> MakeTwilightHostile(EntityTypeId type, EntityLevel* level);

    // ── Spiders ────────────────────────────────────────────────────────────

    // TF HedgeSpider extends Spider: MC's spider with the light-shy halves
    // replaced — MeleeAttackGoal(1, true) at 4 instead of SpiderAttackGoal,
    // NearestAttackableTargetGoal(Player, true) at target 2 instead of
    // SpiderTargetGoal. Spider.createAttributes, unchanged.
    class HedgeSpider : public Spider {
    public:
        explicit HedgeSpider(EntityLevel* level);

        // HedgeSpider.isValidLightLevel: a hedge-maze chunk OR
        // Monster.isDarkEnoughToSpawn. The landmark test needs TF's legacy
        // landmark grid, which the TF generator does not expose to the
        // spawner, so only the light half is answered.
        static bool IsValidLightLevel(EntityLevel& level, const glm::ivec3& pos, JavaRandom& rng);

    protected:
        HedgeSpider(EntityTypeId type, EntityLevel* level);
        void RegisterGoals() override;
    };

    // TF SwarmSpider: MAX_HEALTH 3, ATTACK_DAMAGE 1 on Spider's attributes,
    // xpReward 2 (the type row). Only one bite in four lands; spawns in packs
    // of up to 6; finalizeSpawn's summonJockey mounts a baby skeleton druid
    // (on the spider jockey's skeleton seat, or 1-in-200 otherwise).
    class SwarmSpider : public HedgeSpider {
    public:
        explicit SwarmSpider(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool DoHurtTarget(Entity& target) override;
        int  GetMaxSpawnClusterSize() const override { return 6; }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;
    };

    // ── Wraith ─────────────────────────────────────────────────────────────

    // TF Wraith extends Mob (Enemy): MAX_HEALTH 20, MOVEMENT_SPEED 0.5,
    // ATTACK_DAMAGE 5. noPhysics — it drifts through walls under
    // NoClipMoveControl; travelFlying(input, 0.2) without collision. Placed by
    // a structure or a spawner it is leashed to a 20-block home
    // (EnforcedHomePoint — the engine's Mob home; saved with the vanilla
    // home_pos/home_radius fields).
    class Wraith : public Mob {
    public:
        explicit Wraith(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        static constexpr int kHomeRadius = 20;   // Wraith.getHomeRadius

        // TFEntities.WRAITH: fireImmune().
        bool FireImmune() const override { return true; }
        bool CauseFallDamage(double, float) override { return false; }
        void Travel(const glm::dvec3& input) override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        bool DoHurtTarget(Entity& target) override;

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // Wraith.checkMonsterSpawnRules minus Mob.checkMobSpawnRules (the
        // spawner's floor test composes that half).
        static bool CheckSpawnRules(EntityLevel& level, const glm::ivec3& pos, JavaRandom& rng);

    protected:
        void RegisterGoals() override;
    };

    // ── Beetles ────────────────────────────────────────────────────────────

    // TF IBreathAttacker.
    class BreathAttacker {
    public:
        virtual ~BreathAttacker() = default;
        virtual bool IsBreathing() const = 0;
        virtual void SetBreathing(bool breathing) = 0;
        virtual void DoBreathAttack(Entity& target) = 0;
    };

    // TF FireBeetle: MAX_HEALTH 25, MOVEMENT_SPEED 0.23, ATTACK_DAMAGE 4;
    // fireImmune. BreathAttackGoal(5, 30, 0.1): 30 ticks of flame at the
    // last attacker within 5 blocks, 2 damage and 10 s of fire per tick after
    // the 5-tick wind-up. The BREATHING flag is the anim byte's bit 0; the
    // client throws the flame particles from it (FireBeetle.aiStep).
    class FireBeetle : public Monster, public BreathAttacker {
    public:
        explicit FireBeetle(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        static constexpr int   kBreathDuration = 10;   // seconds of fire
        static constexpr float kBreathDamage   = 2.0f;

        bool FireImmune() const override { return true; }
        int  GetMaxHeadXRot() const override { return 500; }

        bool IsBreathing() const override { return m_breathing; }
        void SetBreathing(bool breathing) override { m_breathing = breathing; }
        void DoBreathAttack(Entity& target) override;
        bool DoHurtTarget(Entity& target) override;

        void AiStep() override;

        uint8_t GetAnimStateByte() const override { return m_breathing ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_breathing = (v & 1) != 0; }

    protected:
        void RegisterGoals() override;

    private:
        bool m_breathing = false;
    };

    // TF SlimeBeetle: MAX_HEALTH 25, MOVEMENT_SPEED 0.23, ATTACK_DAMAGE 4.
    // Backs off from a player inside 3 blocks and lobs SlimeProjectiles —
    // RangedAttackGoal(1, 30, 10).
    class SlimeBeetle : public Monster, public RangedAttackMob {
    public:
        explicit SlimeBeetle(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        void PerformRangedAttack(LivingEntity& target, float power) override;

    protected:
        void RegisterGoals() override;
    };

    // TF SlimeProjectile (TFThrowable): gravity 0.006, 4 damage to a living
    // target, gone on any hit. Rides the wire as EntityTypeId::Snowball (the
    // ZephyrSnowball precedent), so clients draw the snowball sprite where TF
    // draws a slime ball; a saved one reloads as a plain snowball.
    class SlimeProjectile : public ThrowableProjectile {
    public:
        explicit SlimeProjectile(EntityLevel* level)
            : ThrowableProjectile(EntityTypeId::Snowball, level) {}

    protected:
        double GetDefaultGravity() const override { return 0.006; }
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;
    };

    // TF ITFCharger — the Minotaur's CHARGING flag, which ChargeAttackGoal
    // raises during the wind-up.
    class TFCharger {
    public:
        virtual ~TFCharger() = default;
        virtual void SetCharging(bool charging) = 0;
    };

    // TF PinchBeetle (IHostileMount): MAX_HEALTH 40, MOVEMENT_SPEED 0.23,
    // ATTACK_DAMAGE 4, ARMOR 2. A landed bite plucks the victim out of its
    // seat and clamps it in the jaws (the victim rides the beetle), after
    // which the beetle keeps it as its target and takes no knockback; the box
    // grows to 2.2 x 1.6 while it holds something.
    //
    // A PLAYER victim is not seized: players are client-authoritative and
    // not Entities in this engine, so a player cannot be made a passenger.
    // The bite still lands as normal melee damage (CLAMPED).
    class PinchBeetle : public Monster {
    public:
        explicit PinchBeetle(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        void AiStep() override;
        bool DoHurtTarget(Entity& target) override;
        void Knockback(double power, double dx, double dz) override;
        void Die(MobDamageSource source, Entity* attacker) override;
        glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const override;

        float BaseBbWidth() const override;
        float BaseBbHeight() const override;
        float BaseEyeHeight() const override;

        // PinchBeetleRenderer: isHoldingVictim = isVehicle — anim byte bit 0.
        bool IsHoldingVictim() const { return m_holdingClient; }
        uint8_t GetAnimStateByte() const override { return IsVehicle() ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_holdingClient = (v & 1) != 0; }

    protected:
        void RegisterGoals() override;

    private:
        bool m_holdingClient = false;
    };

    // TF HelmetCrab: MAX_HEALTH 13, MOVEMENT_SPEED 0.28, ATTACK_DAMAGE 3,
    // ARMOR 6; fireImmune. The helmet lags the head turn (helmetRot follows
    // yHeadRotO — both sides, for the renderer). BLUE (1 in 10000 at spawn)
    // is the variant byte and saves as "blue".
    class HelmetCrab : public Monster {
    public:
        explicit HelmetCrab(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool FireImmune() const override { return true; }
        void Tick() override;

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        bool IsBlue() const { return m_blue; }
        uint8_t GetVariantByte() const override { return m_blue ? 1 : 0; }
        void    SetVariantByte(uint8_t v) override { m_blue = v != 0; }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

        // HelmetCrabRenderState.getHelmetRotation.
        float GetHelmetRotation(float partialTick) const;

    protected:
        void RegisterGoals() override;

    private:
        bool  m_blue = false;
        float m_helmetRot = 0.0f;
        float m_helmetRotO = 0.0f;
    };

    // ── Troll ──────────────────────────────────────────────────────────────

    // TF Troll: MAX_HEALTH 30, MOVEMENT_SPEED 0.25, ATTACK_DAMAGE 7. With a
    // target and its cooldown (300-399 ticks) spent, it pries a block of
    // #base_stone_overworld out of the ground within 2 blocks and holds it
    // overhead (ROCK_FLAG — the anim byte's bit 0; +8 FOLLOW_RANGE while
    // held), swapping its melee goal for RangedAttackGoal(1, 20, 60, 15).
    // The throw is a ThrownBlock (6 damage).
    //
    // Not modelled: the held rock's riding ThrownBlock entity (the arms-up
    // pose is drawn, the block on the troll's head is not) and the block's
    // own look in flight (it rides the wire as a snowball); the trollber
    // ripening on death waits on the trollber blocks (resolved by slug, a
    // no-op until they exist).
    class Troll : public Monster, public RangedAttackMob {
    public:
        explicit Troll(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        void Tick() override;
        void TickDeath() override;
        void PerformRangedAttack(LivingEntity& target, float power) override;

        bool HasRock() const { return m_hasRock; }
        uint8_t GetAnimStateByte() const override { return m_hasRock ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_hasRock = (v & 1) != 0; }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;

    private:
        void SetHasRock(bool rock);
        // Troll.setCombatTask — melee without a rock, the throw with one.
        void SetCombatTask();
        void RipenTrollBerNearby(int offset);

        bool       m_hasRock = false;
        int        m_rockCooldown = 0;
        BlockState m_rock{};
        Goal*      m_meleeGoal = nullptr;
        Goal*      m_rangedGoal = nullptr;
    };

    // TF ThrownBlock (TFThrowable): the troll's rock — 6 damage to any
    // living thing but a troll, gone on any hit. Rides the wire as
    // EntityTypeId::Snowball (see the Troll note).
    class ThrownBlock : public ThrowableProjectile {
    public:
        ThrownBlock(EntityLevel* level, BlockState state)
            : ThrowableProjectile(EntityTypeId::Snowball, level), m_state(state) {}

    protected:
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;

    private:
        BlockState m_state;
    };

    // ── Towerwood borer ────────────────────────────────────────────────────

    // TF TowerwoodBorer — MC Silverfish for the Dark Tower: MAX_HEALTH 15,
    // MOVEMENT_SPEED 0.27, ATTACK_DAMAGE 5, FOLLOW_RANGE 8. A hurt borer
    // wakes the infested towerwood within 10 blocks (SummonBorersGoal); an
    // idle one burrows into towerwood (HideInTowerwoodGoal). Both resolve
    // the TF blocks by slug and do nothing until those blocks exist.
    // ClimbOnTopOfPowderSnowGoal has no engine counterpart.
    class TowerwoodBorer : public Monster {
    public:
        explicit TowerwoodBorer(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void Tick() override;

    protected:
        void RegisterGoals() override;

    private:
        class SummonBorersGoal* m_summonBorers = nullptr;
    };

    // ── Maze slime ─────────────────────────────────────────────────────────

    // TF MazeSlime extends Slime: setSize adds DOUBLE_HEALTH (+2 x base, an
    // ADD_MULTIPLIED_BASE modifier — three times the slime's size^2) and
    // pays size + 3 XP; isDealsDamage is always true (a size-1 maze slime
    // bites too). Its splits are maze slimes.
    class MazeSlime : public Slime {
    public:
        explicit MazeSlime(EntityLevel* level);

        void SetSize(int size, bool resetHealth) override;
        int  GetXpReward() const override { return m_size + 3; }
        bool DealsDamage() const override { return IsEffectiveAi(); }

        // MazeSlime.getCanSpawnHere minus Mob.checkMobSpawnRules.
        static bool CheckSpawnRules(EntityLevel& level, const glm::ivec3& pos, JavaRandom& rng);

    protected:
        std::unique_ptr<Slime> MakeSplitChild() override {
            return std::make_unique<MazeSlime>(m_level);
        }
    };

    // ── Minotaur ───────────────────────────────────────────────────────────

    // TF Minotaur: MAX_HEALTH 30, MOVEMENT_SPEED 0.25. Spawns holding a
    // golden axe (1 in 10/(difficulty+1): TF's golden minotaur axe); with no
    // mob equipment here, the golden axe's +6 attack is folded into
    // ATTACK_DAMAGE (Monster's 2 + 6 = 8) and the renderer draws the axe.
    // ChargeAttackGoal(1.5): a 15-44 tick wind-up, then a sprint through the
    // target. CHARGING is the anim byte's bit 0.
    class Minotaur : public Monster, public TFCharger {
    public:
        explicit Minotaur(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        void SetCharging(bool charging) override { m_charging = charging; }
        bool IsCharging() const { return m_charging; }
        void AiStep() override;

        uint8_t GetAnimStateByte() const override { return m_charging ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_charging = (v & 1) != 0; }

    protected:
        void RegisterGoals() override;

    private:
        bool m_charging = false;
    };

    // ── Redcap sapper ──────────────────────────────────────────────────────

    // TF RedcapSapper extends Redcap: MAX_HEALTH 30, ARMOR 2 on the redcap's
    // attributes (MOVEMENT_SPEED 0.28; the held ironwood pickaxe folded into
    // ATTACK_DAMAGE exactly as the redcap's iron one is — 5). The redcap's
    // goal set; the TNT goals (RedcapPlantTNTGoal, and the redcap's own
    // three) are left out as they are on the redcap. The ironwood boots'
    // armour is not modelled. Built standalone rather than on Game::Redcap,
    // whose only constructor fixes the type id.
    class RedcapSapper : public Monster {
    public:
        explicit RedcapSapper(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
    };

    // ── Block-and-chain goblin ─────────────────────────────────────────────

    // TF BlockChainGoblin: MAX_HEALTH 20, MOVEMENT_SPEED 0.28, ATTACK_DAMAGE
    // 8, ARMOR 11. The spiked ball (TF's SpikeBlock part, 0.75 x 0.75) circles
    // it at 16 degrees a tick — 0.9 blocks out while it swings at a target,
    // 0.3 otherwise — and ThrowSpikeBlockGoal flings it up to 6 blocks along
    // the view. Anything pushable the ball touches while swinging or thrown
    // takes the goblin's melee hit and a 0.4 upward shove, which starts a
    // 40-tick recoil. Synced: chain angle (anim byte, 0-255 of a turn) and
    // chain length x127 + the THROWING bit 0x80 (variant byte); the client
    // runs the same ball placement for the renderer.
    //
    // Not modelled: AvoidAnyEntityGoal on primed TNT (the engine's avoid goal
    // takes living types only); the ball is not a separate hittable part.
    class BlockChainGoblin : public Monster {
    public:
        explicit BlockChainGoblin(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        static constexpr float kChainSpeed = 16.0f;
        // SpikeBlock's dimensions.
        static constexpr float kBallSize = 0.75f;

        void Tick() override;

        bool  IsThrowing() const { return m_throwing; }
        void  SetThrowing(bool throwing) { m_throwing = throwing; }
        float GetChainMoveLength() const { return m_chainMoveLength; }
        bool  IsSwingingChain() const;

        // The ball's feet position relative to the goblin's, lerped
        // (BlockChainGoblinRenderer: entity.block.getPosition(partialTick)).
        glm::dvec3 GetBallOffset(float partialTick) const {
            return m_ballOffsetO + (m_ballOffset - m_ballOffsetO) * static_cast<double>(partialTick);
        }

        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;
        uint8_t GetVariantByte() const override;
        void    SetVariantByte(uint8_t v) override;

    protected:
        void RegisterGoals() override;

    private:
        float GetChainAngle() const;
        float GetChainLength() const;
        void  ApplyBlockCollisions();

        int   m_recoilCounter = 0;
        float m_chainAngle = 0.0f;
        float m_chainMoveLength = 0.0f;
        bool  m_throwing = false;
        // Client copies of DATA_CHAINLENGTH / DATA_CHAINPOS.
        uint8_t m_syncedChainLength = 0;
        uint8_t m_syncedChainPos = 0;

        glm::dvec3 m_ballOffset{0.0};
        glm::dvec3 m_ballOffsetO{0.0};
    };

    // ── Goblin knights ─────────────────────────────────────────────────────

    // TF UpperGoblinKnight: MAX_HEALTH 30, MOVEMENT_SPEED 0.28, ATTACK_DAMAGE
    // 8; spawns with armour (+20 ARMOR) and a shield. Half its melee swings
    // turn into a heavy spear slam instead: a 60-tick timer (entity event 4
    // starts the client's copy) that lands at tick 25 — +12 damage to
    // everything in a 1.5-block box 1.25 ahead, with 50 large smoke puffs.
    // Hits from the front strike the shield (an axe disables it for 100
    // ticks, >10 damage or three hits break it); hits from behind break the
    // breastplate. Losing its mount breaks the shield. Anim byte: armour 1,
    // shield 2, shield disabled 4.
    class UpperGoblinKnight : public Monster {
    public:
        explicit UpperGoblinKnight(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        static constexpr int kHeavySpearTimerStart = 60;
        static constexpr float kShieldDamageThreshold = 10.0f;

        bool HasArmor() const { return (m_equip & 1) != 0; }
        bool HasShield() const { return (m_equip & 2) != 0; }
        bool IsShieldDisabled() const { return m_shieldDisabled; }
        int  GetHeavySpearTimer() const { return m_heavySpearTimer; }

        void AiStep() override;
        void CustomServerAiStep() override;
        void HandleEntityEvent(uint8_t id) override;
        bool DoHurtTarget(Entity& target) override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // UpperGoblinKnight.landHeavySpearAttack.
        void LandHeavySpearAttack();
        // UpperGoblinKnight.takeHitOnShield — true when the shield took it.
        bool TakeHitOnShield(Entity* attacker, float amount);

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(m_equip | (m_shieldDisabled ? 4 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_equip = static_cast<uint8_t>(v & 3);
            m_shieldDisabled = (v & 4) != 0;
        }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;

    private:
        void SetHasArmor(bool flag);
        void SetHasShield(bool flag);
        void BreakArmor();
        void BreakShield();
        void DamageShield();

        uint8_t m_equip = 0;
        bool    m_shieldDisabled = false;
        int     m_shieldHits = 0;
        int     m_shieldDisabledTicks = 0;
        int     m_heavySpearTimer = 0;
    };

    // TF LowerGoblinKnight: MAX_HEALTH 20, MOVEMENT_SPEED 0.28, ATTACK_DAMAGE
    // 4; spawns armoured (+17 ARMOR) and carrying an upper knight
    // (finalizeSpawn). Its bites go through the rider's doHurtTarget, and
    // it stands still while the rider's spear slam runs
    // (RiderSpearAttackGoal). Anim byte: armour 1, has rider 2.
    class LowerGoblinKnight : public Monster {
    public:
        explicit LowerGoblinKnight(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool HasArmor() const { return m_hasArmor; }
        bool HasUpperGoblinClient() const { return m_hasUpperClient; }
        UpperGoblinKnight* GetUpper() const;

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        bool DoHurtTarget(Entity& target) override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void HandleEntityEvent(uint8_t id) override;
        glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const override;

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_hasArmor ? 1 : 0) | (GetUpper() ? 2 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_hasArmor = (v & 1) != 0;
            m_hasUpperClient = (v & 2) != 0;
        }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;

    private:
        void SetHasArmor(bool flag);

        bool m_hasArmor = false;
        bool m_hasUpperClient = false;
    };

} // namespace Game
