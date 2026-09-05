// File: src/common/entity/mobs/SulfurCube.hpp
//
// MC 26.3 net.minecraft.world.entity.monster.cubemob.SulfurCube — the
// sulfur cube, ported standalone (no sulfur-caves biome, no sulfur blocks,
// no bucket): /summon, /spawnall and its spawn egg are how one appears.
//
// It is a cube mob (26.3 split Slime into AbstractCubeMob + Slime /
// MagmaCube / SulfurCube; this engine's Slime IS that base, so the cube
// derives from it the way MagmaCube does) crossed with an AgeableMob: size
// 1 is a baby, size 2 the adult, health 4·size, it never attacks, splits
// into exactly two babies, and grows up on slime balls.
//
// What makes it a sulfur cube is the BLOCK IT SWALLOWS. Right-click it with
// (or let it hop onto) a block from `#sulfur_cube_swallowable` and the block
// sits inside it: the cube stops moving on its own, becomes a physics
// object whose archetype (data/minecraft/sulfur_cube_archetype/*.json →
// GeneratedSulfurCubeArchetypes) sets its knockback resistance, bounciness,
// friction and air drag — hit it and it flies, bounces and slides like the
// block it holds — plus a buoyancy flag, an explosion (TNT: a 120-tick fuse
// lit by fire, redstone, flint and steel or a fire charge) or contact
// damage (magma). Shears pop the block back out. A player walking into a
// laden cube shoves it (playerPush). Every number is MC's.
#pragma once

#include "common/entity/mobs/Slime.hpp"
#include "common/entity/mobs/GeneratedSulfurCubeArchetypes.hpp"
#include "common/entity/Item.hpp"
#include "common/data/DataComponents.hpp"

namespace Game {

    class SulfurCube;

    // MC SulfurCube.SulfurCubeMobMoveControl: the hop controller sits still
    // while the cube holds a block — it is pushed around instead.
    class SulfurCubeMoveControl : public SlimeMoveControl {
    public:
        explicit SulfurCubeMoveControl(SulfurCube* cube);
        void Tick() override;
    private:
        SulfurCube* m_cube;
    };

    // MC SulfurCube.SulfurCubeLookControl: with a block inside, the body
    // snaps to the nearest 180° instead of looking around, so the swallowed
    // block reads axis-aligned.
    class SulfurCubeLookControl : public LookControl {
    public:
        explicit SulfurCubeLookControl(SulfurCube* cube);
        void Tick() override;
    private:
        SulfurCube* m_cube;
    };

    class SulfurCube : public Slime {
    public:
        static constexpr int    kSplitCount           = 2;
        static constexpr int    kMaxSize              = 2;
        static constexpr int    kMinSize              = 1;
        static constexpr int    kPickupTimerDuration  = 100;
        static constexpr double kPushDistanceThreshold = 1.2999999523162842;
        static constexpr double kMaxPlayerPushSpeed   = 0.5;
        static constexpr float  kPlayerPushSpeedScale = 0.3f;
        static constexpr float  kVehiclePushSpeedScale = 0.16f;
        static constexpr float  kVerticalPushMultiplier = 0.3f;
        static constexpr float  kHorizontalHitAngleScale = 1.6f;
        static constexpr float  kVerticalHitAngleScale   = 0.5f;
        static constexpr float  kVerticalPositionAngleScale = 0.8f;
        // AgeableMob.BABY_START_AGE.
        static constexpr int    kBabyStartAge         = -24000;
        // SulfurCubeArchetype.DEFAULT_KNOCKBACK_MODIFIERS / DEFAULT_SOUND_SETTINGS.
        static constexpr float  kDefaultKnockbackH    = 0.33f;
        static constexpr float  kDefaultKnockbackV    = 0.06f;
        static constexpr float  kDefaultPushThreshold = 0.2f;
        static constexpr float  kDefaultPushCooldown  = 0.5f;
        // createSulfurCubeAttributes: TEMPT_RANGE 8.
        static constexpr double kTemptRange           = 8.0;

