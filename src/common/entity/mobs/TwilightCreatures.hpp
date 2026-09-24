// File: src/common/entity/mobs/TwilightCreatures.hpp
//
// The rest of the Twilight Forest's biome-spawner creatures (docs/mod-ports.md,
// pass two), ported from the mod's own classes (mods_reference/twilightforest,
// entity/passive and entity/monster). Numbers come from each class's
// registerAttributes and TFEntities' builder rows; goal sets are the mod's
// registerGoals rebuilt from the engine's goal classes, with the mod goals that
// have no engine counterpart either ported here (TFBreathAttackGoal,
// YetiThrowRiderGoal) or named at their site.
//
//   Squirrel       — Animal; skittish (flees players inside 2 blocks), never
//                    breeds.
//   DwarfRabbit    — Animal; three colour variants, breeds on carrots,
//                    golden carrots and dandelions; never despawns.
//   HostileWolf    — Monster; the TF wolf base: a leaping melee hunter of
//                    players, sheep, rabbits, foxes and skeletons.
//   MistWolf       — HostileWolf at 1.9x; its bite blinds in the dark.
//   WinterWolf     — HostileWolf at 1.9x; breathes frost (TF BreathAttackGoal).
//   KingSpider     — MC Spider at 1.9x that does not climb; always carries a
//                    skeleton druid.
//   MosquitoSwarm  — Monster; its bite starves (Hunger).
//   SkeletonDruid  — AbstractSkeleton with a golden hoe: lobs NatureBolts
//                    (poison, bonemeal).
//   Yeti           — Monster; grabs and throws what it catches, grows angry
//                    when hit.
//
// Built by MakeTwilightMob (TwilightMobs.cpp) through MakeTwilightCreature.
#pragma once

