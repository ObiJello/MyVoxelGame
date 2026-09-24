// File: src/common/entity/mobs/AetherMobs.hpp
//
// The Aether's creatures (docs/mod-ports.md), ported from the mod's own
// classes (mods_reference/aether, entity/passive and entity/monster). Numbers
// come from each class's createMobAttributes and AetherEntityTypes' builder
// rows; goal sets are the mod's registerGoals rebuilt from the engine's goal
// classes, with the mod goals that have no engine counterpart named at their
// site. This file holds the pass-one five and the shared Aether machinery;
// AetherCreatures.hpp holds the pass-two creatures and dungeon mobs.
//
//   Phyg        — WingedAnimal (a winged pig): glides down instead of
//                 falling, breeds on blue berries, takes a saddle.
//   FlyingCow   — WingedAnimal: the same glide and saddle, milked with a
//                 bucket.
//   Sheepuff    — AetherAnimal: grazes aether grass to aether dirt, puffs up
//                 after eating twice (floating), shorn for wool of its dye
//                 colour, dyed with any dye.
//   Cockatrice  — Monster + RangedAttackMob: shoots poison needles, glides.
//   Zephyr      — FlyingMob: the ghast's impulse flight, look and 20-tick
//                 wind-up, firing a ZephyrSnowball that knocks its target
//                 back instead of a fireball.
//
// All have no MobDef row, so they are built through MakeGenericMob's
// promotion switch before the def check, like the Hush's mobs.
//
// The Aether targets MC 1.21.1. Its createMobAttributes suppliers are the
// bare Mob.createMobAttributes (1.21.1's TemptGoal had a fixed 10-block
// range, no TEMPT_RANGE attribute); here the animals take
// CreateAnimalAttributes so the engine's TemptGoal finds its range at the
// same 10 blocks. Aether's FallingRandomStrollGoal (a RandomStrollGoal that
// can pick land targets while airborne) and FallPathNavigation (path updates
// while airborne) have no engine counterpart: the stroll is MC's
// WaterAvoidingRandomStrollGoal (same interval, same 0.001 probability) on
// the ground navigation.
//
// RIDING: every MountableAnimal (phyg, flying cow, moa) takes a saddle, draws
// it, drops it and saves it, exactly as the mod does — but this engine has no
// player mount control yet (Entity.hpp: "player mount control is a later
// wave"), so a saddled animal is not rideable. MountableMob's travel/jump
// half waits on that system.
#pragma once

#include "common/entity/Animal.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Monster.hpp"
#include "common/entity/RangedAttackMob.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/world/block/Blocks.hpp"

#include <cstdint>
#include <memory>

namespace Game {

    class Arrow;
    class JavaRandom;

    // The Aether creatures' factory — MakeGenericMob's promotion switch for
    // this file's types and AetherCreatures'. Null for any other type.
    std::unique_ptr<Mob> MakeAetherMob(EntityTypeId type, EntityLevel* level);

    // The Aether's shared item/block lookups, resolved by slug on first use
    // (Items::Air / BlockID::Air until the Aether pass adds them).
    namespace AetherIds {
        // aether:blue_berry — PHYG/FLYING_COW/SHEEPUFF_TEMPTATION_ITEMS.
        ItemID BlueBerry();
        // aether:aether_grass_block — AETHER_ANIMALS_SPAWNABLE_ON.
        BlockID AetherGrassBlock();
        // aether:aether_dirt — what EatAetherGrassGoal leaves behind.
        BlockID AetherDirt();
        // An item by registry slug (block items included — the pure-item
        // loot tables cannot name them), Items::Air when it does not exist.
        ItemID ItemBySlug(const char* slug);
    }

    // MC WingedAnimal.tick / Cockatrice.tick: a fall is capped at
    // max(GRAVITY * -1.25, maxFall) blocks per tick — with MC's 0.08 gravity
    // and the default -0.1 that is 0.1, a slow glide. Returns true when it
    // clamped (the mods then clear their synced "entity on ground" flag).
    bool ClampWingedFall(LivingEntity& mob, double maxFall = -0.1);

    // The Aether's PoisonNeedle (projectile/PoisonNeedle extends AbstractDart
    // extends AbstractArrow): an arrow with base damage 0.25 that cannot be
    // picked up and inflicts INEBRIATION (500 ticks). It rides the wire as a
    // plain EntityTypeId::Arrow — every client draws MC's arrow — and the
    // inebriation is carried as vanilla POISON for the same 500 ticks: the
    // effect's damage-over-time half (Inebriation hurts 1 every 50 ticks;
    // Poison every 25, and never below 1 HP). The distraction half (random
    // yaw/motion) has no engine effect to ride. Placed at the shooter's eye
    // line minus 0.1, AbstractArrow's shooter constructor.
    std::unique_ptr<Arrow> MakePoisonNeedle(EntityLevel* level, LivingEntity& shooter);

