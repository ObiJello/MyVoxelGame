// File: src/common/entity/mobs/Fish.hpp
//
// MC AbstractFish + AbstractSchoolingFish — cod, salmon, tropical fish,
// pufferfish.
//
// A fish is a WaterBound pathfinder with the FishMoveControl and one habit:
// out of water on solid ground it flops (a 0.4-up hop with random sideways
// jerks) until it dies or lands back in water. Schooling adds the leader
// system: one fish accumulates followers up to the school size (8), followers
// give up their own wandering and path to the leader — which is why a school
// moves as one body with a single decision-maker.
#pragma once

#include "common/core/EntityRef.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/MountInventory.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/PlayerRideableJumping.hpp"
#include "common/entity/TamableAnimal.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/entity/mobs/TropicalFishVariant.hpp"

#include <string>
#include <vector>

namespace Game {

    struct BucketEntityData;

    // MC WaterAnimal.handleAirSupply — the INVERSE of LivingEntity's
    // drowning: a water animal out of water loses 1 air per tick and takes
    // 2.0 drown damage every time air hits -20; in water (or dead) its air
    // pins at 300. Called from BaseTick with the PRE-super air value, because
    // LivingEntity's own air block would otherwise refill a beached fish
    // (+4/tick out of water) faster than this drains it. Shared by Fish,
    // Squid and Tadpole — MC's WaterAnimal subclasses here.
    void HandleWaterAnimalAirSupply(class Mob& mob, int preTickAirSupply);

    class Fish : public PathfinderMob {
    public:
        // MC WaterAnimal.isPushedByFluid: false — a fish holds its place in a current.
        bool IsPushedByFluid() const override { return false; }
        // MC WaterAnimal.checkSpawnObstruction: level.isUnobstructed(this) only — the
        // base's no-liquid half would refuse every underwater spawn.
        bool CheckSpawnObstruction(EntityLevel& level) const override { return IsUnobstructed(level); }

    public:
        Fish(EntityTypeId type, EntityLevel* level);

        // MC AbstractFish.getMaxSpawnClusterSize.
        int GetMaxSpawnClusterSize() const override { return 8; }

        // ── MC Bucketable (AbstractFish) ──────────────────────────────────
        // MC FROM_BUCKET / "FromBucket": a fish released from a bucket is
        // kept — requiresCustomPersistence = super || fromBucket, and
        // removeWhenFarAway = !fromBucket && !hasCustomName.
        virtual bool FromBucket() const { return m_fromBucket; }
        virtual void SetFromBucket(bool fromBucket) { m_fromBucket = fromBucket; }
        bool RequiresCustomPersistence() const override {
            return PathfinderMob::RequiresCustomPersistence() || FromBucket();
        }
        bool RemoveWhenFarAway(double) const override { return !FromBucket() && !HasCustomName(); }

        // MC AbstractFish.mobInteract: Bucketable.bucketMobPickup, else super.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // MC getBucketItemStack — the cod / salmon / pufferfish / tropical
        // fish bucket, by type.
        ItemID GetBucketItem() const;
        // MC AbstractFish.saveToBucketTag / loadFromBucketTag: the default
        // keys (Bucketable). Salmon adds SALMON_SIZE and the tropical fish
        // its three TROPICAL_FISH_* components (their overrides).
        virtual void SaveToBucket(ItemStack& bucket) const;
        virtual void LoadFromBucket(const BucketEntityData& data);
        // MC Entity.applyImplicitComponents(stack) — the bucket's typed
        // components onto the fish it releases (EntityType.
        // createDefaultStackConfig, after finalizeSpawn): SALMON_SIZE, the
        // TROPICAL_FISH_* trio. Nothing for cod and pufferfish.
        virtual void ApplyImplicitComponents(const ItemStack& stack) { (void)stack; }

        // MC AbstractFish.travelInWater: a 0.01 push along the input, the
        // move, a uniform 0.9 drag, and a -0.005 sink while it has no target
        // — no gravity term and no jumpOutOfFluid (the override drops both).
        bool TravelInWaterOverride(const glm::dvec3& input, double baseGravity,
                                   bool isFalling, double oldY) override;
        // MC getPickupSound — BUCKET_FILL_FISH.
        virtual const char* GetPickupSound() const;

        // MC WaterAnimal.getBaseExperienceReward (WaterAnimal.java:32-34):
        // 1..3, same roll as Animal's. Out-of-line: needs the level random.
        int GetXpReward() const override;

        // MC AbstractFish.aiStep — the beached flop.
        void AiStep() override;

        // MC WaterAnimal.baseTick: capture air, super, handleAirSupply —
        // a beached fish now actually suffocates like MC's.
        void BaseTick() override;