        explicit SulfurCube(EntityLevel* level);

        // ── The AgeableMob half (MC SulfurCube extends AbstractCubeMob
        //    extends AgeableMob; this engine's Slime is a plain Mob) ──────
        bool IsBaby() const override { return m_age < 0; }
        void SetBaby(bool baby) override;
        int  GetAge() const { return m_age; }
        void SetAge(int age) { m_age = age; }
        int  GetForcedAge() const { return m_forcedAge; }
        void SetForcedAge(int age) { m_forcedAge = age; }
        bool IsAgeLocked() const { return m_ageLocked; }
        void SetAgeLocked(bool locked) { m_ageLocked = locked; }
        // MC AgeableMob.canAgeUp: a baby that is not age-locked.
        bool CanAgeUp() const { return IsBaby() && !m_ageLocked; }
        // MC AgeableMob.ageUp(seconds, forced), the Animal port's twin.
        void AgeUp(int seconds, bool forced);

        // ── The cube ──────────────────────────────────────────────────────
        // MC SulfurCube.setSize: 4·size health (setCubeMobHealth), and size
        // 1 with updateHealth makes it a baby.
        void SetSize(int size, bool updateHealth) override;
        // EntityTypes: sized(0.49, 0.49).eyeHeight(0.175), scaled by size
        // (AbstractCubeMob.getDefaultDimensions).
        float BaseBbWidth()   const override { return 0.49f  * static_cast<float>(GetSize()); }
        float BaseBbHeight()  const override { return 0.49f  * static_cast<float>(GetSize()); }
        float BaseEyeHeight() const override { return 0.175f * static_cast<float>(GetSize()); }
        // canDealDamage → false: a sulfur cube never bites.
        bool DealsDamage() const override { return false; }
        // getBaseExperienceReward: 0 as a baby, else 1 + rand(2).
        int  GetXpReward() const override;
        int  GetSplitCount() override { return IsPrimed() ? 0 : kSplitCount; }
        // MC SulfurCube.setSpawnSize: a fresh cube is size 2 (1 as a baby) —
        // never the slime's 1/2/4 roll AbstractCubeMob defaults to.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // ── MC Bucketable ─────────────────────────────────────────────────
        // saveToBucketTag: the swallowed block (SULFUR_CUBE_CONTENT), age and
        // age lock (bucket_entity_data), plus NoAI from the default tag.
        void SaveToBucket(ItemStack& bucket) const;
        // loadFromBucketTag on a cube a MobBucketItem just spawned.
        void LoadFromBucket(const SulfurCubeBucketData& data);

        // ── The swallowed block (MC: the BODY equipment slot) ─────────────
        ItemID  GetBodyItem()  const { return m_bodyItem; }
        bool    HasBodyItem()  const { return m_bodyItem != Items::Air; }
        BlockID GetBodyBlock() const;
        const SulfurCubeArchetypeDef* Archetype() const { return m_archetype; }
        // MC equipItem: swallow `item` (spitting out the one it held). False
        // for a baby or the same block it already holds.
        bool EquipItem(ItemID item);
        // Load/wire setter: takes the block as is, no drop, no sound.
        void SetBodyItem(ItemID item);
        // MC Shearable: shears eject the block in front of the cube and it
        // ignores dropped blocks for 100 ticks.
        bool ReadyForShearing() const { return HasBodyItem(); }
        void Shear();
        static const SulfurCubeArchetypeDef* ArchetypeFor(ItemID item);
        static bool IsSwallowable(ItemID item) { return ArchetypeFor(item) != nullptr; }
        static bool IsFood(ItemID item);

        // ── The fuse (explosive archetype: TNT inside) ────────────────────
        bool IsPrimed() const { return m_fuse >= 0; }
        int  GetFuse()  const { return m_fuse; }
        int  GetMaxFuse() const { return m_maxFuse; }
        bool CanExplode() const;
        // MC primeTime: light the fuse — a short random one when `imminent`
        // (set off by another explosion). False when it cannot explode, is
        // already lit, or tntExplodes is off.
        bool PrimeTime(bool imminent);