    // WingedBird (the Aether's interface over the cockatrice and moa):
    // animateWings runs on both sides off the synced "entity on ground"
    // flag, and BipedBirdModel.setupWingsAnimation turns it into the flap.
    struct WingedBirdAnim {
        float wingRotation = 0.0f;
        float prevWingRotation = 0.0f;
        float destPos = 0.0f;
        float prevDestPos = 0.0f;

        // WingedBird.animateWings.
        void Step(bool entityOnGround);
        // BipedBirdModel.setupWingsAnimation.
        float Get(float partialTick) const;
    };

    // ── Mountable animals ──────────────────────────────────────────────────

    // Aether AetherAnimal + MountableAnimal: the aether-grass walk
    // preference, the saddle (Saddleable / SaddleLayer / dropEquipment /
    // "Saddled" save key) and the synced DATA_ENTITY_ON_GROUND_ID flag the
    // wing models key on. The anim byte carries bit 0 saddled, bit 1 entity
    // on ground; subclasses add bits above those.
    class MountableAetherAnimal : public Animal {
    public:
        MountableAetherAnimal(EntityTypeId type, EntityLevel* level);

        void Tick() override;
        void JumpFromGround() override;

        // MountableAnimal.mobInteract: food first (Animal), then a saddle on
        // a saddleable animal (SaddleItem.interactLivingEntity). A saddled,
        // unridden animal would seat the player — no player riding here.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MountableAnimal.dropEquipment: the saddle comes off with the body.
        void DropCustomDeathLoot(EntityLevel& level) override;

        // MountableAnimal.isSaddleable: alive and grown (the moa adds
        // isPlayerGrown).
        virtual bool IsSaddleable() const { return IsAlive() && !IsBaby(); }
        bool IsSaddled() const { return m_saddled; }
        void SetSaddled(bool saddled) { m_saddled = saddled; }
        bool IsEntityOnGround() const { return m_entityOnGround; }
        void SetEntityOnGround(bool onGround) { m_entityOnGround = onGround; }

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_saddled ? 1 : 0) | (m_entityOnGround ? 2 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_saddled = (v & 1) != 0;
            m_entityOnGround = (v & 2) != 0;
        }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

        // AetherAnimal.getWalkTargetValue: aether grass below scores 10,
        // anything else the light-level cost.
        float GetWalkTargetValue(const glm::ivec3& pos) const override;