        // MC canRandomSwim — a schooling follower stops wandering on its own.
        virtual bool CanRandomSwim() const { return true; }

    protected:
        void RegisterGoals() override;

    private:
        bool m_fromBucket = false;   // MC AbstractFish.FROM_BUCKET
    };

    // MC AbstractSchoolingFish — cod, salmon, tropical fish. The leader is
    // an EntityRef (never a raw pointer across ticks): MC's `leader` field
    // plus its isAlive() test becomes "resolves to a live fish".
    class SchoolingFish : public Fish {
    public:
        SchoolingFish(EntityTypeId type, EntityLevel* level);

        // MC: the spawn cluster IS the school.
        int GetMaxSpawnClusterSize() const override { return GetMaxSchoolSize(); }
        // MC getMaxSchoolSize: super.getMaxSpawnClusterSize() (8); the
        // salmon's is 5.
        virtual int GetMaxSchoolSize() const { return Fish::GetMaxSpawnClusterSize(); }

        // MC isFollower: leader != null && leader.isAlive().
        bool IsFollower() const {
            const SchoolingFish* leader = Leader();
            return leader && leader->IsAlive();
        }
        bool HasFollowers() const { return m_schoolSize > 1; }
        bool CanBeFollowed() const {
            return HasFollowers() && m_schoolSize < GetMaxSchoolSize();
        }
        // MC inRangeOfLeader: within 11 blocks (121 squared).
        bool InRangeOfLeader() const {
            const SchoolingFish* leader = Leader();
            return leader && DistanceToSqr(*leader) <= 121.0;
        }

        // MC startFollowing: take the leader, bump its school.
        void StartFollowing(SchoolingFish& leader);
        // MC stopFollowing: shrink the leader's school, drop it.
        void StopFollowing();
        // MC addFollowers: the stream limited to (max - schoolSize) FIRST,
        // then every member but this one starts following — the limit
        // counts this fish when it is in the list, exactly as MC's
        // limit-before-filter order does.
        void AddFollowers(const std::vector<SchoolingFish*>& candidates);
        void PathToLeader();

        bool CanRandomSwim() const override { return !IsFollower(); }

        void Tick() override;

        // MC SchoolSpawnGroupData: the first fish of a pack becomes leader,
        // the rest of the pack spawns already following it.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void ClearReferenceTo(const Entity* entity) override {
            Fish::ClearReferenceTo(entity);
            m_leader.OnEntityRemoved(entity);
        }

    protected:
        void RegisterGoals() override;

    private:
        // The resolved leader, or null. Same type as this fish by
        // construction (only same-type fish ever start following).
        SchoolingFish* Leader() const;