#include "common/entity/Animal.hpp"
#include "common/entity/ModMobNbt.hpp"
#include "common/entity/Monster.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace Game {

    struct SpawnRuleContext;

    // This file's half of MakeTwilightMob. Null for any other type.
    std::unique_ptr<Mob> MakeTwilightCreature(EntityTypeId type, EntityLevel* level);

    // ── Squirrel ───────────────────────────────────────────────────────────

    // TF passive/Squirrel. MAX_HEALTH 6, MOVEMENT_SPEED 0.3, STEP_HEIGHT 1.0.
    // isFood false, getBreedOffspring null: squirrels never breed.
    class Squirrel : public Animal {
    public:
        explicit Squirrel(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool IsFood(uint32_t) const override { return false; }
        std::unique_ptr<Animal> CreateBaby() override { return nullptr; }

        // Squirrel.getWalkTargetValue: leaves 12, logs 15, dirt-likes 10.
        float GetWalkTargetValue(const glm::ivec3& pos) const override;

    protected:
        void RegisterGoals() override;
    };

    // ── Dwarf rabbit ───────────────────────────────────────────────────────

    // TF passive/DwarfRabbit. MAX_HEALTH 3, MOVEMENT_SPEED 0.3, STEP_HEIGHT 1.0.
    // DwarfRabbitVariant (twilight/dwarf_rabbit_variant: brown, dutch, white
    // in bootstrap order; none names biomes, so finalizeSpawn's pick is
    // uniform) rides the wire variant byte and saves as the mod's "variant".
    class DwarfRabbit : public Animal {
    public:
        enum Variant : uint8_t { Brown = 0, Dutch = 1, White = 2, VariantCount = 3 };

        explicit DwarfRabbit(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // TFItemTags.DWARF_RABBIT_TEMPT_ITEMS: #c:crops/carrot,
        // golden_carrot, dandelion.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;
        // DwarfRabbit.getBreedOffspring reads the partner's variant.
        void SpawnChildFromBreeding(Animal& partner) override;

        // DwarfRabbit.removeWhenFarAway: never.
        bool RemoveWhenFarAway(double) const override { return false; }

        // DwarfRabbit.getWalkTargetValue: -1 on leaves or logs (no rabbits
        // in the canopy), 10 on dirt-likes, else brightness.
        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        uint8_t GetVariantByte() const override { return m_variant; }
        void    SetVariantByte(uint8_t v) override { m_variant = v < VariantCount ? v : Brown; }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;

    private:
        uint8_t      m_variant = Brown;           // the synced default
        DwarfRabbit* m_breedPartner = nullptr;    // only inside SpawnChildFromBreeding
    };

    // ── Hostile wolves ─────────────────────────────────────────────────────

    // TF monster/HostileWolf. Mob.createMobAttributes + MOVEMENT_SPEED 0.3,
    // MAX_HEALTH 20, ATTACK_DAMAGE 2. The wolf-variant data (always PALE here:
    // nothing rolls another) picks wolf.png / wolf_angry.png by the
    // aggressive flag.
    //
    // The anim byte carries what HostileWolfRenderer reads off the entity:
    // bit 0 = getTarget() != null (isAngry, the tail angle), bit 1 = the
    // winter wolf's BREATH_FLAG.
    class HostileWolf : public Monster {
    public:
        explicit HostileWolf(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override { m_animBits = v; }

        bool HasTargetClient() const { return (m_animBits & 1) != 0; }
        // HostileWolf.getTailAngle: 1.5393804 while it has a target, else PI/5.
        float GetTailAngle() const;

        // HostileWolf.checkWolfSpawnRules: not peaceful, dark enough (or in a
        // hedge maze — no landmark map yet, so dark enough alone), and the
        // mob rules.
        static bool CheckWolfSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos);

    protected:
        HostileWolf(EntityTypeId type, EntityLevel* level);
        void RegisterGoals() override;

        uint8_t m_animBits = 0;   // the client's copy of the anim byte
    };

    // TF monster/MistWolf. HostileWolf attributes with MAX_HEALTH 30,
    // ATTACK_DAMAGE 6. A landed bite in total darkness (brightness 0) and out
    // of any solid block blinds: 7 s normal, 15 s hard, none on easy.
    class MistWolf : public HostileWolf {
    public:
        explicit MistWolf(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool DoHurtTarget(Entity& target) override;
    };

    // TF monster/WinterWolf (IBreathAttacker). HostileWolf attributes with
    // MAX_HEALTH 30, ATTACK_DAMAGE 6, its own goal set with the frost breath.
    class WinterWolf : public HostileWolf {
    public:
        explicit WinterWolf(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // IBreathAttacker.
        bool IsBreathing() const;
        void SetBreathing(bool breathing) { m_breathing = breathing; }
        // WinterWolf.doBreathAttack: BREATH_DAMAGE 2 as a mob attack.
        void DoBreathAttack(Entity& target);

        // WinterWolf.aiStep: the client throws the breath particles.
        void AiStep() override;

        // HostileWolf's bits plus bit 1 = BREATH_FLAG.
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(HostileWolf::GetAnimStateByte() | (m_breathing ? 2 : 0));
        }

        // WinterWolf.canSpawnHere, TF precedence kept:
        // (not peaceful && biome == snowy_forest) || isDarkEnoughToSpawn.
        static bool CheckWinterWolfSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos);

    protected:
        void RegisterGoals() override;

    private:
        bool m_breathing = false;   // server: the goal's flag
    };

    // TF ai/goal/BreathAttackGoal, for the winter wolf: when the last mob to
    // hurt it is in range and in sight, a `chance` roll starts a `duration`
    // tick breath that turns the head onto the target's eyes and, after five
    // ticks, damages whatever the head is looking at each tick.
    class TFBreathAttackGoal : public Goal {
    public:
        TFBreathAttackGoal(WinterWolf* host, float range, int duration, float chance);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "TFBreathAttackGoal"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Entity* GetHeadLookTarget() const;
        void FaceVec(const glm::dvec3& pos, float yawConstraint, float pitchConstraint);
        static bool IsValidTarget(const LivingEntity& e);

        WinterWolf*   m_host;
        LivingEntity* m_target = nullptr;
        glm::dvec3    m_breathPos{0.0};
        int   m_maxDuration;
        float m_attackChance;
        float m_breathRange;
        int   m_durationLeft = 0;
    };

    // ── King spider ────────────────────────────────────────────────────────

    // TF monster/KingSpider extends MC Spider: Spider.createAttributes +
    // MAX_HEALTH 30, MOVEMENT_SPEED 0.35, ATTACK_DAMAGE 6; Spider's goals;
    // onClimbable false (it does not climb walls). finalizeSpawn always puts
    // a skeleton druid on top of whatever rides it.
    class KingSpider : public Spider {
    public:
        explicit KingSpider(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // KingSpider.onClimbable = false: Monster's tick/travel, without
        // Spider's climbing flag or climb clamp.
        void Tick() override;
        void Travel(const glm::dvec3& input) override;

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // KingSpider.getPassengerAttachmentPoint: 15% of the height lower.
        glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const override;
    };

    // ── Mosquito swarm ─────────────────────────────────────────────────────

    // TF monster/MosquitoSwarm. MAX_HEALTH 12, MOVEMENT_SPEED 0.23,
    // ATTACK_DAMAGE 3, STEP_HEIGHT 2.1. A landed bite adds Hunger: 7 s easy,
    // 15 s normal, 30 s hard. Spawns alone, rides nothing.
    class MosquitoSwarm : public Monster {
    public:
        explicit MosquitoSwarm(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool DoHurtTarget(Entity& target) override;
        int  GetMaxSpawnClusterSize() const override { return 1; }
        bool CanRide(const Entity&) const override { return false; }

    protected:
        void RegisterGoals() override;
    };

    // ── Skeleton druid ─────────────────────────────────────────────────────

    // TF monster/SkeletonDruid extends AbstractSkeleton: the skeleton's
    // attributes, goals and daylight burn, but armed with a golden hoe (a
    // stick as a baby — populateDefaultEquipmentSlots), which reassessWeapon
    // Goal turns into RangedAttackGoal(1.25, 60, 5) firing NatureBolts. With
    // the stick it is AbstractSkeleton's melee skeleton (and performRanged
    // Attack does nothing). The baby half is TF's Zombie copy: +50% speed,
    // x2.5 XP, "IsBaby" saved.
    class SkeletonDruid : public Skeleton {
    public:
        explicit SkeletonDruid(EntityLevel* level);

        bool IsBaby() const override { return m_baby; }
        void SetBaby(bool baby) override;
        int  GetXpReward() const override;

        // SkeletonDruid.performRangedAttack.
        void PerformRangedAttack(LivingEntity& target, float power) override;

        // Whether the main hand holds the hoe (adult) or the stick (baby).
        bool HoldsHoe() const { return !m_baby; }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

        // SkeletonDruid.checkDruidSpawnRules: not peaceful, TF's own light
        // test (isValidLightLevel: raw sky light vs nextInt(32), then
        // brightness vs nextInt(12) — vanilla's 8 widened), and the mob rules.
        static bool CheckDruidSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos);

    protected:
        void RegisterGoals() override;

    private:
        // AbstractSkeleton.reassessWeaponGoal, TF's override: the ranged
        // goal while the hoe is in hand, AbstractSkeleton's melee otherwise.
        void ReassessWeaponGoal();

        bool  m_baby = false;
        Goal* m_rangedGoal = nullptr;
        Goal* m_meleeGoal = nullptr;
    };

    // TF projectile/NatureBolt extends TFThrowable: a thrown wheat-seed bolt
    // with gravity 0.003. A living hit takes 2 (TF's LEAF_BRAIN indirect
    // damage) and, off peaceful, Poison (3 s, 7 s on hard); a block hit
    // bone-meals a bonemealable block when mob griefing allows. It rides the
    // wire as EntityTypeId::Snowball (the ZephyrSnowball precedent), so
    // clients draw the snowball sprite; a saved one reloads as a snowball.
    // Not modelled: the happy-villager trail and leaf burst (no such particle
    // kinds yet), and turning solid #druid_projectile_replaceable blocks into
    // birch leaves (TF's block tag is not in this data pack).
    class NatureBolt : public ThrowableProjectile {
    public:
        explicit NatureBolt(EntityLevel* level)
            : ThrowableProjectile(EntityTypeId::Snowball, level) {}

    protected:
        double GetDefaultGravity() const override { return 0.003; }
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHitBlock(const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;
    };

    // ── Yeti ───────────────────────────────────────────────────────────────

    // TF monster/Yeti (IHostileMount). MAX_HEALTH 20, MOVEMENT_SPEED 0.38,
    // ATTACK_DAMAGE 0, FOLLOW_RANGE 4 (+8 while angry). Its only attack is the
    // ThrowRiderGoal: catch the target, carry it a moment, hurl it.
    //
    // Mobs are carried for real (they ride the yeti). Players cannot ride in
    // this engine (Entity.hpp: player mount control is a later wave), so a
    // player is thrown the moment the yeti catches it, with TF's throw
    // vector and its 200-tick per-player throw cooldown.
    //
    // Anim byte: bit 0 = ANGER_FLAG, bit 1 = isVehicle (YetiRenderer's
    // isHoldingEntity — the arms go up).
    class Yeti : public Monster {
    public:
        explicit Yeti(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool IsAngry() const { return m_angry; }
        void SetAngry(bool angry);

        // Yeti.hurtServer: an attacker that is not a creative player angers it.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // Yeti.aiStep: look at what is in its arms.
        void AiStep() override;

        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;
        bool    IsHoldingEntityClient() const { return m_holdingClient; }

        // Yeti.getPassengerAttachmentPoint: 0.4 forward, in its arms.
        glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const override;

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

        // Yeti.normalYetiSpawnHandler (TFEntities' registered predicate):
        // isValidLightLevel — snowy forest always, else vanilla's darkness
        // test — and the mob rules.
        static bool CheckYetiSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos);

    protected:
        void RegisterGoals() override;

    private:
        bool m_angry = false;
        bool m_holdingClient = false;
    };

    // TF ai/goal/ThrowRiderGoal (with the Yeti's sound-only override): a
    // MeleeAttackGoal whose "hit" picks the victim up, holds it 10-39 ticks
    // and throws it (look * 2 horizontally, 0.9 up) when the goal stops. The
    // chase gives up after 80-119 ticks.
    class YetiThrowRiderGoal : public MeleeAttackGoal {
    public:
        YetiThrowRiderGoal(Yeti* yeti, double speedModifier, bool useLongMemory);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "YetiThrowRiderGoal"; }

    protected:
        void CheckAndPerformAttack(LivingEntity& victim) override;

    private:
        // The throw itself (Stop's body, and the player path's).
        void Throw(Entity& rider);

        Yeti* m_yeti;
        int m_throwTimer = 0;
        int m_timeout = 0;
        int m_cooldown = 0;
    };

} // namespace Game