        int  GetPickupTimer() const { return m_pickupTimer; }
        void SetPickupTimer(int t) { m_pickupTimer = t; }
        bool FromBucket() const { return m_fromBucket; }
        void SetFromBucket(bool v) { m_fromBucket = v; }
        bool FloatsInLiquids() const { return m_floatsInLiquids; }

        // ── Wire ──────────────────────────────────────────────────────────
        // Size rides the variant byte, as the slime's does.
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(GetSize()); }
        void    SetVariantByte(uint8_t v) override;
        // MC MAX_FUSE (synched at priming; the client counts down itself):
        // 0 = never lit, else maxFuse + 1. Constant while burning, so the
        // tracker sends it once.
        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;
        // The swallowed block's state, for the carried-block field.
        uint32_t GetCarriedBlockRaw() const override;
        void     SetCarriedBlockRaw(uint32_t raw) override;

        // ── Physics / behaviour ───────────────────────────────────────────
        bool  OmnidirectionalAirMover() const override { return HasBodyItem(); }
        float MaxUpStep() const override { return HasBodyItem() ? 0.0f : Slime::MaxUpStep(); }
        bool  CanBeLeashed() const { return HasBodyItem(); }
        void  Tick() override;
        void  AiStep() override;
        void  CustomServerAiStep() override;
        bool  Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void  Knockback(double power, double dx, double dz) override;
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

    protected:
        void RegisterGoals() override;
        std::unique_ptr<Slime> MakeSplitChild() override;

    private:
        friend class SulfurCubeMoveControl;
        friend class SulfurCubeLookControl;
        friend class SulfurCubeTemptGoal;
        friend class SulfurCubeSearchForItemsGoal;

        void TickFuse();
        void PrimeWhenOnPoweredPosition();
        void Explode();
        // MC collectEquipmentChanges: swap the archetype's attribute
        // modifiers and settings for the block now held (Air = none).
        void ApplyArchetype(ItemID item);
        void SetGoalsForBodyItem(bool hasBody);
        // MC playerTouch → playerPush, per player in contact.
        void PlayerPush();
        // MC doPush/playerTouch → applyContactDamage (the hot archetype).
        void ApplyContactDamage(LivingEntity& target);
        // MC Mob.aiStep's pickUpItem, over ItemEntities this cube touches.
        void PickUpNearbyItems();
        // MC travelInFluid's buoyancy for a laden, buoyant cube.
        void FloatInLiquid();
        // MC knockback(power, xd, zd, source, damage) with a body item —
        // the aim-angle transfer that lets a player "kick" the cube.
        void KnockbackWithBody(Entity& attacker, float damage, double xd, double zd);

        int   m_age = 0;
        int   m_forcedAge = 0;
        int   m_forcedAgeTimer = 0;
        bool  m_ageLocked = false;

        ItemID m_bodyItem = Items::Air;
        const SulfurCubeArchetypeDef* m_archetype = nullptr;
        bool  m_floatsInLiquids = false;
        float m_knockbackH = kDefaultKnockbackH;
        float m_knockbackV = kDefaultKnockbackV;
        float m_pushSoundThreshold = kDefaultPushThreshold;
        float m_pushSoundCooldownSeconds = kDefaultPushCooldown;
        int   m_pushSoundCooldown = 0;
        bool  m_goalsForBody = false;

        int   m_fuse = -1;
        int   m_maxFuse = -1;
        int   m_pickupTimer = 0;
        bool  m_fromBucket = false;

        // The attacker and damage of the Hurt in flight, so the (attacker-
        // less) Knockback override can run MC's DamageSource-aware version.
        Entity* m_hurtAttacker = nullptr;
        float   m_hurtDamage = 0.0f;
    };

} // namespace Game