        mutable EntityRef m_leader;   // MC AbstractSchoolingFish.leader
        int m_schoolSize = 1;         // MC schoolSize
    };

    // MC animal/fish/Salmon — the schooling fish in three sizes. The size
    // (MC Salmon.Variant: SMALL 0 / MEDIUM 1 / LARGE 2, DATA_TYPE) scales the
    // box and the eye 0.5 / 1.0 / 1.5 (getDefaultDimensions) and picks the
    // SALMON_SMALL / SALMON / SALMON_LARGE mesh on the client; it is rolled
    // 30/50/15 at spawn, saved as "type" ("small"/"medium"/"large"), rides
    // the wire's variant byte, and is kept by the bucket (SALMON_SIZE).
    class Salmon : public SchoolingFish {
    public:
        static constexpr uint8_t kSmall  = 0;
        static constexpr uint8_t kMedium = 1;   // MC Variant.DEFAULT
        static constexpr uint8_t kLarge  = 2;

        explicit Salmon(EntityLevel* level)
            : SchoolingFish(EntityTypeId::Salmon, level) {}

        // MC Salmon.getMaxSchoolSize.
        int GetMaxSchoolSize() const override { return 5; }

        uint8_t GetSize() const { return m_size; }
        // MC Variant.BY_ID — ByIdMap.continuous, CLAMP.
        void SetSize(int size) { m_size = static_cast<uint8_t>(size < 0 ? 0 : (size > 2 ? 2 : size)); }
        // MC Variant.boundingBoxScale.
        static float ScaleOf(uint8_t size) { return size == kSmall ? 0.5f : (size == kLarge ? 1.5f : 1.0f); }
        // MC getSalmonScale.
        float GetSalmonScale() const { return ScaleOf(m_size); }
        // MC Variant.getSerializedName / the codec's decode (-1 = unknown).
        static const char* SizeName(uint8_t size);
        static int SizeFromName(const std::string& name);

        // MC getDefaultDimensions: super's scaled by the size.
        float BaseBbWidth()   const override { return SchoolingFish::BaseBbWidth()   * GetSalmonScale(); }
        float BaseBbHeight()  const override { return SchoolingFish::BaseBbHeight()  * GetSalmonScale(); }
        float BaseEyeHeight() const override { return SchoolingFish::BaseEyeHeight() * GetSalmonScale(); }

        uint8_t GetVariantByte() const override { return m_size; }
        void    SetVariantByte(uint8_t v) override { SetSize(v); }

        // MC Salmon.finalizeSpawn: the weighted size roll, THEN super.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Salmon.saveToBucketTag: the default keys + SALMON_SIZE.
        void SaveToBucket(ItemStack& bucket) const override;
        // MC applyImplicitComponents: SALMON_SIZE.
        void ApplyImplicitComponents(const ItemStack& stack) override;

    private:
        uint8_t m_size = kMedium;   // MC DATA_TYPE
    };

    // MC animal/fish/TropicalFish — the variant (pattern over one of two
    // body shapes, base colour, pattern colour; TropicalFishVariant.hpp),
    // rolled at spawn (90% one of the 22 named COMMON_VARIANTS shared by the
    // whole school via TropicalFishGroupData, else a fully random loner that
    // caps its spawn group — isSchool), saved as "Variant" (MC's packed int),
    // copied into the bucket as the TROPICAL_FISH_* components and read back
    // from them on release.
    //
    // Wire: the packed variant is 12 bits of information; it rides the
    // variant byte (base colour | pattern colour << 4) and the anim byte
    // (the pattern's ordinal), both applied on spawn and on change.
    class TropicalFish : public SchoolingFish {
    public:
        explicit TropicalFish(EntityLevel* level)
            : SchoolingFish(EntityTypeId::TropicalFish, level) {}

        // MC TropicalFish.isMaxGroupSizeReached.
        bool IsMaxGroupSizeReached(int) const override { return !m_isSchool; }

        // MC getPackedVariant / setPackedVariant.
        int32_t GetPackedVariant() const { return TropicalFishVariants::Pack(m_variant); }
        void    SetPackedVariant(int32_t packed) { m_variant = TropicalFishVariants::Unpack(packed); }
        const TropicalFishVariants::Variant& GetVariant() const { return m_variant; }
        void SetVariant(const TropicalFishVariants::Variant& v) { m_variant = v; }

        uint8_t GetVariantByte() const override {
            return static_cast<uint8_t>((m_variant.baseColor & 0x0F) |
                                        ((m_variant.patternColor & 0x0F) << 4));
        }
        void SetVariantByte(uint8_t v) override {
            m_variant.baseColor    = static_cast<uint8_t>(v & 0x0F);
            m_variant.patternColor = static_cast<uint8_t>((v >> 4) & 0x0F);
        }
        uint8_t GetAnimStateByte() const override { return static_cast<uint8_t>(m_variant.pattern); }
        void SetAnimStateByte(uint8_t v) override {
            m_variant.pattern = v < TropicalFishVariants::kPatternCount
                ? static_cast<TropicalFishVariants::Pattern>(v)
                : TropicalFishVariants::Pattern::Kob;
        }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC TropicalFish.saveToBucketTag: the default keys + the pattern,
        // base colour and pattern colour components.
        void SaveToBucket(ItemStack& bucket) const override;
        // MC applyImplicitComponents: each TROPICAL_FISH_* component present
        // replaces its third of the variant.
        void ApplyImplicitComponents(const ItemStack& stack) override;

    private:
        TropicalFishVariants::Variant m_variant = TropicalFishVariants::kDefaultVariant;   // MC DATA_ID_TYPE_VARIANT
        bool m_isSchool = true;   // MC TropicalFish.isSchool
    };

    // MC Pufferfish — a non-schooling fish with the three-stage puff-up:
    // PufferfishPuffGoal starts inflating when a scary LivingEntity comes
    // within 2 blocks; tick() walks the SMALL → MID → FULL state machine and
    // back down; while puffed, anything touching it takes (1 + state) damage
    // and POISON for 60 * state ticks.
    //
    // The puff state rides the wire's anim byte (MC syncs it as PUFF_STATE)
    // so the client's copy has it, and scales the box per state (0.5 / 0.7 /
    // 1.0 — getDefaultDimensions; the renderer swaps the small / mid / big
    // mesh). MC's nausea on player sting is a skipped-comment at the site
    // (no NAUSEA gameplay half exists — it is a pure screen effect).
    class Pufferfish : public Fish {
    public:
        explicit Pufferfish(EntityLevel* level);

        // MC Pufferfish.STATE_*.
        static constexpr int kStateSmall = 0;
        static constexpr int kStateMid   = 1;
        static constexpr int kStateFull  = 2;

        int  GetPuffState() const { return m_puffState; }
        // MC's read applies min(saved, 2); the clamp lives here so every
        // caller gets it.
        void SetPuffState(int state) { m_puffState = state < 0 ? 0 : (state > 2 ? 2 : state); }

        // MC Pufferfish.getScale / getDefaultDimensions: the box follows the
        // puff (refreshDimensions on every PUFF_STATE change — the box here
        // is read live, so the change IS the refresh).
        static float ScaleOf(int state) { return state == 0 ? 0.5f : (state == 1 ? 0.7f : 1.0f); }
        float BaseBbWidth()   const override { return Fish::BaseBbWidth()   * ScaleOf(m_puffState); }
        float BaseBbHeight()  const override { return Fish::BaseBbHeight()  * ScaleOf(m_puffState); }
        float BaseEyeHeight() const override { return Fish::BaseEyeHeight() * ScaleOf(m_puffState); }

        // MC's SCARY_MOB selector: not a creative player, and not in the
        // NOT_SCARY_FOR_PUFFERFISH tag (the aquatic neighbours).
        static bool IsScaryTarget(const LivingEntity& target);

        // Called by PufferfishPuffGoal (MC's inner class writes the counters
        // directly).
        void StartPuffing() { m_inflateCounter = 1; m_deflateTimer = 0; }
        void StopPuffing()  { m_inflateCounter = 0; }

        // MC Pufferfish.tick — the inflate/deflate state machine (server).
        void Tick() override;

        // MC Pufferfish.aiStep — the touch sweep while puffed.
        void AiStep() override;

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(m_puffState);
        }
        void SetAnimStateByte(uint8_t v) override {
            m_puffState = v <= 2 ? static_cast<int>(v) : 2;
        }

    protected:
        void RegisterGoals() override;

    private:
        // MC Pufferfish.touch — damage + poison one victim.
        void Touch(LivingEntity& mob);

        int m_puffState = 0;        // MC PUFF_STATE (synced)
        int m_inflateCounter = 0;   // MC inflateCounter
        int m_deflateTimer = 0;     // MC deflateTimer
    };

    // MC animal/dolphin/Dolphin. MAX_HEALTH 10, MOVEMENT_SPEED 1.2 (a swim
    // speed — SmoothSwimmingMoveControl's 0.1 land factor is what keeps a
    // beached dolphin from sprinting), ATTACK_DAMAGE 3.
    //
    // The parts that make a dolphin a dolphin here: the smooth-swim controls
    // (SmoothSwimmingMoveControl(85, 10, 0.02, 0.1, gravity) +
    // SmoothSwimmingLookControl(10)), the breach (DolphinJumpGoal), the
    // beached flop + dry-out damage from tick(), melee retaliation
    // (guardian-grudge HurtByTarget + MeleeAttackGoal(1.2)), fleeing
    // guardians, TryFindWaterGoal, and the air supply — 4800 ticks of
    // breath, drowning underwater, BreathAirGoal surfacing it below 140.
    // Item play is modelled: canPickUpLoot from birth, a whole stack into
    // the empty MAINHAND (a guaranteed drop), MoveToItemGoal to floating
    // items and PlayWithItemsGoal tossing them. So is feeding (a fish ages a
    // calf or sets gotFish). Not modelled, each named at its site
    // (DolphinGoals.hpp): the treasure hunt's structure search (a fed
    // dolphin finds nothing and forgets the fish, MC's own stuck path),
    // boat following.
    //
    // MC's AgeableWaterCreature half IS modelled: a dolphin is an AgeableMob
    // (age, baby box, the 26.x baby mesh), 10% of a spawn pack past the first
    // member spawns as a calf (Dolphin.finalizeSpawn), and a spawn egg on an
    // adult makes one (getBreedOffspring). Dolphins still do not breed.
    class Dolphin : public AgeableMob {
    public:
        // MC Dolphin.canDispenserEquipIntoSlot: the main hand of a dolphin
        // that picks up loot.
        bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const override {
            return slot == EquipmentSlot::MAINHAND && CanPickUpLoot();
        }
        // MC AgeableWaterCreature.isPushedByFluid: false.
        bool IsPushedByFluid() const override { return false; }
        // MC AgeableWaterCreature.checkSpawnObstruction: level.isUnobstructed(this) only — the
        // base's no-liquid half would refuse every underwater spawn.
        bool CheckSpawnObstruction(EntityLevel& level) const override { return IsUnobstructed(level); }

    public:
        explicit Dolphin(EntityLevel* level);

        // MC Dolphin.playAttackSound.
        void PlayAttackSound() override { PlaySound(SoundEvents::DOLPHIN_ATTACK, 1.0f, 1.0f); }

        // MC Dolphin.pickUpItem: only into an empty mouth — the whole stack,
        // a guaranteed drop.
        void PickUpItem(int32_t itemEntityId, const ItemStack& stack) override;
        // MC Dolphin.mobInteract: a fish (#fishes) ages a calf, else sets
        // gotFish (the treasure hunt's trigger); the eat sound either way.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // MC GOT_FISH ("GotFish").
        bool GotFish() const { return m_gotFish; }
        void SetGotFish(bool v) { m_gotFish = v; }
        // MC Dolphin.ItemGoal.drop: the held stack thrown ahead from just
        // below the eyes (40-tick pickup delay), the slot emptied. False
        // when the mouth was empty.
        bool DropHeldItem();

        // MC Dolphin.finalizeSpawn: full air, level pitch, then the
        // AgeableMobGroupData(0.1F) pack roll.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        static void CreateAttributes(AttributeMap& out);

        // MC WaterCreature despawns like any water ambient.
        bool RemoveWhenFarAway(double) const override { return true; }

        // MC AgeableWaterCreature.getBaseExperienceReward
        // (AgeableWaterCreature.java:30-32): 1..3. Out-of-line: needs the
        // level random.
        int GetXpReward() const override;

        // MC Dolphin.canAttack: never as a baby.
        bool CanAttack(const LivingEntity& target) const override {
            return !IsBaby() && AgeableMob::CanAttack(target);
        }

        // ── Air (MC Dolphin's 4800-tick lungs) ────────────────────────────
        // A dolphin DROWNS underwater — it is not in the can-breathe tag, so
        // LivingEntity's air block ticks it down while its eyes are
        // submerged — but a single breath at the surface refills everything.
        // WaterAnimal's beached air-loss is overridden EMPTY in MC
        // (handleAirSupply {}), which is why Dolphin derives PathfinderMob's
        // BaseTick untouched here: on land the moistness clock is the killer.
        int GetMaxAirSupply() const override { return 4800; }
        int IncreaseAirSupply(int) override { return GetMaxAirSupply(); }

        // MC Dolphin.getMaxHeadXRot/YRot: 1 — the whole body steers.
        int GetMaxHeadXRot() const override { return 1; }
        int GetMaxHeadYRot() const override { return 1; }

        // MC Dolphin.tick — the moistness clock: 2400 ticks out of water
        // or rain (isInWaterOrRain), then 1 damage per
        // tick, and the beached flop (random hop + spin); the client copy
        // draws the DOLPHIN trail.
        void Tick() override;

    protected:
        void RegisterGoals() override;

    public:
        // MC Dolphin.getMoistnessLevel / setMoistnessLevel — the "Moistness"
        // save key. A dolphin left out of water dies on this clock, so a save
        // that resets it to full is a stay of execution vanilla does not give.
        int  GetMoistness() const { return m_moistnessLevel; }
        void SetMoistness(int level) { m_moistnessLevel = level; }

    private:
        int m_moistnessLevel = 2400;   // MC TOTAL_MOISTNESS_LEVEL
        bool m_gotFish = false;        // MC GOT_FISH
    };

    // MC Squid / GlowSquid — no navigation, no move control: the tentacle
    // pump IS the locomotion. Each pump cycle's final quarter sets velocity
    // to the chosen movement vector outright; between pumps the squid coasts
    // at 0.9 drag. Travel is overridden to raw movement — a squid takes no
    // block friction and no gravity while it swims.
    // Like the dolphin an AgeableWaterCreature in MC: 5% of a pack past the
    // first member spawns as a baby (Squid.finalizeSpawn), the baby box is
    // 0.5 x 0.5 (BABY_DIMENSIONS) and the ink burst shrinks with it.
    class Squid : public AgeableMob {
    public:
        // MC AgeableWaterCreature.isPushedByFluid: false.
        bool IsPushedByFluid() const override { return false; }
        // MC AgeableWaterCreature.checkSpawnObstruction: level.isUnobstructed(this) only — the
        // base's no-liquid half would refuse every underwater spawn.
        bool CheckSpawnObstruction(EntityLevel& level) const override { return IsUnobstructed(level); }

    public:
        Squid(EntityTypeId type, EntityLevel* level);

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Squid is a WaterAnimal: a beached squid suffocates on the same
        // clock as a beached fish (see HandleWaterAnimalAirSupply).
        void BaseTick() override;

        // MC AgeableWaterCreature.getBaseExperienceReward
        // (AgeableWaterCreature.java:30-32): 1..3.
        int GetXpReward() const override;

        bool HasMovementVector() const {
            return glm::dot(m_movementVector, m_movementVector) > 1.0e-10;
        }
        void SetMovementVector(const glm::dvec3& v) { m_movementVector = v; }

        void AiStep() override;
        void Travel(const glm::dvec3& input) override;

        // MC hurtServer sprays ink (SpawnInk) and flees; the flee half is
        // the goal's.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Squid.handleEntityEvent(19) — the pump-wrap reset. The server
        // broadcasts it once per stroke; the client pins its own clock at 2π
        // until the event arrives, which keeps both sides' strokes in phase.
        void HandleEntityEvent(uint8_t id) override;

        // MC SquidRenderer.extractRenderState: tentacleAngle lerped by the
        // partial tick.
        float GetTentacleAngle(float partialTick) const {
            return m_oldTentacleAngle +
                   partialTick * (m_tentacleAngle - m_oldTentacleAngle);
        }

    private:
        // MC spawnInk: the squirt sound and the 30-puff SQUID_INK (glow
        // squid: GLOW_SQUID_INK) spray.
        void SpawnInk();

        glm::dvec3 m_movementVector{0.0};
        float m_tentacleMovement = 0.0f;
        float m_oldTentacleMovement = 0.0f;
        float m_tentacleAngle = 0.0f;
        float m_oldTentacleAngle = 0.0f;
        float m_tentacleSpeed = 0.0f;
        float m_rotateSpeed = 0.0f;
    };

    // ── Nautilus / ZombieNautilus ──────────────────────────────────────────
    //
    // MC animal/nautilus/AbstractNautilus and its two concretes, on the
    // ported NautilusAi / ZombieNautilusAi brains. MC's AbstractNautilus is a
    // TamableAnimal + PlayerRideableJumping: a pufferfish (or its bucket)
    // tames a wild adult on a 1-in-3 roll, fish feed and breed a tame one,
    // an empty hand mounts it, and a saddle hands the rider the controls —
    // look to steer (pitch climbs and dives), charge the jump bar to dash
    // (40-tick cooldown, the dash-ready call at its end), and the rider
    // breathes under water (BREATH_OF_THE_NAUTILUS). The TamableAnimal half
    // is the engine's mixin (TamableAnimal.hpp); the tame flag and the synced
    // DASH flag ride the anim byte. The shell inventory, the saddle/armour
    // equips and their sounds are the mount-equipment system's.
    class AbstractNautilus : public GenericAnimal, public TamableAnimal, public PlayerRideableJumping {
    public:
        AbstractNautilus(EntityTypeId type, EntityLevel* level);

        // MC AbstractNautilus's dash constants.
        static constexpr int   kDashCooldownTicks      = 40;
        static constexpr int   kDashMinimumDuration    = 5;
        static constexpr float kDashMomentumInWater    = 1.2f;
        static constexpr float kDashMomentumOnLand     = 0.5f;
        static constexpr float kRiddenSpeedInWater     = 0.0325f;
        static constexpr float kRiddenSpeedOnLand      = 0.02f;
        static constexpr int   kSmallRestrictionRadius = 16;
        static constexpr int   kLargeRestrictionRadius = 32;
        static constexpr int   kRestrictionRadiusBuffer = 8;
        static constexpr int   kEffectDuration         = 60;
        static constexpr int   kEffectRefreshRate      = 40;

        // MC AbstractNautilus.checkSpawnObstruction: level.isUnobstructed(this) only — the
        // base's no-liquid half would refuse every underwater spawn.
        bool CheckSpawnObstruction(EntityLevel& level) const override { return IsUnobstructed(level); }

        // MC AbstractNautilus.getWalkTargetValue — flat 0 (water is home).
        float GetWalkTargetValue(const glm::ivec3&) const override { return 0.0f; }

        // MC Nautilus.getMaxAirSupply — 300, refilled in water, drained on
        // land with the dry-out damage (handleAirSupply in BaseTick).
        int GetMaxAirSupply() const override { return 300; }
        void BaseTick() override;

        // MC AbstractNautilus.isPushedByFluid — currents never move it.
        bool IsPushedByFluid() const override { return false; }

        // MC AbstractNautilus.hurtServer — anger at whatever hit it.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC AbstractNautilus.canBeAffected — immune to poison (it eats
        // pufferfish for lunch).
        bool CanBeAffected(const MobEffectInstance& effect) const override;

        // MC AbstractNautilus.finalizeSpawn → NautilusAi.initMemories.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC AbstractNautilus.removeWhenFarAway — true; requiresCustomPersistence
        // adds isTame (Mob::IsTamedPet keeps a tame one).
        bool RemoveWhenFarAway(double) const override { return true; }
        bool IsTamedPet() const override { return IsTame(); }

        // ── Taming and feeding (MC isFood / mobInteract / tryToTame) ───────
        // MC isFood: an untamed adult takes only #nautilus_taming_items
        // (pufferfish, pufferfish bucket); a tame one or a baby #nautilus_food
        // (the fishes, raw and cooked, and the four fish buckets).
        bool IsFood(uint32_t itemId) const override;
        static bool IsTamingItem(uint32_t itemId);
        // MC ItemTags.NAUTILUS_FOOD — also NautilusAi.getTemptations.
        static bool IsNautilusFood(uint32_t itemId);
        // MC ItemTags.NAUTILUS_BUCKET_FOOD — eaten out of the bucket, which
        // comes back as a water bucket (AbstractNautilus.usePlayerItem).
        static bool IsBucketFood(uint32_t itemId);
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // MC AbstractNautilus.canUseSlot: the SADDLE and BODY slots only on a
        // live, adult, tame nautilus.
        bool CanUseSlot(EquipmentSlot slot) const override {
            if (slot != EquipmentSlot::SADDLE && slot != EquipmentSlot::BODY) {
                return GenericAnimal::CanUseSlot(slot);
            }
            return IsAlive() && !IsBaby() && IsTame();
        }
        // ── Equipment and the shell inventory (MC AbstractNautilus; the
        //    system is MountInventory's) ────────────────────────────────────
        // MC canDispenserEquipIntoSlot: the body and the saddle.
        bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const override {
            return slot == EquipmentSlot::BODY || slot == EquipmentSlot::SADDLE ||
                   GenericAnimal::CanDispenserEquipIntoSlot(slot);
        }
        // MC getEquipSound: the saddle's NAUTILUS_SADDLE_EQUIP, under water
        // NAUTILUS_SADDLE_UNDERWATER_EQUIP.
        std::string GetEquipSound(EquipmentSlot slot, const ItemStack& stack,
                                  const Equippable& equippable) const override {
            if (slot == EquipmentSlot::SADDLE && IsUnderWater()) return SoundEvents::NAUTILUS_SADDLE_UNDERWATER_EQUIP;
            if (slot == EquipmentSlot::SADDLE) return SoundEvents::NAUTILUS_SADDLE_EQUIP;
            return GenericAnimal::GetEquipSound(slot, stack, equippable);
        }
        MountInventory*       GetMountInventory() override       { return &m_mountInventory; }
        const MountInventory* GetMountInventory() const override { return &m_mountInventory; }
        bool HasCustomInventoryScreen() const override { return true; }
        // MC openCustomInventoryScreen: server, nobody else aboard, tame →
        // player.openNautilusInventory.
        void OpenCustomInventoryScreen(LivingEntity& player) override;
        void HandleEntityEvent(uint8_t id) override {
            if (!HandleTamableEntityEvent(id)) GenericAnimal::HandleEntityEvent(id);
        }
        void ClearReferenceTo(const Entity* entity) override {
            GenericAnimal::ClearReferenceTo(entity);
            ClearOwnerReferenceTo(entity);
        }

        // ── Riding (MC getControllingPassenger / travelRidden hooks) ───────
        // A saddle and a player in the first seat: that player steers.
        bool CanBeSteeredBy(const RiderControl& rider) const override;
        glm::dvec3 GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) override;
        void  TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) override;
        float GetRiddenSpeed(const RiderControl& rider) const override;
        // MC travel → travelInWater override: the swim step at getSpeed()
        // with a flat 0.9 drag and no gravity (the steering speed and the
        // SmoothSwimmingMoveControl's 0.011 both land here); the rest is the
        // base travel.
        void Travel(const glm::dvec3& input) override;
        // MC EntityTypes passengerAttachments(1.1375); the baby's dimensions
        // carry (0, 0.5, 0).
        glm::dvec3 GetPassengerAttachmentPoint(const Entity& passenger) const override;
        // MC doPlayerRide: the player climbs on (server); a refused seat
        // clears the home restriction.
        void DoPlayerRide(LivingEntity& player);

        // ── PlayerRideableJumping — the dash ───────────────────────────────
        void OnPlayerJump(int jumpAmount) override;
        bool CanJump() const override { return IsSaddled(); }
        void HandleStartJump(int jumpScale) override;
        void HandleStopJump() override {}
        int  GetJumpCooldown() const override { return m_dashCooldown; }

        // MC DASH (synched): set by a dash, cleared once 5 ticks of the
        // cooldown have run. Every change on a side with the flag live
        // (MC onSyncedDataUpdated, !firstTick) arms the cooldown when idle.
        bool IsDashing() const { return m_dashing; }
        void SetDashing(bool dashing);

        void Tick() override;
        void CustomServerAiStep() override;
        // MC AbstractNautilus.playStepSound — silent.
        void PlayStepSound(const glm::ivec3&, BlockState) override {}

        // Anim byte: bit 0 sitting pose, bit 1 tame (TamableAnimal's
        // packing), bit 2 DASH.
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(GetTamableAnimByte() | (m_dashing ? 4 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            SetTamableAnimByte(static_cast<uint8_t>(v & 3));
            SetDashing((v & 4) != 0);
        }

        // MC getDashSound / getDashReadySound — each concrete names its own.
        virtual const char* GetDashSound() const = 0;
        virtual const char* GetDashReadySound() const = 0;

    private:
        // MC executeRidersJump — the dash along the rider's look.
        void ExecuteRidersJump(float amount, const RiderControl& rider);
        // MC tryToTame — 1 in 3, then the chew either way.
        void TryToTame(LivingEntity& player);
        // MC checkRestriction — a tame, free nautilus stays near home.
        void CheckRestriction();
        // MC applyEffects — the rider's water breathing.
        void ApplyEffects();
        // MC spawnBubbles — the trail behind the shell.
        void SpawnBubbles();
        // MC Entity.getBlockSpeedFactor through LivingEntity's
        // MOVEMENT_EFFICIENCY lerp.
        float BlockSpeedFactor() const;

        int   m_dashCooldown = 0;
        bool  m_dashing = false;
        float m_playerJumpPendingScale = 0.0f;
        // MC AbstractNautilus.inventory (getInventoryColumns 0).
        MountInventory m_mountInventory;
    };

    class Nautilus : public AbstractNautilus {
    public:
        explicit Nautilus(EntityLevel* level);
        void UpdateBrainActivity() override;

        // MC Nautilus.getBreedOffspring — a tame parent's foal is born tame
        // to the same owner.
        std::unique_ptr<Animal> CreateBaby() override;

        // MC Nautilus sounds: the baby's set and the on-land variants.
        const char* GetAmbientSound() const override;
        const char* GetHurtSound(MobDamageSource source) const override;
        const char* GetDeathSound() const override;
        const char* GetSwimSound() const override {
            return IsBaby() ? SoundEvents::BABY_NAUTILUS_SWIM : SoundEvents::NAUTILUS_SWIM;
        }
        void PlayEatingSound() override {
            MakeSound(IsBaby() ? SoundEvents::BABY_NAUTILUS_EAT : SoundEvents::NAUTILUS_EAT);
        }
        const char* GetDashSound() const override {
            return IsUnderWater() ? SoundEvents::NAUTILUS_DASH : SoundEvents::NAUTILUS_DASH_ON_LAND;
        }
        const char* GetDashReadySound() const override {
            return IsUnderWater() ? SoundEvents::NAUTILUS_DASH_READY : SoundEvents::NAUTILUS_DASH_READY_ON_LAND;
        }
    };

    // MC ZombieNautilus — never a baby, never breeds, and hostile through the
    // shared target finder; tamed and ridden exactly as the nautilus. Its
    // biome texture variant (temperate/cold/warm) is SKIPPED: the port ships
    // one zombie_nautilus.png.
    class ZombieNautilus : public AbstractNautilus {
    public:
        explicit ZombieNautilus(EntityLevel* level);
        void UpdateBrainActivity() override;

        bool IsBaby() const override { return false; }

        // MC ZombieNautilus.getBreedOffspring — null: no zombie babies. The
        // brain has no AnimalMakeLove either, so this is belt and braces.
        std::unique_ptr<Animal> CreateBaby() override { return nullptr; }

        void PlayEatingSound() override { MakeSound(SoundEvents::ZOMBIE_NAUTILUS_EAT); }
        const char* GetDashSound() const override {
            return IsUnderWater() ? SoundEvents::ZOMBIE_NAUTILUS_DASH : SoundEvents::ZOMBIE_NAUTILUS_DASH_ON_LAND;
        }
        const char* GetDashReadySound() const override {
            return IsUnderWater() ? SoundEvents::ZOMBIE_NAUTILUS_DASH_READY
                                  : SoundEvents::ZOMBIE_NAUTILUS_DASH_READY_ON_LAND;
        }
    };

} // namespace Game