    private:
        bool m_saddled = false;
        bool m_entityOnGround = true;   // DATA_ENTITY_ON_GROUND_ID, defined true
    };

    // Aether WingedAnimal: the glide. Phyg/FlyingCow.isFood is the Aether's
    // *_TEMPTATION_ITEMS tag (aether:blue_berry).
    class WingedAetherAnimal : public MountableAetherAnimal {
    public:
        WingedAetherAnimal(EntityTypeId type, EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;

        void Tick() override;
        // WingedAnimal.getMaxFallDistance: 14 while airborne.
        int GetMaxFallDistance() const override;
    };

    // Aether passive/Phyg. MAX_HEALTH 10, MOVEMENT_SPEED 0.25.
    class Phyg : public WingedAetherAnimal {
    public:
        explicit Phyg(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);
        std::unique_ptr<Animal> CreateBaby() override;

    protected:
        void RegisterGoals() override;
    };

    // Aether passive/FlyingCow. MAX_HEALTH 10, MOVEMENT_SPEED 0.2. Baby box
    // is COW's halved with eye 0.665 (gen_entity_types BABY_EYE).
    class FlyingCow : public WingedAetherAnimal {
    public:
        explicit FlyingCow(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);
        std::unique_ptr<Animal> CreateBaby() override;

        // FlyingCow.mobInteract: a bucket on a grown cow fills with milk.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

    protected:
        void RegisterGoals() override;
    };

    // ── Sheepuff ───────────────────────────────────────────────────────────

    class EatAetherGrassGoal;

    // Aether passive/Sheepuff. MAX_HEALTH 8, MOVEMENT_SPEED 0.23.
    //
    // The wool byte is MC Sheep's layout (colour in the low nibble, bit 4
    // sheared) and rides the wire as the variant byte; the puffed flag is
    // anim byte bit 0. EatAetherGrassGoal grazes short grass or aether grass
    // (to aether dirt); `ate` puffs an unshorn sheepuff after two meals and
    // regrows a shorn one after one. A puffed sheepuff floats (fall capped at
    // max(gravity * -0.625, -0.05)), jumps 1.8 higher and takes no fall
    // damage. Offspring inherit this parent's colour (the engine's sheep
    // precedent — the dye-recipe mix needs the partner, which CreateBaby
    // does not see).
    class Sheepuff : public Animal {
    public:
        explicit Sheepuff(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void Tick() override;
        void AiStep() override;
        void CustomServerAiStep() override;
        void HandleEntityEvent(uint8_t id) override;
        void JumpFromGround() override;
        bool CauseFallDamage(double fallDist, float damageMultiplier) override;
        int  GetMaxFallDistance() const override;

        // Sheepuff.ate (EatAetherGrassGoal's callback).
        void OnEatBlock() override;

        // Sheepuff.mobInteract: dye (two dyes on a puffed one), then shears
        // (IShearable, the Sheep precedent), then Animal.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        bool ReadyForShearing() const { return IsAlive() && !IsSheared() && !IsBaby(); }
        void Shear();

        // Sheepuff.getDefaultLootTable: the sheared table has no wool; the
        // unshorn one adds one wool of its colour — a block item, so it rides
        // this hook (the Bighorn precedent).
        void DropCustomDeathLoot(EntityLevel& level) override;

        uint8_t GetColor() const { return m_woolData & 0x0F; }
        void    SetColor(uint8_t color) {
            m_woolData = static_cast<uint8_t>((m_woolData & 0xF0) | (color & 0x0F));
        }
        bool IsSheared() const { return (m_woolData & 0x10) != 0; }
        void SetSheared(bool sheared) {
            m_woolData = sheared ? static_cast<uint8_t>(m_woolData | 0x10)
                                 : static_cast<uint8_t>(m_woolData & ~0x10);
        }
        bool IsPuffed() const { return m_puffed; }
        void SetPuffed(bool puffed) { m_puffed = puffed; }

        uint8_t GetVariantByte() const override { return m_woolData; }
        void    SetVariantByte(uint8_t v) override { m_woolData = v; }
        uint8_t GetAnimStateByte() const override { return m_puffed ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_puffed = (v & 1) != 0; }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

        // Sheepuff.getHeadEatPositionScale / getHeadEatAngleScale.
        float GetHeadEatPositionScale(float partialTick) const;
        float GetHeadEatAngleScale(float partialTick) const;

        // Sheepuff.getRandomSheepuffColor.
        static uint8_t RandomSheepuffColor(JavaRandom& rng);

    protected:
        void RegisterGoals() override;

    private:
        EatAetherGrassGoal* m_eatGoal = nullptr;
        uint8_t m_woolData = 0;
        bool    m_puffed = false;
        int     m_eatAnimationTick = 0;
        int     m_amountEaten = 0;
    };

    // Aether ai/goal/EatAetherGrassGoal — MC EatBlockGoal over SHORT_GRASS
    // at the mob or aether grass below (turned to aether dirt). The roll is
    // the mod's raw 1-in-1000 (50 for a baby), not adjustedTickDelay'd.
    class EatAetherGrassGoal : public Goal {
    public:
        explicit EatAetherGrassGoal(Sheepuff* mob);

        bool CanUse() override;
        bool CanContinueToUse() override { return m_eatAnimationTick > 0; }
        void Start() override;
        void Stop() override { m_eatAnimationTick = 0; }
        void Tick() override;
        const char* Name() const override { return "EatAetherGrassGoal"; }

        int GetEatAnimationTick() const { return m_eatAnimationTick; }

    private:
        Sheepuff* m_mob;
        int m_eatAnimationTick = 0;
    };

    // ── Cockatrice ─────────────────────────────────────────────────────────

    // Aether monster/Cockatrice. MAX_HEALTH 20, MOVEMENT_SPEED 0.25. A
    // ranged mob: RangedAttackGoal(1.0, 60, 10.0) firing poison needles
    // (performRangedAttack is a [CODE COPY] of AbstractSkeleton's with the
    // needle's origin raised).
    //
    // WingedBird's wing animation runs client-side; "on ground" is the mod's
    // synced DATA_ENTITY_ON_GROUND_ID flag, carried in the anim byte.
    class Cockatrice : public Monster, public RangedAttackMob {
    public:
        explicit Cockatrice(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        void PerformRangedAttack(LivingEntity& target, float power) override;

        void Tick() override;
        void AiStep() override;
        // Cockatrice.getMaxFallDistance: 14 while airborne.
        int GetMaxFallDistance() const override;

        bool IsEntityOnGround() const { return m_entityOnGround; }
        uint8_t GetAnimStateByte() const override { return m_entityOnGround ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_entityOnGround = (v & 1) != 0; }

        float GetWingsAnimation(float partialTick) const { return m_wings.Get(partialTick); }

    protected:
        void RegisterGoals() override;

    private:
        bool           m_entityOnGround = true;   // DATA_ENTITY_ON_GROUND_ID, defined true
        WingedBirdAnim m_wings;
    };

    // ── Zephyr ─────────────────────────────────────────────────────────────

    // Aether monster/Zephyr extends FlyingMob — a [CODE COPY] of MC's Ghast:
    // ZephyrMoveControl, RandomFloatAroundGoal and ZephyrLookGoal are the
    // ghast's, so the engine's GhastMoveControl / RandomFloatAroundGoal /
    // GhastLookGoal drive it (FLYING_SPEED 0.06 makes GhastMoveControl's
    // impulse exactly the Zephyr's literal 0.1). MAX_HEALTH 5, FOLLOW_RANGE
    // 50, xpReward 5.
    class Zephyr : public Mob {
    public:
        explicit Zephyr(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // FlyingMob.checkFallDamage is empty.
        bool CauseFallDamage(double, float) override { return false; }
        // FlyingMob.travel: no gravity; drag 0.91 (0.8 water, 0.5 lava).
        void Travel(const glm::dvec3& input) override;
        // Zephyr.aiStep: discarded below the world floor or over the top;
        // the client animates the tail and the charge puff.
        void AiStep() override;

        int  GetChargeTime() const { return m_chargeTime; }
        void SetChargeTime(int t) { m_chargeTime = t; }

        // DATA_CHARGE_TIME_ID reaches the client only through the renderer's
        // test `0 < chargeTime < 20` (Zephyr.aiStep), so the anim byte carries
        // that one bit.
        uint8_t GetAnimStateByte() const override {
            return (m_chargeTime > 0 && m_chargeTime < 20) ? 1 : 0;
        }
        void SetAnimStateByte(uint8_t v) override { m_chargingClient = (v & 1) != 0; }

        // The Aether's zephyr table drops 0-2 aether:cold_aercloud — a
        // BLOCK item, which the baked loot tables cannot carry (the
        // Hushling's sculk precedent), so zephyr.json has no pools and the
        // drop rides this hook, resolved by slug. Looting's +0-1 is not
        // modelled (no enchanted weapons on mob kills).
        void DropCustomDeathLoot(EntityLevel& level) override;

        // ZephyrRenderer.scale's input (lerped cloudScale) and getBob's tail
        // rotation — client-side animation state.
        float GetCloudScale(float partialTick) const {
            return static_cast<float>(m_cloudScale) + static_cast<float>(m_cloudScaleAdd) * partialTick;
        }
        float GetTailRot(float partialTick) const { return m_tailRot + m_tailRotAdd * partialTick; }

    protected:
        void RegisterGoals() override;

    private:
        int   m_chargeTime = 0;          // server: ZephyrShootSnowballGoal's counter
        bool  m_chargingClient = false;  // client: the synced bit
        int   m_cloudScale = 0;
        int   m_cloudScaleAdd = 0;
        float m_tailRot = 0.0f;
        float m_tailRotAdd = 0.0f;
    };

    // Aether Zephyr.ZephyrShootSnowballGoal — a [CODE COPY] of MC's
    // GhastShootFireballGoal (40-block range instead of 64, and a
    // ZephyrSnowball instead of a LargeFireball). Engine-only goal: MC's
    // version is bound to Ghast.
    class ZephyrShootSnowballGoal : public Goal {
    public:
        explicit ZephyrShootSnowballGoal(Zephyr* zephyr) : m_zephyr(zephyr) {}

        bool CanUse() override;
        void Start() override { m_zephyr->SetChargeTime(0); }
        void Stop() override { m_zephyr->SetChargeTime(0); }
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "ZephyrShootSnowballGoal"; }

    private:
        Zephyr* m_zephyr;
    };

    // Aether projectile/ZephyrSnowball extends Fireball: an unburning
    // hurting projectile (acceleration 0.1, inertia 0.95) that lives 400
    // ticks in the air and, on an entity, deals no damage but throws it —
    // +0.5 up and 1.5x the snowball's horizontal velocity. It rides the
    // wire as EntityTypeId::Snowball, so every client draws MC's snowball
    // sprite (the item the Aether's getItem returns); a client-side copy is
    // a plain Snowball whose path the server corrects. Not modelled: the
    // shield-damage branch (no shield blocking) and the sentry-boots
    // exemption (no such item). A saved one reloads as a plain snowball.
    class ZephyrSnowball : public HurtingProjectile {
    public:
        explicit ZephyrSnowball(EntityLevel* level)
            : HurtingProjectile(EntityTypeId::Snowball, level) {}

        void Tick() override;

    protected:
        bool ShouldBurn() const override { return false; }
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;

    private:
        int m_ticksInAir = 0;
    };

} // namespace Game
