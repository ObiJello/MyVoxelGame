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

#include <algorithm>
#include <optional>
#include <string_view>

#include "common/entity/Animal.hpp"
#include "common/entity/NeutralMob.hpp"
#include "common/entity/RangedAttackMob.hpp"
#include "common/entity/TamableAnimal.hpp"
#include "common/entity/ParrotDanceRange.hpp"
#include "common/entity/HorseTaming.hpp"
#include "common/entity/MountInventory.hpp"
#include "common/entity/PlayerRideableJumping.hpp"
#include "common/entity/ItemBasedSteering.hpp"
#include "common/entity/DyeColorUtil.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/mobs/WolfVariants.hpp"
#include "common/sound/SoundEvents.hpp"

namespace Game {

    // MC TemperatureVariants — the three farm-animal variants of 1.21.5
    // (cow, pig, chicken), in the order the variant byte carries them.
    enum class TemperatureVariant : uint8_t { Temperate = 0, Warm = 1, Cold = 2 };
    // The variant a farm animal spawning in `biome` (slug, no namespace)
    // gets: MC's CowVariants/PigVariants/ChickenVariants bootstraps all
    // resolve to warm-tag → warm (priority 1), cold-tag → cold (priority 1),
    // else the temperate fallback (priority 0).
    TemperatureVariant FarmAnimalVariantForBiome(std::string_view biome);

    // MC Cow. MAX_HEALTH 10, MOVEMENT_SPEED 0.2.
    class Cow : public Animal {
    public:
        explicit Cow(EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        static void CreateAttributes(AttributeMap& out);

        // MC 1.21.5 CowVariants: temperate / warm / cold, chosen by biome in
        // FinalizeSpawn (BiomeTags.SPAWNS_WARM_VARIANT_FARM_ANIMALS and
        // SPAWNS_COLD_VARIANT_FARM_ANIMALS beat the temperate fallback),
        // inherited from a random parent when bred, saved as "variant"
        // (minecraft:temperate|warm|cold) and carried by the wire's variant
        // byte (DATA_VARIANT_ID).
        TemperatureVariant GetVariant() const { return m_variant; }
        void SetVariant(TemperatureVariant v) { m_variant = v; }
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_variant); }
        void    SetVariantByte(uint8_t v) override {
            m_variant = v <= 2 ? static_cast<TemperatureVariant>(v) : TemperatureVariant::Temperate;
        }
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;
        // MC getBreedOffspring: the baby takes THIS parent's or the partner's
        // variant at random. The partner is only known here, so it is noted
        // for the CreateBaby that follows.
        void SpawnChildFromBreeding(Animal& partner) override;
        // MC <Mob>SoundVariants (FarmSoundVariants.hpp): the sound set,
        // picked at random in FinalizeSpawn, saved as "sound_variant" and set
        // by the item's <mob>/sound_variant. Classic answers the type's own
        // sounds (EntitySounds).
        uint8_t GetSoundVariant() const { return m_soundVariant; }
        void SetSoundVariant(uint8_t v) { m_soundVariant = v; }
        const char* GetAmbientSound() const override;
        const char* GetHurtSound(MobDamageSource source) const override;
        const char* GetDeathSound() const override;
        void PlayStepSound(const glm::ivec3& pos, BlockState state) override;

    protected:
        TemperatureVariant m_variant = TemperatureVariant::Temperate;
        int8_t m_breedPartnerVariant = -1;   // -1 = no partner known
        uint8_t m_soundVariant = 0;         // FarmSoundVariants, 0 = classic
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

        // MC MushroomCow.shear(level, soundSource, tool) — convert to Cow,
        // drop 5 mushrooms (of the variant's colour); the sound in
        // `soundSource` (PLAYERS from a player, BLOCKS from a dispenser).
        void Shear(SoundSource soundSource = SoundSource::Players);

        // MC MushroomCow.Variant — RED (0, the default) / BROWN (1). Saved
        // as "Type" ("red" / "brown"), synced through the variant byte
        // (DATA_TYPE); the renderer picks red_ / brown_mooshroom.png and the
        // matching mushrooms on its back.
        enum class Variant : uint8_t { Red = 0, Brown = 1 };
        Variant GetVariant() const { return m_variant; }
        void SetVariant(Variant v) { m_variant = v; }
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_variant); }
        void    SetVariantByte(uint8_t v) override { m_variant = v == 1 ? Variant::Brown : Variant::Red; }

        // MC MushroomCow.thunderHit: a bolt this mooshroom has not been hit
        // by yet (lastLightningBoltUUID) swaps red <-> brown with the
        // convert sound — once per bolt, however many flashes it makes. No
        // fire, no damage (the base thunderHit is not called).
        void ThunderHit(Entity* bolt) override;

        // MC getBreedOffspring / getOffspringVariant: two parents of one
        // colour have a 1-in-1024 mutation to the other; otherwise either
        // parent's colour at random.
        std::unique_ptr<Animal> CreateBaby() override;
        void SpawnChildFromBreeding(Animal& partner) override;

    private:
        Variant m_variant = Variant::Red;
        int8_t  m_breedPartnerVariant = -1;   // -1 = no partner known
        Uuid    m_lastLightningBoltUuid{};
    };

    // MC Pig. MAX_HEALTH 10, MOVEMENT_SPEED 0.25. ItemSteerable: a saddled
    // pig carries a player (mobInteract) and is steered by one holding a
    // carrot on a stick (ItemBasedSteering, FoodOnAStickItem).
    class Pig : public Animal, public ItemSteerable {
    public:
        explicit Pig(EntityLevel* level);

        // ── Riding (MC Pig.getControllingPassenger / tickRidden /
        //    getRiddenInput / getRiddenSpeed / boost) ────────────────────
        // MC getControllingPassenger: saddled, and the player in the first
        // seat holds a carrot on a stick in either hand (isHolding).
        bool CanBeSteeredBy(const RiderControl& rider) const override;
        // MC getRiddenInput: always straight ahead — the pig walks where the
        // rider looks, whatever keys are held.
        glm::dvec3 GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) override;
        // MC tickRidden: turn to the rider's view (pitch halved), body and
        // head with it, then the boost clock.
        void TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) override;
        // MC getRiddenSpeed: MOVEMENT_SPEED * 0.225 * boostFactor.
        float GetRiddenSpeed(const RiderControl& rider) const override;
        // MC ItemSteerable.boost — the carrot on a stick's use.
        bool Boost() override;
        // MC Pig.mobInteract: an empty-of-food hand on a saddled, unridden
        // pig climbs on (not while sneaking); anything else is Animal's (the
        // saddle's own interactLivingEntity follows a PASS on the server).
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // ── The saddle (MC Pig.canUseSlot / canDispenserEquipIntoSlot /
        //    getEquipSound) ─────────────────────────────────────────────────
        bool CanUseSlot(EquipmentSlot slot) const override {
            if (slot != EquipmentSlot::SADDLE) return Animal::CanUseSlot(slot);
            return IsAlive() && !IsBaby();
        }
        bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const override {
            return slot == EquipmentSlot::SADDLE || Animal::CanDispenserEquipIntoSlot(slot);
        }
        std::string GetEquipSound(EquipmentSlot slot, const ItemStack& stack,
                                  const Equippable& equippable) const override {
            return slot == EquipmentSlot::SADDLE ? std::string(SoundEvents::PIG_SADDLE)
                                                 : Animal::GetEquipSound(slot, stack, equippable);
        }
        // MC DATA_BOOST_TIME, carried on the mob's synced data int (the
        // sulfur cube's carried-block field — a pig carries no block). The
        // client's copy starts its burst when a new length arrives
        // (onSyncedDataUpdated → steering.onSynced).
        uint32_t GetCarriedBlockRaw() const override {
            return static_cast<uint32_t>(m_steering.BoostTimeTotal());
        }
        void SetCarriedBlockRaw(uint32_t raw) override;

        // MC Pig.thunderHit: off Peaceful, the pig becomes a ZOMBIFIED_PIGLIN
        // (Mob.convertTo SINGLE, equipment not kept, loot pickup kept) with
        // its default gear (the golden sword) and persistence; on Peaceful —
        // or if the conversion fails — the base burn-and-hurt.
        void ThunderHit(Entity* bolt) override;

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        static void CreateAttributes(AttributeMap& out);

        // MC 1.21.5 PigVariants: temperate / warm / cold, chosen by biome in
        // FinalizeSpawn (BiomeTags.SPAWNS_WARM_VARIANT_FARM_ANIMALS and
        // SPAWNS_COLD_VARIANT_FARM_ANIMALS beat the temperate fallback),
        // inherited from a random parent when bred, saved as "variant"
        // (minecraft:temperate|warm|cold) and carried by the wire's variant
        // byte (DATA_VARIANT_ID).
        TemperatureVariant GetVariant() const { return m_variant; }
        void SetVariant(TemperatureVariant v) { m_variant = v; }
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_variant); }
        void    SetVariantByte(uint8_t v) override {
            m_variant = v <= 2 ? static_cast<TemperatureVariant>(v) : TemperatureVariant::Temperate;
        }
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;
        // MC getBreedOffspring: the baby takes THIS parent's or the partner's
        // variant at random. The partner is only known here, so it is noted
        // for the CreateBaby that follows.
        void SpawnChildFromBreeding(Animal& partner) override;
        // MC <Mob>SoundVariants (FarmSoundVariants.hpp): the sound set,
        // picked at random in FinalizeSpawn, saved as "sound_variant" and set
        // by the item's <mob>/sound_variant. Classic answers the type's own
        // sounds (EntitySounds).
        uint8_t GetSoundVariant() const { return m_soundVariant; }
        void SetSoundVariant(uint8_t v) { m_soundVariant = v; }
        const char* GetAmbientSound() const override;
        const char* GetHurtSound(MobDamageSource source) const override;
        const char* GetDeathSound() const override;
        void PlayEatingSound() override;

    protected:
        TemperatureVariant m_variant = TemperatureVariant::Temperate;
        int8_t m_breedPartnerVariant = -1;   // -1 = no partner known
        uint8_t m_soundVariant = 0;         // FarmSoundVariants, 0 = classic
        ItemBasedSteering m_steering;        // MC Pig.steering
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

        // MC Sheep.shear(level, soundSource, tool) — drop the wool and set the
        // sheared flag; the sound in `soundSource` (PLAYERS from a player,
        // BLOCKS from a dispenser).
        void Shear(SoundSource soundSource = SoundSource::Players);

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

        // MC 1.21.5 ChickenVariants: temperate / warm / cold, chosen by biome in
        // FinalizeSpawn (BiomeTags.SPAWNS_WARM_VARIANT_FARM_ANIMALS and
        // SPAWNS_COLD_VARIANT_FARM_ANIMALS beat the temperate fallback),
        // inherited from a random parent when bred, saved as "variant"
        // (minecraft:temperate|warm|cold) and carried by the wire's variant
        // byte (DATA_VARIANT_ID).
        TemperatureVariant GetVariant() const { return m_variant; }
        void SetVariant(TemperatureVariant v) { m_variant = v; }
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_variant); }
        void    SetVariantByte(uint8_t v) override {
            m_variant = v <= 2 ? static_cast<TemperatureVariant>(v) : TemperatureVariant::Temperate;
        }
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;
        // MC getBreedOffspring: the baby takes THIS parent's or the partner's
        // variant at random. The partner is only known here, so it is noted
        // for the CreateBaby that follows.
        void SpawnChildFromBreeding(Animal& partner) override;
        // MC <Mob>SoundVariants (FarmSoundVariants.hpp): the sound set,
        // picked at random in FinalizeSpawn, saved as "sound_variant" and set
        // by the item's <mob>/sound_variant. Classic answers the type's own
        // sounds (EntitySounds).
        uint8_t GetSoundVariant() const { return m_soundVariant; }
        void SetSoundVariant(uint8_t v) { m_soundVariant = v; }
        const char* GetAmbientSound() const override;
        const char* GetHurtSound(MobDamageSource source) const override;
        const char* GetDeathSound() const override;

    protected:
        TemperatureVariant m_variant = TemperatureVariant::Temperate;
        int8_t m_breedPartnerVariant = -1;   // -1 = no partner known
        uint8_t m_soundVariant = 0;         // FarmSoundVariants, 0 = classic
    public:

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

    // MC Parrot (extends ShoulderRidingEntity extends TamableAnimal).
    // MAX_HEALTH 6, FLYING_SPEED 0.4, MOVEMENT_SPEED 0.2, ATTACK_DAMAGE 3.
    //
    // FlyingMoveControl(10, false) + the flying navigation, the flap state
    // machine (calculateFlapping — also the slow fall: descending motion is
    // scaled by 0.6, and checkFallDamage is a no-op), never breeding,
    // seed-taming (1/10), sit-on-command, follow-owner (it may perch on
    // leaves: canFlyToOwner), the cookie poison-kill, the five-colour
    // Parrot.Variant (rolled in finalizeSpawn, saved as "Variant", on the
    // wire as the variant byte), the tree-perching wander (ParrotWanderGoal),
    // FollowMobGoal, the shoulder ride (LandOnOwnersShoulderGoal + the
    // ShoulderRidingEntity cooldown; the player half lives on the server's
    // player, see server/entity/ShoulderEntities), mob imitation and the
    // jukebox party dance.
    class Parrot : public Animal, public TamableAnimal {
    public:
        // Mob::IsTamedPet — a tamed one never despawns (IsDespawnPersistent).
        bool IsTamedPet() const override { return IsTame(); }

        // MC Parrot.Variant, declaration order = id (ByIdMap.continuous,
        // OutOfBoundsStrategy.CLAMP).
        enum class Variant : uint8_t { RedBlue = 0, Blue = 1, Green = 2, YellowBlue = 3, Gray = 4 };
        static constexpr int kVariantCount = 5;
        // MC Parrot.Variant.byId: out-of-range ids CLAMP to the ends.
        static Variant VariantById(int id) {
            return static_cast<Variant>(id < 0 ? 0 : (id >= kVariantCount ? kVariantCount - 1 : id));
        }
        // MC ParrotRenderer.getVariantTexture.
        static const char* VariantTexture(Variant variant);

        // MC ShoulderRidingEntity.RIDE_COOLDOWN.
        static constexpr int kRideCooldown = 100;
        // The jukebox counts while it is closer than this to the block's
        // centre (MC Parrot.aiStep's closerToCenterThan, 3.46 there — widened
        // on purpose, see ParrotDanceRange.hpp).
        static constexpr double kJukeboxRange = kParrotDanceRange;

        explicit Parrot(EntityLevel* level);

        // MC Parrot.finalizeSpawn — a random variant for every spawn reason
        // (natural, spawn egg, command, structure), then super.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        Variant GetVariant() const { return m_variant; }
        void    SetVariant(Variant v) { m_variant = v; }
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_variant); }
        void    SetVariantByte(uint8_t v) override { m_variant = VariantById(v); }

        // MC Parrot.mobInteract — seeds tame (1/10), cookies kill, a tame
        // grounded parrot toggles sitting.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Parrot.hurtServer: a hit parrot stops sitting first.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

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
        // anim state byte.
        uint8_t GetAnimStateByte() const override { return GetTamableAnimByte(); }
        void    SetAnimStateByte(uint8_t v) override { SetTamableAnimByte(v); }

        // MC Parrot.isFood: false — seeds TAME, they never breed.
        bool IsFood(uint32_t itemId) const override { (void)itemId; return false; }
        // MC Parrot.getBreedOffspring returns null; canMate is false.
        std::unique_ptr<Animal> CreateBaby() override { return nullptr; }
        bool CanMate(const Animal& other) const override { (void)other; return false; }
        // MC Parrot.canBeABaby: false — parrots have no baby form.
        bool IsBaby() const override { return false; }

        bool IsFlyingAnimal() const override { return true; }

        // MC Parrot.isPushable: true (the living default gates on alive).
        bool IsPushable() const override { return true; }
        // MC Parrot.doPush: a player is never pushed by (nor pushes) the
        // parrot through its own push.
        void DoPush(Entity& other) override;

        // MC ShoulderRidingEntity.tick — the ride cooldown counts up — then
        // super.
        void Tick() override;
        // MC ShoulderRidingEntity.canSitOnShoulder.
        bool CanSitOnShoulder() const { return m_rideCooldownCounter > kRideCooldown; }

        // MC Parrot.aiStep — the jukebox check, the imitation roll, super,
        // calculateFlapping.
        void AiStep() override;

        // MC Parrot.setRecordPlayingNearby / isPartyParrot.
        void SetRecordPlayingNearby(const glm::ivec3& pos, bool playing) override;
        bool IsPartyParrot() const { return m_partyParrot; }

        // MC Parrot.getAmbientSound (a 1/1000 mob imitation outside
        // peaceful) and getVoicePitch (no baby shift).
        const char* GetAmbientSound() const override;
        float GetVoicePitch() const override;

        // MC Parrot.getAmbient / getPitch / imitateNearbyMobs — static in MC
        // because the shoulder parrot (ServerPlayer.playShoulderEntity
        // AmbientSound) speaks through them with the PLAYER as the entity.
        static const char* GetAmbient(EntityLevel& level, JavaRandom& random);
        static float GetPitch(JavaRandom& random);
        static bool ImitateNearbyMobs(EntityLevel& level, const Entity& entity);

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

        // MC Parrot.isFlapping / onFlap — PARROT_FLY every half flap-speed
        // of flight distance.
        bool IsFlapping() const override;
        void OnFlap() override;

    private:
        // MC Parrot.calculateFlapping — the flap fields verbatim.
        void CalculateFlapping();
        // The party state (MC aiStep's head, event-driven) — see
        // UpdatePartyState.
        void UpdatePartyState();
        bool IsJukeboxStillPlaying(const glm::ivec3& pos) const;
        bool IsWithinJukeboxRange(const glm::ivec3& pos) const;

        Variant m_variant = Variant::RedBlue;   // MC Variant.DEFAULT
        float m_flap = 0.0f, m_oFlap = 0.0f;
        float m_flapSpeed = 0.0f, m_oFlapSpeed = 0.0f;
        float m_flapping = 1.0f;
        float m_nextFlap = 1.0f;
        int   m_rideCooldownCounter = 0;
        bool  m_partyParrot = false;
        // The jukeboxes whose songs this parrot was told of (song start, or
        // the one check on arrival), each kept while its song plays.
        std::vector<glm::ivec3> m_jukeboxes;
        bool  m_jukeboxSearched = false;     // client: the one on-arrival check ran
        int   m_lastDanceLogTick = -1000;    // diagnostics rate limit
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
    // particles.
    class Rabbit : public Animal {
    public:
        explicit Rabbit(EntityLevel* level);

        // MC Rabbit.Variant (ids as the save and the wire carry them).
        enum class Variant : uint8_t {
            Brown = 0, White = 1, Black = 2, WhiteSplotched = 3, Gold = 4, Salt = 5, Evil = 99,
        };
        static bool IsValidVariant(int id) { return (id >= 0 && id <= 5) || id == 99; }
        static const char* VariantName(Variant v);
        Variant GetVariant() const { return m_variant; }
        // MC Rabbit.setVariant: the killer bunny (EVIL) gains its armour,
        // attack goals, +5 attack damage and its name.
        void SetVariant(Variant v);
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_variant); }
        void    SetVariantByte(uint8_t v) override {
            m_variant = IsValidVariant(v) ? static_cast<Variant>(v) : Variant::Brown;
        }
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;
        void SpawnChildFromBreeding(Animal& partner) override;
        SoundSource GetSoundSource() const override;
        bool DoHurtTarget(Entity& target) override;

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

        // MC 26.1 Rabbit.setupAnimationStates (client side, from baseTick):
        // the remodeled rabbit's hop clip runs while a jump is in flight and
        // the idle head tilt fires every 180..220 ticks of standing still.
        // The classic mesh reads jumpCompletion instead and ignores both.
        void SetupAnimationStates() override;

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
        int  m_idleAnimationTimeout = 0;   // MC 26.1 Rabbit.idleAnimationTimeout (client)
        Variant m_variant = Variant::Brown;
        int     m_breedPartnerVariant = -1;
        bool    m_evilGoalsAdded = false;
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

        // MC PolarBear.playWarningSound — the growl, at most every 40 ticks.
        void PlayWarningSound();

        // MC PolarBear.tick — the client-side 0..6 stand animation lerp.
        void Tick() override;

        // MC PolarBear.getDefaultDimensions — the hitbox grows with the
        // stand animation (client-side only, where the animation runs).
        float BaseBbHeight() const override;

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

    // MC 26.3 animal/wolf/Wolf — TamableAnimal + NeutralMob.
    //
    // Built on GenericAnimal for the def's attributes and locomotion only:
    // the constructor clears the def-driven goal set and registers
    // Wolf.registerGoals exactly, priority for priority (MC's wolf has no
    // TemptGoal and no FollowParentGoal, and its panic is the
    // environmental-only TamableAnimalPanicGoal — a hit wolf fights, it does
    // not run).
    //
    // Synched data (MC DATA_*), and where each rides this port's wire:
    //   anim byte    bit 0 sitting pose, bit 1 tame (TamableAnimal's flags),
    //                bit 2 DATA_INTERESTED_ID (the beg head tilt),
    //                bit 3 isAngry() — MC syncs DATA_ANGER_END_TIME and the
    //                client compares it with its own game time; the server
    //                evaluates the same comparison each tick and ships the
    //                answer, which is what the renderer's angry sheet, raised
    //                tail and still tail read,
    //                bits 4-7 DATA_COLLAR_COLOR (a DyeColor ordinal).
    //   variant byte bits 0-3 DATA_VARIANT_ID (WolfVariants::Variant),
    //                bits 4-6 DATA_SOUND_VARIANT_ID.
    //   BodyArmorS2C the BODY equipment slot (wolf armor), whole stack — the
    //                armour layer needs its damage and dye.
    //
    // Not modelled, named at its site: armour trims/glint on the armour
    // layer. (Rain soaks a wolf like water does — Wolf::Tick's
    // isInWaterOrRain.)
    class Wolf : public GenericAnimal, public NeutralMob, public TamableAnimal {
    public:
        // Mob::IsTamedPet — a tamed one never despawns (IsDespawnPersistent).
        bool IsTamedPet() const override { return IsTame(); }

        explicit Wolf(EntityLevel* level);

        // MC Wolf.startPersistentAngerTimer — rangeOfSeconds(20, 39).
        void StartPersistentAngerTimer() override;

        // MC Wolf.aiStep: the server's shake start (wet, not already
        // shaking, not path-finding, on the ground → entity event 8) and
        // updatePersistentAnger(level, true).
        void AiStep() override;

        // MC Wolf.tick: the interested-angle spring and the whole wet/shake
        // cycle (sound, splash particles), both sides.
        void Tick() override;

        // MC Wolf.die: a dying wolf stops shaking.
        void Die(MobDamageSource source, Entity* attacker) override;

        // MC Wolf.mobInteract, every branch (see the .cpp).
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Wolf.applyTamingSideEffects: MAX_HEALTH 40 tame, 8 wild.
        void ApplyTamingSideEffects() override;

        // MC Wolf.hurtServer: a hit wolf stands up.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Wolf.actuallyHurt: worn wolf armor takes the whole hit (unless
        // #bypasses_wolf_armor), cracking audibly at each Crackiness step.
        void ActuallyHurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Wolf.getMaxHeadXRot: 20 while sitting.
        int GetMaxHeadXRot() const override {
            return IsInSittingPose() ? 20 : GenericAnimal::GetMaxHeadXRot();
        }

        // MC Wolf.getMaxSpawnClusterSize — a pack of 8.
        int GetMaxSpawnClusterSize() const override { return 8; }

        // MC TamableAnimal.canAttack — never the owner.
        bool CanAttack(const LivingEntity& target) const override {
            return TamableCanAttack(target) && GenericAnimal::CanAttack(target);
        }

        // MC Wolf.handleEntityEvent: 8 begin shake, 56 cancel shake; then
        // TamableAnimal's 7/6 taming particles; 65 the armour's break sound.
        void HandleEntityEvent(uint8_t id) override;

        // MC Wolf.wantsToAttack, verbatim.
        bool WantsToAttack(const LivingEntity& target,
                           const LivingEntity& owner) const override;

        // MC Wolf.canMate: both tame, partner not sitting, both in love.
        bool CanMate(const Animal& other) const override;

        // MC Wolf.getBreedOffspring — coat from a random parent, tame +
        // owner + mixed collar from a tame parent, a fresh sound variant.
        // The partner is only known in SpawnChildFromBreeding, so it is
        // noted there for the CreateBaby that follows (the Cow pattern); a
        // spawn egg on an adult breeds with the adult itself, as in MC.
        std::unique_ptr<Animal> CreateBaby() override;
        void SpawnChildFromBreeding(Animal& partner) override;

        // MC Wolf.finalizeSpawn: the coat by biome (shared through the
        // pack's WolfPackData), then a random sound variant.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // ── Wire bytes (see the class note) ────────────────────────────────
        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;
        uint8_t GetVariantByte() const override;
        void    SetVariantByte(uint8_t v) override;
        // Mob::GetRenderPhase: the beg tilt (isInterested + interestedAngle)
        // and the shake (isWet, isShaking, shakeAnim).
        int GetRenderPhase(float* out) const override {
            out[0] = m_interested ? 1.0f : 0.0f;
            out[1] = m_interestedAngle;
            out[2] = m_interestedAngleO;
            out[3] = m_isWet ? 1.0f : 0.0f;
            out[4] = m_isShaking ? 1.0f : 0.0f;
            out[5] = m_shakeAnim;
            out[6] = m_shakeAnimO;
            return 7;
        }
        void SetRenderPhase(const float* in, int count) override {
            if (count < 7) return;
            m_interested       = in[0] != 0.0f;
            m_restoredInterest = m_interested;   // see Wolf::AiStep
            m_interestedAngle  = in[1];
            m_interestedAngleO = in[2];
            m_isWet            = in[3] != 0.0f;
            m_isShaking        = in[4] != 0.0f;
            m_shakeAnim        = in[5];
            m_shakeAnimO       = in[6];
        }

        // ── Coat / sound / collar ──────────────────────────────────────────
        WolfVariants::Variant GetVariant() const { return m_variant; }
        void SetVariant(WolfVariants::Variant v) { m_variant = v; }
        WolfSoundVariants::SoundVariant GetSoundVariant() const { return m_soundVariant; }
        void SetSoundVariant(WolfSoundVariants::SoundVariant v) { m_soundVariant = v; }
        // MC getCollarColor / setCollarColor (DyeColor ordinal, default RED).
        uint8_t GetCollarColor() const { return m_collarColor; }
        void    SetCollarColor(uint8_t c) { m_collarColor = static_cast<uint8_t>(c & 0x0F); }

        // MC Wolf.isAngry() as each side sees it: the server's anger window,
        // the client's synched copy of the same answer.
        bool IsAngryState() const;

        // MC Wolf.getTexture — the variant's tame / angry / wild sheet.
        std::string GetTexturePath() const;

        // MC Wolf.setIsInterested / isInterested — BegGoal's head tilt.
        void SetIsInterested(bool v) { m_interested = v; }
        bool IsInterested() const { return m_interested; }

        // MC Wolf.getHeadRollAngle / getShakeAnim / getWetShade — what
        // WolfRenderer.extractRenderState reads.
        float GetHeadRollAngle(float partialTick) const;
        float GetShakeAnim(float partialTick) const;
        float GetWetShade(float partialTick) const;

        // MC Wolf.getTailAngle: angry 1.5393804; tame scales with health;
        // wild idle DEFAULT_TAIL_ANGLE.
        float GetTailAngle() const;

        // ── Body armor (MC's BODY equipment slot) ──────────────────────────
        const ItemStack& GetBodyArmorItem() const { return m_bodyArmor; }
        bool IsWearingBodyArmor() const { return !m_bodyArmor.IsEmpty(); }
        // Server: equips (or clears) the slot, applying the item's armour
        // modifier, and marks it for the tracker. Client: the synched copy.
        void SetBodyArmorItem(const ItemStack& stack);
        // The tracker's send-on-change latch for BodyArmorS2C.
        bool ConsumeBodyArmorDirty() {
            const bool d = m_bodyArmorDirty;
            m_bodyArmorDirty = false;
            return d;
        }
        // LivingEntity's equipment view — the BODY slot only.
        ItemStack* EquipmentInSlot(EquipmentSlot slot) override;
        bool HasEquipmentSlots() const override { return true; }
        // MC Mob.dropCustomDeathLoot: the guaranteed BODY-slot drop
        // (setItemSlotAndDropWhenKilled).
        void DropCustomDeathLoot(EntityLevel& level) override;

        void ClearReferenceTo(const Entity* entity) override {
            GenericAnimal::ClearReferenceTo(entity);
            ClearAngerReferenceTo(entity);
            ClearOwnerReferenceTo(entity);
        }

        // MC Wolf's sound hooks, through the wolf's sound variant (the baby
        // set for a pup). getHurtSound is WOLF_ARMOR_DAMAGE when the armour
        // takes the hit.
        const char* GetAmbientSound() const override;
        const char* GetHurtSound(MobDamageSource source) const override;
        const char* GetDeathSound() const override;

        // MC Wolf.PREY_SELECTOR, for NonTameRandomTargetGoal.
        static bool IsPrey(EntityTypeId type) {
            return type == EntityTypeId::Sheep || type == EntityTypeId::Rabbit ||
                   type == EntityTypeId::Fox;
        }

    private:
        void RegisterWolfGoals();

        // MC Wolf.tryToTame — the 1/3 bone roll.
        void TryToTame(LivingEntity& player);

        // MC Wolf.cancelShake.
        void CancelShake();

        // Engine repair (not in MC), the first server steps after the wolf
        // exists: a TAMED wolf whose saved grudge or target is one of the
        // wild hunt's prey (sheep, rabbit, fox, turtle — what only
        // NonTameRandomTargetGoal hunts) drops it. Worlds saved while
        // NonTameRandomTargetGoal / the owner goals resurrected cleared
        // targets carry tamed wolves angry at sheep; this clears them on load.
        void HealLoadedTameAnger();

        // MC Wolf.canArmorAbsorb: wearing wolf armor, and the source is not
        // in #bypasses_wolf_armor.
        bool CanArmorAbsorb(MobDamageSource source) const;

        // MC Mob.attemptToShearEquipment for the BODY slot (Entity.interact's
        // shears branch, reached from MobInteract — see the .cpp).
        bool TryShearBodyArmor(LivingEntity& player, ItemStack& shears);

        WolfVariants::Variant           m_variant = WolfVariants::kDefault;
        WolfSoundVariants::SoundVariant m_soundVariant = WolfSoundVariants::SoundVariant::Classic;
        uint8_t m_collarColor = kDyeColorRed;

        // MC Wolf.interestedAngle(+O).
        bool  m_interested = false;
        bool  m_restoredInterest = false;   // set by SetRenderPhase, see AiStep
        float m_interestedAngle = 0.0f;
        float m_interestedAngleO = 0.0f;

        // The client's copy of isAngry() (anim byte bit 3).
        bool  m_clientAngry = false;

        // MC Wolf.isWet / isShaking / shakeAnim(O).
        bool  m_isWet = false;
        bool  m_isShaking = false;
        float m_shakeAnim = 0.0f;
        float m_shakeAnimO = 0.0f;

        ItemStack m_bodyArmor;
        bool      m_bodyArmorDirty = false;
        // The stack whose attribute modifiers are on m_attributes right now
        // (collectEquipmentChanges' lastBodyItemStack): the BODY armour as it
        // was when last applied, so an in-place change can take the old
        // modifiers off.
        ItemStack m_bodyArmorModifiersFrom;
        // Client: the last armour the server showed, so entity event 65
        // (the break) still knows its sound when the emptied slot's update
        // lands first.
        ItemStack m_lastBodyArmorSeen;

        // Set for the duration of SpawnChildFromBreeding.
        const Wolf* m_breedPartner = nullptr;

        // HealLoadedTameAnger's state: pending until the grudge the wolf
        // started with resolves (or ends); the uuid is that grudge, so a NEW
        // one the owner starts meanwhile is never touched.
        bool m_healLoadedAnger = true;
        bool m_healAngerUuidTaken = false;
        Uuid m_healAngerUuid{};
    };

    // MC Wolf.WolfPackData — the coat the first member of a natural pack
    // rolled, shared by the rest (AgeableMobGroupData(false): no pups).
    struct WolfPackData : SpawnGroupData {
        explicit WolfPackData(WolfVariants::Variant v) : variant(v) {}
        WolfVariants::Variant variant;
    };

    // MC animal/equine/Llama (extends AbstractChestedHorse extends
    // AbstractHorse — the equine half is the HorseTaming mixin here). MC's
    // own goal table replaces the generic base's wholesale: the taming buck,
    // the caravan (LlamaFollowCaravanGoal), the spit (RangedAttackGoal(1.25,
    // 40, 20)), LlamaHurtByTargetGoal (spit once, stand down) and
    // LlamaAttackWolfGoal. The chested-horse attributes (MAX_HEALTH rolled
    // 15..30 at spawn and inherited with the speed and jump on breeding),
    // AbstractHorse's shared rules (pushable only while unridden, the 1-in-3
    // hurt roll, the slow self-heal) and the llama's own fall damage (none
    // below six blocks). The coat (MC Llama.Variant, DATA_VARIANT_ID, saved
    // as "Variant") rides the wire's variant byte. A llama is never steered:
    // it wears no saddle, so AbstractHorse.getControllingPassenger falls
    // through to Mob's (no player) and a rider goes where its AI takes it.
    class Llama : public GenericAnimal, public RangedAttackMob, public HorseTaming {
    public:
        // Its rolled health, and the inherited speed/jump, reach the client
        // (UpdateAttributesS2C) — the rider's vehicle hearts read them.
        bool SyncsAttributesToClient() const override { return true; }
        // ── Taming (MC Llama extends AbstractChestedHorse extends
        // AbstractHorse — the HorseTaming mixin here). Tamed by riding, as
        // the horses; a llama cannot be steered (no saddle), and the
        // chest/carpet/caravan systems are not carried by this port.
        // Mob::IsTamedPet — a tamed llama never despawns.
        bool IsTamedPet() const override { return IsTamed(); }
        // MC Llama.getMaxTemper: 30.
        int GetMaxTemper() const override { return 30; }
        // MC AbstractHorse.makeMad for a llama: canPerformRearing is false,
        // so only the angry sound (LLAMA_ANGRY) — server only.
        void MakeMad() override;
        // MC AbstractChestedHorse.mobInteract → AbstractHorse.mobInteract.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // MC AbstractHorse.fedFood with Llama.handleEating's table.
        UseResult FedFood(LivingEntity& player, ItemStack& held);
        bool HandleEating(LivingEntity& player, const ItemStack& held);
        // MC AbstractHorse.doPlayerRide (TraderLlama refuses while it is on
        // a wandering trader's lead).
        virtual void DoPlayerRide(LivingEntity& player);
        // PlayerRideable — MC Llama.getPassengerAttachmentPoint: the type's
        // (0, 1.37, -0.3) turned to the yaw, less the player's 0.6.
        glm::dvec3 PlayerRiderPosition() const override;
        // MC AbstractHorse.getDismountLocationForPassenger (the llama is one).
        glm::dvec3 GetDismountLocationForPassenger(const LivingEntity& passenger) const override {
            return EquineDismountLocation(*this, passenger);
        }
        // MC entity events 7 / 6: the taming hearts / smoke.
        void HandleEntityEvent(uint8_t id) override;
        void ClearReferenceTo(const Entity* entity) override {
            GenericAnimal::ClearReferenceTo(entity);
            ClearOwnerReferenceTo(entity);
            // The caravan links are plain references, as MC's fields are.
            if (m_caravanHead && static_cast<const Entity*>(m_caravanHead) == entity) m_caravanHead = nullptr;
            if (m_caravanTail && static_cast<const Entity*>(m_caravanTail) == entity) m_caravanTail = nullptr;
        }

        // ── The caravan (MC Llama.caravanHead / caravanTail) ───────────────
        // MC leaveCaravan: the llama ahead loses its tail, this its head.
        void LeaveCaravan() {
            if (m_caravanHead) m_caravanHead->m_caravanTail = nullptr;
            m_caravanHead = nullptr;
        }
        // MC joinCaravan(tail): fall in behind `head`.
        void JoinCaravan(Llama& head) {
            m_caravanHead = &head;
            head.m_caravanTail = this;
        }
        bool   HasCaravanTail() const { return m_caravanTail != nullptr; }
        bool   InCaravan() const { return m_caravanHead != nullptr; }
        Llama* GetCaravanHead() const { return m_caravanHead; }

        // MC Llama.isTraderLlama.
        virtual bool IsTraderLlama() const { return false; }

        // ── Equipment and chest (MC Llama / AbstractChestedHorse /
        //    AbstractHorse; the system is MountInventory's) ─────────────────
        // MC Llama.canUseSlot: every slot — the carpet (BODY) included; no
        // saddle ever fits (#can_equip_saddle leaves the llamas out).
        bool CanUseSlot(EquipmentSlot) const override { return true; }
        // MC AbstractHorse.canDispenserEquipIntoSlot.
        bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const override {
            return ((slot == EquipmentSlot::BODY || slot == EquipmentSlot::SADDLE) && IsTamed()) ||
                   GenericAnimal::CanDispenserEquipIntoSlot(slot);
        }
        // MC AbstractHorse.getEquipSound: HORSE_SADDLE for the saddle slot.
        std::string GetEquipSound(EquipmentSlot slot, const ItemStack& stack,
                                  const Equippable& equippable) const override {
            return slot == EquipmentSlot::SADDLE ? std::string(SoundEvents::HORSE_SADDLE)
                                                 : GenericAnimal::GetEquipSound(slot, stack, equippable);
        }
        // MC AbstractHorse.equipBodyArmor — a carpet on the llama.
        void EquipBodyArmor(LivingEntity& player, ItemStack& held);
        MountInventory*       GetMountInventory() override       { return &m_mountInventory; }
        const MountInventory* GetMountInventory() const override { return &m_mountInventory; }
        // MC AbstractChestedHorse.hasChest (DATA_ID_CHEST, the anim byte's
        // bit 7 here).
        bool HasChest() const { return m_mountInventory.HasChest(); }
        // MC Llama.getInventoryColumns: the strength while chested.
        int GetInventoryColumns() const override { return m_mountInventory.HasChest() ? GetStrength() : 0; }
        bool HasCustomInventoryScreen() const override { return true; }
        // MC AbstractHorse.openCustomInventoryScreen.
        void OpenCustomInventoryScreen(LivingEntity& player) override;
        // Anim byte: bit 3 FLAG_TAME, bit 7 DATA_ID_CHEST — the equines'
        // bits (AbstractHorse::kAnimTameBit / kAnimChestBit).
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((IsTamed() ? 0x08 : 0) | (m_mountInventory.HasChest() ? 0x80 : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            SetTamed((v & 0x08) != 0);
            m_mountInventory.SetChest((v & 0x80) != 0);
        }

        // ── AbstractHorse's shared rules, as a llama inherits them ─────────
        // MC Llama.isImmobile: dead or eating (a llama never grazes).
        bool IsImmobile() const override { return IsDeadOrDying(); }
        // MC AbstractHorse.isPushable: not while anyone rides it.
        bool IsPushable() const override { return !IsVehicle(); }
        // MC AbstractHorse.getMaxSpawnClusterSize: herds of 6.
        int GetMaxSpawnClusterSize() const override { return 6; }
        // MC AbstractHorse.hurtServer: a 1-in-3 roll to rear (standIfPossible,
        // which canPerformRearing = false makes a no-op for a llama).
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // MC Llama.causeFallDamage: hurt only from six blocks up, the block
        // fall sound whenever there is damage — no HORSE_LAND.
        bool CauseFallDamage(double fallDist, float damageMultiplier) override;
        // MC AbstractHorse.aiStep for a llama: the tail roll (both sides),
        // then the server's 1-in-900 self-heal (canEatGrass is false; the
        // bred-foal followMommy needs the Bred flag nothing sets).
        void AiStep() override;
        // MC Llama.canMate: another llama (either kind), both able to parent
        // (AbstractHorse.canParent: unridden, not riding, tamed, adult, at
        // full health, in love).
        bool CanMate(const Animal& other) const override;
        bool CanParent() const;

        // MC Llama.Variant — id order: CREAMY, WHITE, BROWN, GRAY.
        enum class Variant : uint8_t { Creamy = 0, White = 1, Brown = 2, Gray = 3 };
        static constexpr int kVariantCount = 4;

        explicit Llama(EntityLevel* level) : Llama(EntityTypeId::Llama, level) {}

        // MC Llama.didSpit — read/consumed by LlamaHurtByTargetGoal.
        bool DidSpit() const { return m_didSpit; }
        void SetDidSpit(bool v) { m_didSpit = v; }

        // MC Llama.performRangedAttack — spit(target).
        void PerformRangedAttack(LivingEntity& target, float power) override;

        // MC Llama.getStrength / setStrength (DATA_STRENGTH_ID, saved as
        // "Strength", clamped 1..5 by the setter). Read by the wolf's
        // WolfAvoidEntityGoal: a wild wolf flees a llama whose strength beats
        // a nextInt(5) roll.
        int  GetStrength() const { return m_strength; }
        void SetStrength(int strength) { m_strength = std::clamp(strength, 1, 5); }

        // MC Llama.getVariant / setVariant. Variant.byId clamps an unknown
        // id to CREAMY (the DEFAULT).
        Variant GetVariant() const { return m_variant; }
        void    SetVariant(Variant v) { m_variant = v; }
        static Variant VariantById(int id) {
            return id >= 0 && id < kVariantCount ? static_cast<Variant>(id) : Variant::Creamy;
        }
        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_variant); }
        void    SetVariantByte(uint8_t v) override { m_variant = VariantById(v); }

        // MC Llama.finalizeSpawn: setRandomStrength (1 + nextInt(3), or
        // 1 + nextInt(5) on a 4% roll), then the coat — the pack's
        // (LlamaGroupData), else a uniform pick that becomes the pack's.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Llama.getBreedOffspring: makeNewLlama, AbstractHorse
        // .setOffspringAttributes (health, jump, speed), strength
        // nextInt(max(both)) + 1 (+1 on a 3% roll), a random parent's coat.
        std::unique_ptr<Animal> CreateBaby() override;
        void SpawnChildFromBreeding(Animal& partner) override;

    protected:
        // The variant constructor — TraderLlama is a llama of a different
        // type id, exactly as MC's extends.
        Llama(EntityTypeId type, EntityLevel* level);

        // MC Llama.makeNewLlama — the baby's class.
        virtual std::unique_ptr<Llama> MakeNewLlama();

        // MC Llama.spit — a LlamaSpit from just ahead of the mouth, aimed a
        // third up the target with the 0.2 loft, velocity 1.5, inaccuracy 10.
        void Spit(LivingEntity& target);

    private:
        // MC Llama.registerGoals, priority for priority.
        void RegisterLlamaGoals();

        bool    m_didSpit = false;
        int     m_strength = 0;   // MC DATA_STRENGTH_ID's defined default
        Variant m_variant = Variant::Creamy;   // MC Variant.DEFAULT
        // MC caravanHead / caravanTail (server; cleared by ClearReferenceTo).
        Llama*  m_caravanHead = nullptr;
        Llama*  m_caravanTail = nullptr;
        // Set for the duration of SpawnChildFromBreeding.
        const Llama* m_breedPartner = nullptr;
        // MC AbstractHorse.inventory with AbstractChestedHorse's chest.
        MountInventory m_mountInventory{true};
    };

    class WanderingTrader;

    // MC animal/equine/TraderLlama — the llama that comes with a wandering
    // trader: the blue trader decor (LlamaDecorLayer's TRADER_LLAMA asset),
    // PanicGoal(2.0) on top, it defends the trader it is leashed to
    // (TraderLlamaDefendWanderingTraderGoal) and hunts zombies and illagers,
    // and it despawns with the trader — or on its own 47999-tick clock once
    // off the trader's lead — unless a player leashed it, rides it, or it is
    // persistent or age-locked.
    class TraderLlama : public Llama {
    public:
        static constexpr int kDefaultDespawnDelay = 47999;   // MC DEFAULT_DESPAWN_DELAY

        explicit TraderLlama(EntityLevel* level);

        int  GetDespawnDelay() const { return m_despawnDelay; }
        void SetDespawnDelay(int ticks) { m_despawnDelay = ticks; }

        bool IsTraderLlama() const override { return true; }

        // MC TraderLlama.finalizeSpawn: an EVENT spawn is an adult, and a
        // spawn with no group data gets AgeableMobGroupData(false) (no baby
        // roll) before Llama's strength and coat.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void AiStep() override;

        // The trader it is on the lead of, or null.
        WanderingTrader* GetLeashedWanderingTrader() const;

    protected:
        // MC TraderLlama.makeNewLlama: a trader llama, persistence required.
        std::unique_ptr<Llama> MakeNewLlama() override;

    public:
        // MC TraderLlama.doPlayerRide: not while on a wandering trader's lead.
        void DoPlayerRide(LivingEntity& player) override;

    private:
        void MaybeDespawn();
        bool CanDespawn() const;
        bool IsLeashedToSomethingOtherThanTheWanderingTrader() const;

        int m_despawnDelay = kDefaultDespawnDelay;
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
    // the wire's variant byte, and the mouth-item layer: the item rides the
    // MAINHAND equipment slot (synced like any mob's), rolled at spawn
    // (populateDefaultEquipmentSlots), taken from the ground by
    // FoxSearchForItemsGoal + Mob's looting (canHoldItem / pickUpItem, the
    // old item spat out), picked off berry bushes and glow-berry vines
    // (FoxEatBerriesGoal), eaten after 600 ticks (event 45 crumbs before),
    // and dropped on death. Not modelled, named at its site: villages
    // (FoxStrollThroughVillageGoal, SeekShelterGoal's isVillage term).
    class Fox : public Animal {
    public:
        // MC Fox.canDispenserEquipIntoSlot: only the mouth (main hand), and
        // only a fox that picks up loot.
        bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const override {
            return slot == EquipmentSlot::MAINHAND && CanPickUpLoot();
        }
        // MC Fox.getAmbientSound (sleep / night screech / yip) and
        // playAmbientSound (the screech at volume 2).
        const char* GetAmbientSound() const override;
        void PlayAmbientSound() override;

        // MC Fox.Variant.
        enum class Variant : uint8_t { Red = 0, Snow = 1 };

        explicit Fox(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ItemTags.FOX_FOOD: sweet berries, glow berries.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // ── Trust (MC DATA_TRUSTED_ID_0 / _1, the "Trusted" NBT list) ──────
        //
        // Up to two trusted identities, kept as UUIDs (MC EntityReference):
        // a trusted player may be offline or in another dimension and the
        // fox still knows them. Granted when a player breeds foxes (the cub
        // trusts each feeder — FoxBreedGoal.breed) or hatches a cub with a
        // spawn egg (onOffspringSpawnedFromEgg). A trusted player is not
        // fled from, does not wake the fox, and is defended
        // (DefendTrustedTargetGoal). Server-side state: MC syncs the two
        // slots, but no client code reads them.
        //
        // MC trusts(entity) — does either slot name this entity?
        bool Trusts(const LivingEntity& entity) const;
        // MC addTrustedEntity: the first slot while it is empty, else the
        // second (overwriting it — MC's own behaviour for a third).
        void AddTrustedEntity(const LivingEntity& entity);
        void AddTrustedUuid(const Uuid& uuid);
        // MC clearTrusted.
        void ClearTrusted() { m_trusted[0] = Uuid{}; m_trusted[1] = Uuid{}; }
        // MC getTrustedEntities, as identities (slot 0 first; empty slots skipped).
        std::vector<Uuid> GetTrustedUuids() const;

        // MC FoxBreedGoal.breed: the offspring trusts the love-cause player
        // of each parent (distinct ones both). Captures them, then the shared
        // Animal path makes the cub through CreateBaby, which applies them.
        void SpawnChildFromBreeding(Animal& partner) override;
        // MC Fox.onOffspringSpawnedFromEgg: a cub from a spawn egg used on
        // this fox trusts the player who used it.
        void OnOffspringSpawnedFromEgg(LivingEntity& spawner, Mob& offspring) override;

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
        // Mob::GetRenderPhase: the head tilt (interested flag + ramp) and the
        // crouch ramp.
        int GetRenderPhase(float* out) const override {
            out[0] = IsInterested() ? 1.0f : 0.0f;
            out[1] = m_interestedAngle;
            out[2] = m_interestedAngleO;
            out[3] = m_crouchAmount;
            out[4] = m_crouchAmountO;
            return 5;
        }
        void SetRenderPhase(const float* in, int count) override {
            if (count < 5) return;
            SetIsInterested(in[0] != 0.0f);
            m_restoredInterest = in[0] != 0.0f;   // see Fox::AiStep
            m_interestedAngle  = in[1];
            m_interestedAngleO = in[2];
            m_crouchAmount     = in[3];
            m_crouchAmountO    = in[4];
        }

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

        // MC Fox.aiStep — the mouth-item eating clock (ticksSinceEaten: a
        // held food is eaten after 600 ticks, crumbs from 560), the
        // target-loss state clear and the sleeping input freeze.
        void AiStep() override;

        // ── The mouth item (MC Fox's MAINHAND) ────────────────────────────
        // MC canHoldItem: an empty mouth, or a food over a non-food once the
        // fox has gone a tick without eating.
        bool CanHoldItem(const ItemStack& stack) const override;
        // MC pickUpItem: one of the stack goes in the mouth (the rest drops
        // where it lay), the old mouth item is spat out, the slot becomes a
        // guaranteed drop.
        void PickUpItem(int32_t itemEntityId, const ItemStack& stack) override;
        // MC populateDefaultEquipmentSlots: 20% carry a trinket.
        void PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty) override;
        // MC handleEntityEvent 45: eating crumbs of the mouth item.
        void HandleEntityEvent(uint8_t id) override;
        // MC dropAllDeathLoot's override: the mouth item always drops.
        void DropEquipment(EntityLevel& level) override;
        // MC isConsumableFood: FOOD and CONSUMABLE.
        static bool IsConsumableFood(const ItemStack& stack);
        int GetTicksSinceEaten() const { return m_ticksSinceEaten; }

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
        Uuid    m_trusted[2] = {};                 // nil = empty slot
        std::vector<Uuid> m_pendingOffspringTrust; // SpawnChildFromBreeding → CreateBaby
        float   m_interestedAngle = 0.0f, m_interestedAngleO = 0.0f;
        float   m_crouchAmount = 0.0f, m_crouchAmountO = 0.0f;
        bool    m_restoredInterest = false;   // set by SetRenderPhase, see AiStep
        bool    m_targetGoalsSet = false;
        int     m_ticksSinceEaten = 0;       // MC ticksSinceEaten
        // MC spitOutItem / dropItemStack.
        void SpitOutItem(const ItemStack& stack);
        void DropItemStack(const ItemStack& stack);
        bool CanEat(const ItemStack& itemInMouth) const;
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
    // tables handle death only), the per-state sounds, and the 0.3 baby
    // scale (the renderer's baby scale is global).
    class Turtle : public Animal {
    public:
        // MC Turtle.getAmbientSound — TURTLE_AMBIENT_LAND for an adult ashore.
        const char* GetAmbientSound() const override;

        // MC Turtle.isPushedByFluid: false.
        bool IsPushedByFluid() const override { return false; }

    public:
        explicit Turtle(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ItemTags.TURTLE_FOOD: seagrass. A block item, resolved by slug.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // MC Turtle.thunderHit: hurtServer(lightningBolt, Float.MAX_VALUE) —
        // lightning always kills a turtle. No fire.
        void ThunderHit(Entity* bolt) override;
        // entities/turtle's second pool: a bowl when the killing blow was
        // #is_lightning (damage_source_properties — a conditional pool the
        // generated mob-loot rows cannot carry).
        void DropCustomDeathLoot(EntityLevel& level) override;

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
    // renderer's per-type texture table does not switch on it yet). The
    // held-food layer rides MAINHAND: pickUpItem takes bamboo / cake off the
    // ground (#panda_eats_from_ground), PandaSitGoal sits to chew it, the
    // EAT_COUNTER clock with its crumbs and chomps eats it (ground food is
    // used up after 100 ticks), a fed adult sits and chews the bamboo it was
    // given, and a sneeze may gift a slime ball (gameplay/panda_sneeze).
    class Panda : public Animal {
    public:
        // MC Panda.canDispenserEquipIntoSlot: the main hand of a panda that
        // picks up loot.
        bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const override {
            return slot == EquipmentSlot::MAINHAND && CanPickUpLoot();
        }
        // MC Panda.playAttackSound (PANDA_BITE) and getAmbientSound
        // (aggressive / worried / plain).
        void PlayAttackSound() override;
        const char* GetAmbientSound() const override;

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
            // Bit 6: MC's EAT_COUNTER > 0 (isEating). The client runs its
            // own counter from there, as MC's does off the synched int.
            if (m_eatCounter > 0) b |= 0x40;
            return b;
        }
        void SetAnimStateByte(uint8_t v) override {
            m_flags = static_cast<uint8_t>(v & 0x1E);
            // Client-side stand-ins for the two derived bits: the unhappy
            // head-shake pose and the scared sit both read these directly.
            m_clientUnhappy = (v & 0x01) != 0;
            m_clientScared = (v & 0x20) != 0;
            if ((v & 0x40) == 0) m_eatCounter = 0;
            else if (m_eatCounter == 0) m_eatCounter = 1;
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

        // MC Panda.isEating / eat(bool) — EAT_COUNTER > 0.
        bool IsEatingPanda() const { return m_eatCounter > 0; }
        void Eat(bool value) { m_eatCounter = value ? 1 : 0; }
        int  GetEatCounter() const { return m_eatCounter; }

        // MC Panda.canPickUpAndEat: #panda_eats_from_ground (bamboo, cake),
        // past its pickup delay.
        static bool CanPickUpAndEat(const ItemStack& stack);
        // MC Panda.pickUpItem: into an empty paw only, the whole stack, a
        // guaranteed drop.
        void PickUpItem(int32_t itemEntityId, const ItemStack& stack) override;

        // MC Panda.canPerformAction.
        bool CanPerformAction() const {
            return !IsOnBack() && !IsScared() && !IsEatingPanda() && !IsRolling()
                && !IsSitting();
        }

        // MC Panda.tryToSit — used by the worried thunder path.
        void TryToSit();

        // MC Panda.mobInteract — bamboo feeding (the item exists as a block
        // item): stand a rolled-over panda up, age a cub, court an adult, and
        // sit a fed one down to chew the bamboo (the old paw item dropped).
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
        // MC Panda.handleEating / addEatingParticles / afterSneeze's gift.
        void HandleEating();
        void AddEatingParticles();
        void DropSneezeGift();

        Gene m_mainGene = Gene::Normal;
        Gene m_hiddenGene = Gene::Normal;
        uint8_t m_flags = 0;
        int  m_unhappyCounter = 0;
        int  m_sneezeCounter = 0;
        int  m_eatCounter = 0;      // MC EAT_COUNTER
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
    // (reassessTrustingGoals) and never scares off the tempt. Its sounds are
    // the generated row's.
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
    // byte carries the 11-texture variant id; the owner dyes the collar
    // (DATA_COLLAR_COLOR, the anim byte's high nibble), which CatCollarLayer
    // tints with the dye's texture-diffuse colour.
    class Cat : public Animal, public TamableAnimal {
    public:
        // Mob::IsTamedPet — a tamed one never despawns (IsDespawnPersistent).
        bool IsTamedPet() const override { return IsTame(); }

        // MC Cat.getAmbientSound / playEatingSound / hiss / hurt / death off
        // its CatSoundVariant (classic or royal; babies share one set).
        const char* GetAmbientSound() const override;
        const char* GetHurtSound(MobDamageSource source) const override;
        const char* GetDeathSound() const override;
        void PlayEatingSound() override;
        void Hiss();
        uint8_t GetSoundVariant() const { return m_soundVariant; }
        void SetSoundVariant(uint8_t v) { m_soundVariant = v; }

        static constexpr int kVariantCount = 11;

        explicit Cat(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC ItemTags.CAT_FOOD: raw cod, raw salmon.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // MC Cat.canMate: both cats tame + the base love test.
        bool CanMate(const Animal& other) const override;

        // MC Cat.mobInteract — collar dye / feed / sit-toggle when owned,
        // fish-taming when wild.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Cat.getCollarColor / setCollarColor (DyeColor ordinal, default
        // RED) — DATA_COLLAR_COLOR, carried in the anim byte's high nibble.
        uint8_t GetCollarColor() const { return m_collarColor; }
        void    SetCollarColor(uint8_t c) { m_collarColor = static_cast<uint8_t>(c & 0x0F); }

        // Breeding: MC Cat.getBreedOffspring mixes the parents' collars, so
        // the partner is noted for the CreateBaby that follows.
        void SpawnChildFromBreeding(Animal& partner) override;

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
        // anim state byte; bits 4-7 carry the collar colour.
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(GetTamableAnimByte() | (m_collarColor << 4));
        }
        void    SetAnimStateByte(uint8_t v) override {
            SetTamableAnimByte(v);
            m_collarColor = static_cast<uint8_t>((v >> 4) & 0x0F);
        }

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

        // Mob::GetRenderPhase: the lying / relax flags and their ramps.
        int GetRenderPhase(float* out) const override {
            out[0] = m_lying ? 1.0f : 0.0f;
            out[1] = m_relaxStateOne ? 1.0f : 0.0f;
            out[2] = m_lieDownAmount;
            out[3] = m_lieDownAmountO;
            out[4] = m_lieDownAmountTail;
            out[5] = m_lieDownAmountOTail;
            out[6] = m_relaxStateOneAmount;
            out[7] = m_relaxStateOneAmountO;
            return 8;
        }
        void SetRenderPhase(const float* in, int count) override {
            if (count < 8) return;
            m_lying                = in[0] != 0.0f;
            m_relaxStateOne        = in[1] != 0.0f;
            m_lieDownAmount        = in[2];
            m_lieDownAmountO       = in[3];
            m_lieDownAmountTail    = in[4];
            m_lieDownAmountOTail   = in[5];
            m_relaxStateOneAmount  = in[6];
            m_relaxStateOneAmountO = in[7];
        }

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

        uint8_t m_soundVariant = 0;   // CatSoundVariants: classic, royal
        uint8_t m_variant = 0;
        uint8_t m_collarColor = kDyeColorRed;   // MC DEFAULT_COLLAR_COLOR
        const Cat* m_breedPartner = nullptr;     // set during SpawnChildFromBreeding
        // Owned by the goal selector; tracked so ReassessTameGoals can
        // remove it (MC keeps the same field).
        class CatAvoidEntityGoal* m_avoidPlayersGoal = nullptr;
        bool m_lying = false;
        bool m_relaxStateOne = false;
        float m_lieDownAmount = 0.0f, m_lieDownAmountO = 0.0f;
        float m_lieDownAmountTail = 0.0f, m_lieDownAmountOTail = 0.0f;
        float m_relaxStateOneAmount = 0.0f, m_relaxStateOneAmountO = 0.0f;
    };

    // MC animal/equine/AbstractHorse — the shared equine base: the DATA_ID_FLAGS
    // byte (eating, standing, open mouth on the anim byte; tame and bred on the
    // server), the eat/stand/mouth ramps ticked on both sides exactly as MC
    // writes them, the 1-in-200 tail swish, the grass-eating roll, taming by
    // riding (HorseTaming), and riding itself: a saddled equine is steered by
    // the player in its seat (getControllingPassenger), turns with the rider's
    // view, strafes at half and backs at a quarter speed, and charges the
    // riders' jump (PlayerRideableJumping — the jump bar) with the rear-up,
    // the jump sound and the forward push. The gallop sounds count ridden
    // steps. The spawn attribute rolls (randomizeAttributes), the herd baby
    // roll, and breeding with inherited attributes (horse × donkey → mule)
    // are MC's. Inventories, chests and armour are the equipment system's.
    struct SoundType;

    class AbstractHorse : public GenericAnimal, public HorseTaming, public PlayerRideableJumping {
    public:
        // Mob::IsTamedPet — a tamed one never despawns (IsDespawnPersistent).
        bool IsTamedPet() const override { return IsTamedHorse(); }

        AbstractHorse(EntityTypeId type, EntityLevel* level);

        // MC AbstractHorse.BACKWARDS_MOVE_SPEED_FACTOR / SIDEWAYS_MOVE_SPEED_FACTOR.
        static constexpr float kBackwardsMoveSpeedFactor = 0.25f;
        static constexpr float kSidewaysMoveSpeedFactor  = 0.5f;
        // MC AbstractHorse.BABY_SCALE.
        static constexpr float kBabyScale = 0.7f;

        // ── MC DATA_ID_FLAGS: FLAG_TAME 2, FLAG_BRED 8, FLAG_EATING 16,
        //    FLAG_STANDING 32, FLAG_OPEN_MOUTH 64 ──────────────────────────
        bool IsEating()    const { return m_eating; }
        bool IsStanding()  const { return m_standing; }
        bool IsBred()      const { return m_bred; }
        bool IsMouthOpen() const { return m_openMouth; }
        void SetEating(bool v) { m_eating = v; }
        void SetBred(bool v) { m_bred = v; }

        // MC AbstractHorse.setStanding(ticks) / clearStanding.
        void SetStanding(int ticks) {
            SetEating(false);
            m_standing = true;
            m_standCounter = ticks;
        }
        void ClearStanding() { m_standing = false; m_standCounter = 0; }

        // MC AbstractHorse.standIfPossible — rear for 20 ticks, where the
        // kind rears and this side decides it (MC `isEffectiveAi() ||
        // !isClientSide()`: the server, or the client steering it).
        void StandIfPossible();

        // MC AbstractHorse.canPerformRearing — true for the whole family
        // except the llama (a separate class here) and the camel.
        virtual bool CanPerformRearing() const { return true; }

        // MC AbstractHorse.getAmbientSoundInterval: 400 (Animal's is 120).
        int GetAmbientSoundInterval() const override { return 400; }

        // MC AbstractHorse.getMaxSpawnClusterSize (AbstractHorse.java:373-375)
        // — herds of 6, for the whole family (horse, donkey, mule, skeleton
        // and zombie horse).
        int GetMaxSpawnClusterSize() const override { return 6; }

        // MC AbstractHorse.getAmbientStandInterval — the ambient interval.
        int GetAmbientStandInterval() const { return GetAmbientSoundInterval(); }

        // Anim byte: bit 0 eating, bit 1 standing, bit 2 open mouth (the
        // synched DATA_ID_FLAGS bits the client reads); bit 3 FLAG_TAME
        // (kAnimTameBit — canUseSlot(SADDLE), and so isSaddled, read it on
        // the steering client); bit 7 the chested equines' DATA_ID_CHEST
        // (kAnimChestBit).
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>((m_eating ? 1 : 0) | (m_standing ? 2 : 0) | (m_openMouth ? 4 : 0) |
                                        (IsTamedHorse() ? kAnimTameBit : 0) |
                                        (m_mountInventory.HasChest() ? kAnimChestBit : 0));
        }
        void SetAnimStateByte(uint8_t v) override {
            m_eating = (v & 1) != 0;
            m_standing = (v & 2) != 0;
            m_openMouth = (v & 4) != 0;
            SetTamedHorse((v & kAnimTameBit) != 0);
            if (m_mountInventory.CanCarryChest()) m_mountInventory.SetChest((v & kAnimChestBit) != 0);
        }
        // The synced tame and chest flags' bits on the anim byte (the
        // llamas' too).
        static constexpr uint8_t kAnimTameBit  = 0x08;
        static constexpr uint8_t kAnimChestBit = 0x80;

        // MC AbstractHorse.hurtServer: 1-in-3 hits make the horse rear.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC AbstractHorse.isImmobile, verbatim precedence:
        // `super.isImmobile() && isVehicle() && isSaddled() || isEating() || isStanding()`.
        bool IsImmobile() const override {
            return (Animal::IsImmobile() && IsVehicle() && IsSaddled()) || IsEating() || IsStanding();
        }

        // MC AbstractHorse.isPushable: never while carrying anyone.
        bool IsPushable() const override { return !IsVehicle(); }

        // MC AbstractHorse.isFood — ItemTags.HORSE_FOOD (the zombie horse's
        // ZOMBIE_HORSE_FOOD).
        bool IsFood(uint32_t itemId) const override;

        // MC AbstractHorse.canMate: false — only the horse and the donkey
        // breed (with each other too), when both canParent.
        bool CanMate(const Animal& other) const override { (void)other; return false; }
        // MC AbstractHorse.canParent.
        bool CanParent() const;
        // Breeding threads the partner through to CreateBaby (the mule, the
        // inherited attributes and coat) — MC getBreedOffspring(partner).
        void SpawnChildFromBreeding(Animal& partner) override;

        // MC AbstractHorse.finalizeSpawn: AgeableMobGroupData(0.2) unless the
        // caller brought one, randomizeAttributes, then AgeableMob's herd baby
        // roll.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // Equines sync their rolled speed / jump / health to the client that
        // steers them (UpdateAttributesS2C).
        bool SyncsAttributesToClient() const override { return true; }

        // MC AbstractHorse.tick — the counters and the three ramps.
        void Tick() override;

        // MC AbstractHorse.aiStep — the tail roll (both sides, MC's own
        // arrangement: each side rolls its own 1-in-200), the server's slow
        // heal and grass-eating roll.
        void AiStep() override;

        // MC AbstractHorse.canEatGrass.
        virtual bool CanEatGrass() const { return true; }

        // ── Taming / temper (HorseTaming) ──────────────────────────────────
        // MC tames equines by riding them: feeding raises temper, an empty
        // hand mounts (doPlayerRide), and RunAroundLikeCrazyGoal rolls temper
        // against nextInt(getMaxTemper()) while the untamed horse bolts —
        // tameWithName, or the rider thrown and temper + 5.
        bool IsTamedHorse() const { return IsTamed(); }
        void SetTamedHorse(bool v) { SetTamed(v); }

        // MC Horse / AbstractChestedHorse / ZombieHorse → AbstractHorse
        // .mobInteract: ridden or a baby → Animal's; tamed + sneaking → the
        // inventory screen; food → fedFood; any other item on an untamed one
        // → makeMad; otherwise the item's own interaction, then
        // doPlayerRide. SkeletonHorse overrides (untamed → Pass).
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC AbstractHorse.doPlayerRide: stop eating/rearing, seat the player.
        void DoPlayerRide(LivingEntity& player);

        // MC AbstractHorse.isMobControlled: false (the zombie horse: a mob
        // rides it).
        virtual bool IsMobControlled() const { return false; }

        // PlayerRideable — MC positionRider: the type's passenger attachment
        // (EntityTypes .passengerAttachments, the baby dimensions' own),
        // AbstractHorse's rear lean (0, 0.15, -0.7) * standAnimO turned to the
        // body's yaw, less the player's 0.6 vehicle attachment.
        glm::dvec3 PlayerRiderPosition() const override;
        // MC AbstractHorse.positionRider: seated as any rider, and a living
        // rider's body turns with the mount (yBodyRot).
        void PositionRider(Entity& passenger) override;
        // MC AbstractHorse.getDismountLocationForPassenger (the llama is one).
        glm::dvec3 GetDismountLocationForPassenger(const LivingEntity& passenger) const override {
            return EquineDismountLocation(*this, passenger);
        }
        // MC entity events 7 / 6: the taming hearts / smoke.
        void HandleEntityEvent(uint8_t id) override;
        void ClearReferenceTo(const Entity* entity) override {
            GenericAnimal::ClearReferenceTo(entity);
            ClearOwnerReferenceTo(entity);
            if (m_breedPartner == entity) m_breedPartner = nullptr;
        }

        // MC AbstractHorse.fedFood / handleEating — the per-item
        // heal/ageUp/temper table, verbatim.
        UseResult FedFood(LivingEntity& player, ItemStack& held);
        bool HandleEating(LivingEntity& player, const ItemStack& held);

        // MC AbstractHorse.makeMad — rear up and voice the angry sound
        // (server only).
        void MakeMad() override;

        // ── Riding (MC getControllingPassenger / travelRidden hooks) ───────
        // MC getControllingPassenger: the player in the first seat of a
        // SADDLED equine steers it.
        bool CanBeSteeredBy(const RiderControl& rider) const override { (void)rider; return IsSaddled(); }
        // MC getRiddenInput: nothing while planted rearing on the ground (no
        // jump pending, not sliding off a jump's rear); otherwise sideways
        // at half, backwards at a quarter.
        glm::dvec3 GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) override;
        // MC tickRidden: the rider's view turns the mount (pitch halved),
        // and on the simulating side the gallop counter resets while not
        // going forward and a charged jump fires from the ground.
        void TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) override;
        // MC getRiddenSpeed: the MOVEMENT_SPEED attribute.
        float GetRiddenSpeed(const RiderControl& rider) const override;

        // ── PlayerRideableJumping (the jump bar) ───────────────────────────
        void OnPlayerJump(int jumpAmount) override;
        bool CanJump() const override { return IsSaddled(); }
        void HandleStartJump(int jumpScale) override;
        void HandleStopJump() override {}
        // MC PlayerRideableJumping.getPlayerJumpPendingScale.
        static float PlayerJumpPendingScale(int jumpAmount) {
            return jumpAmount >= 90 ? 1.0f : 0.4f + 0.4f * static_cast<float>(jumpAmount) / 90.0f;
        }

        // ── Equipment and inventory (MC AbstractHorse /
        //    AbstractChestedHorse; the system is MountInventory's) ───────────
        // MC AbstractHorse.canUseSlot: the saddle only for a live, grown,
        // tamed one (Horse, SkeletonHorse and ZombieHorse: every slot).
        bool CanUseSlot(EquipmentSlot slot) const override {
            if (slot != EquipmentSlot::SADDLE) return GenericAnimal::CanUseSlot(slot);
            return IsAlive() && !IsBaby() && IsTamedHorse();
        }
        // MC AbstractHorse.canDispenserEquipIntoSlot.
        bool CanDispenserEquipIntoSlot(EquipmentSlot slot) const override {
            return ((slot == EquipmentSlot::BODY || slot == EquipmentSlot::SADDLE) && IsTamedHorse()) ||
                   GenericAnimal::CanDispenserEquipIntoSlot(slot);
        }
        // MC AbstractHorse.getEquipSound: HORSE_SADDLE for the saddle.
        std::string GetEquipSound(EquipmentSlot slot, const ItemStack& stack,
                                  const Equippable& equippable) const override {
            return slot == EquipmentSlot::SADDLE ? std::string(SoundEvents::HORSE_SADDLE)
                                                 : GenericAnimal::GetEquipSound(slot, stack, equippable);
        }
        // MC AbstractHorse.equipBodyArmor: an equippable body piece goes on
        // (setItemSlotAndDropWhenKilled of consumeAndReturn(1)).
        void EquipBodyArmor(LivingEntity& player, ItemStack& held);

        MountInventory*       GetMountInventory() override       { return &m_mountInventory; }
        const MountInventory* GetMountInventory() const override { return &m_mountInventory; }
        // MC AbstractChestedHorse.hasChest (DATA_ID_CHEST) — false for the
        // unchested kinds.
        bool HasChest() const { return m_mountInventory.HasChest(); }
        // MC getInventoryColumns: AbstractChestedHorse's 5 while chested.
        int GetInventoryColumns() const override {
            return m_mountInventory.CanCarryChest() && m_mountInventory.HasChest() ? 5 : 0;
        }
        bool HasCustomInventoryScreen() const override { return true; }
        // MC AbstractHorse.openCustomInventoryScreen: server, nobody else
        // aboard, tamed.
        void OpenCustomInventoryScreen(LivingEntity& player) override;

        // ── Sounds ────────────────────────────────────────────────────────
        // MC AbstractHorse.getEatingSound / getAngrySound: null here; each
        // equine names its own ("" = none).
        virtual const char* GetEatingSound() const { return ""; }
        virtual const char* GetAngrySound() const { return ""; }
        // MC AbstractHorse.getAmbientStandSound — RandomStandGoal's rear.
        const char* GetAmbientStandSound() const { return GetAmbientSound(); }

        // MC AbstractHorse.playStepSound: ridden (and able to gallop) the
        // first five steps clop on wood and every third one after gallops;
        // otherwise the wood clop on the wood sound types, else the hoof
        // step; a snow layer on top wins.
        void PlayStepSound(const glm::ivec3& pos, BlockState state) override;

        // MC AbstractHorse.causeFallDamage: HORSE_LAND past one block, then
        // hurt + the block fall sound — no generic fall thud.
        bool CauseFallDamage(double fallDist, float damageMultiplier) override;

        // ── Renderer inputs (MC AbstractHorseRenderer.extractRenderState) ──
        float GetEatAnim(float partialTick) const;
        float GetStandAnim(float partialTick) const;
        float GetMouthAnim(float partialTick) const;
        bool  IsAnimatingTail() const { return m_tailCounter > 0; }

    protected:
        // MC AbstractHorse.mobInteract itself (below the Horse / chested /
        // zombie fronts): Animal's while ridden or a foal, the tamed sneak's
        // inventory, the held item's own interaction, then doPlayerRide.
        UseResult BaseMobInteract(LivingEntity& player, ItemStack& held);
        // MC AbstractHorse.randomizeAttributes — nothing on the base; each
        // equine rolls its own (MC's static generators, HorseTaming).
        virtual void RandomizeAttributes(JavaRandom& rng) { (void)rng; }
        // MC AbstractHorse.playJumpSound (Donkey/Mule/SkeletonHorse override).
        virtual void PlayJumpSound() { PlaySound(SoundEvents::HORSE_JUMP, 0.4f, 1.0f); }
        // MC AbstractHorse.playGallopSound (Horse adds the breath).
        virtual void PlayGallopSound(const SoundType& type);
        // MC AbstractHorse.executeRidersJump.
        void ExecuteRidersJump(float amount, const glm::dvec3& input);
        // MC LivingEntity.getJumpPower(multiplier): JUMP_STRENGTH * multiplier
        // (no block jump factor in this engine) + the jump-boost power.
        float JumpPower(float multiplier) const;
        // The partner of the breeding in flight (SpawnChildFromBreeding →
        // CreateBaby), null outside it.
        const AbstractHorse* BreedPartner() const { return m_breedPartner; }

        // MC AbstractHorse.canGallop (AbstractChestedHorse: false).
        bool m_canGallop = true;
        // MC AbstractHorse.gallopSoundCounter — mutable: the skeleton horse
        // counts it from its (const) swim sound, as MC's getSwimSound does.
        mutable int m_gallopSoundCounter = 0;

    private:
        void RegisterHorseGoals();

        // MC AbstractHorse.eating — open the mouth and play the chew sound.
        void Eating();
        // MC AbstractHorse.openMouth (server).
        void OpenMouth();

        bool m_eating = false;
        bool m_standing = false;
        bool m_bred = false;
        bool m_openMouth = false;
        int  m_standCounter = 0;
        int  m_eatingCounter = 0;
        int  m_mouthCounter = 0;
        int  m_tailCounter = 0;
        // MC AbstractHorse.sprintCounter — counted up to 300 once started;
        // nothing in 26.3 starts it, kept for the tick's arithmetic.
        int  m_sprintCounter = 0;
        // MC playerJumpPendingScale / allowStandSliding.
        float m_playerJumpPendingScale = 0.0f;
        bool  m_allowStandSliding = false;
        float m_eatAnim = 0.0f, m_eatAnimO = 0.0f;
        float m_standAnim = 0.0f, m_standAnimO = 0.0f;
        float m_mouthAnim = 0.0f, m_mouthAnimO = 0.0f;
        const AbstractHorse* m_breedPartner = nullptr;
        // MC AbstractHorse.inventory; AbstractChestedHorse (the donkey and
        // the mule) carries the chest.
        MountInventory m_mountInventory{GetType() == EntityTypeId::Donkey || GetType() == EntityTypeId::Mule};
    };

    // The concrete equines, one MC class each.
    //
    // MC Horse: the coat (Variant: white, creamy, chestnut, brown, black,
    // gray, dark brown) and the markings (none, white, white field, white
    // dots, black dots), MC's DATA_ID_TYPE_VARIANT int `variant | markings <<
    // 8`, saved as "Variant" and carried on the wire's variant byte as
    // `variant | markings << 4`.
    class Horse : public AbstractHorse {
    public:
        static constexpr int kVariantCount = 7;
        static constexpr int kMarkingsCount = 5;

        explicit Horse(EntityLevel* level)
            : AbstractHorse(EntityTypeId::Horse, level) {}
        // MC Horse.canUseSlot: every slot, tamed or not.
        bool CanUseSlot(EquipmentSlot) const override { return true; }

        // MC getVariant / getMarkings (ByIdMap WRAP) and the raw int.
        int  GetVariantId()  const { return WrapId(m_typeVariant & 0xFF, kVariantCount); }
        int  GetMarkingsId() const { return WrapId((m_typeVariant & 0xFF00) >> 8, kMarkingsCount); }
        int  GetTypeVariant() const { return m_typeVariant; }
        void SetTypeVariant(int v) { m_typeVariant = v; }
        // MC setVariantAndMarkings / setVariant.
        void SetVariantAndMarkings(int variant, int markings) {
            m_typeVariant = (variant & 0xFF) | ((markings << 8) & 0xFF00);
        }
        void SetVariant(int variant) { m_typeVariant = (variant & 0xFF) | (m_typeVariant & ~0xFF); }

        uint8_t GetVariantByte() const override {
            return static_cast<uint8_t>((GetVariantId() & 0x0F) | ((GetMarkingsId() & 0x0F) << 4));
        }
        void SetVariantByte(uint8_t v) override { SetVariantAndMarkings(v & 0x0F, (v >> 4) & 0x0F); }
        // The coat and markings sheets (HorseRenderer LOCATION_BY_VARIANT,
        // HorseMarkingLayer LOCATION_BY_MARKINGS; "" for no markings).
        static const char* VariantTexture(int variant);
        static const char* MarkingsTexture(int markings);

        // MC Horse.canMate: a horse or a donkey, both able to parent.
        bool CanMate(const Animal& other) const override;
        // MC Horse.getBreedOffspring: a mule with a donkey, else a foal with
        // the coat (4/9 this, 4/9 partner, 1/9 random) and markings (2/5,
        // 2/5, 1/5) — then the inherited attributes.
        std::unique_ptr<Animal> CreateBaby() override;
        // MC Horse.finalizeSpawn: the herd's coat (HorseGroupData), random
        // markings.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        const char* GetEatingSound() const override {
            return IsBaby() ? SoundEvents::HORSE_EAT_BABY : SoundEvents::HORSE_EAT;
        }
        const char* GetAngrySound() const override {
            return IsBaby() ? SoundEvents::HORSE_ANGRY_BABY : SoundEvents::HORSE_ANGRY;
        }

    protected:
        // MC Horse.randomizeAttributes: health, speed and jump.
        void RandomizeAttributes(JavaRandom& rng) override;
        // MC Horse.playGallopSound: + a 1-in-10 breath.
        void PlayGallopSound(const SoundType& type) override;

    private:
        static int WrapId(int id, int count) { return ((id % count) + count) % count; }
        int m_typeVariant = 0;
    };

    // MC AbstractChestedHorse's shared rules for the donkey and the mule: no
    // gallop sounds, health-only attribute rolls. (The chest itself is the
    // equipment system's.)
    class Donkey : public AbstractHorse {
    public:
        explicit Donkey(EntityLevel* level)
            : AbstractHorse(EntityTypeId::Donkey, level) { m_canGallop = false; }
        // MC Donkey.canMate: a donkey or a horse, both able to parent.
        bool CanMate(const Animal& other) const override;
        // MC Donkey.getBreedOffspring: a mule with a horse, else a donkey —
        // with the inherited attributes.
        std::unique_ptr<Animal> CreateBaby() override;
        const char* GetEatingSound() const override { return SoundEvents::DONKEY_EAT; }
        const char* GetAngrySound() const override { return SoundEvents::DONKEY_ANGRY; }
    protected:
        void RandomizeAttributes(JavaRandom& rng) override;
        void PlayJumpSound() override { PlaySound(SoundEvents::DONKEY_JUMP, 0.4f, 1.0f); }
    };

    // MC Mule: AbstractHorse.canMate (never breeds); its getBreedOffspring
    // would be a mule.
    class Mule : public AbstractHorse {
    public:
        explicit Mule(EntityLevel* level)
            : AbstractHorse(EntityTypeId::Mule, level) { m_canGallop = false; }
        std::unique_ptr<Animal> CreateBaby() override;
        const char* GetEatingSound() const override { return SoundEvents::MULE_EAT; }
        const char* GetAngrySound() const override { return SoundEvents::MULE_ANGRY; }
    protected:
        void RandomizeAttributes(JavaRandom& rng) override;
        void PlayJumpSound() override { PlaySound(SoundEvents::MULE_JUMP, 0.4f, 1.0f); }
    };

    class SkeletonHorse : public AbstractHorse {
    public:
        explicit SkeletonHorse(EntityLevel* level)
            : AbstractHorse(EntityTypeId::SkeletonHorse, level) {}
        // MC SkeletonHorse.canUseSlot: every slot, tamed or not.
        bool CanUseSlot(EquipmentSlot) const override { return true; }

        // ── The skeleton trap (MC SkeletonHorse.isTrap / SkeletonTrapGoal) ─
        //
        // A thunderstorm's lightning spawns a trap horse (ServerLevel
        // .tickThunder, with setAge(0)). While trapped it carries the
        // SkeletonTrapGoal at priority 1: the moment a live non-spectator
        // player comes within 10 blocks, a visual-only bolt strikes and the
        // horse (tamed, adult) takes an iron-helmeted skeleton rider, joined
        // by three more tamed skeleton horses with riders, all persistent
        // and 60 ticks invulnerable, the riders' weapon and helmet enchanted
        // from MOB_SPAWN_EQUIPMENT. An unsprung trap that is not persistent
        // vanishes after TRAP_MAX_LIFE (18000) ticks. Saved as SkeletonTrap /
        // SkeletonTrapTime.
        static constexpr int kTrapMaxLife = 18000;
        bool IsTrap() const { return m_isTrap; }
        void SetTrap(bool trap);
        int  GetTrapTime() const { return m_trapTime; }
        void SetTrapTime(int ticks) { m_trapTime = ticks; }

        void AiStep() override;
        // MC SkeletonHorse.mobInteract: an untamed skeleton horse ignores
        // every click; a tamed one (the trap's) goes straight to
        // AbstractHorse.mobInteract — no feeding front, so it is ridden,
        // saddled or opened, never fed.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override {
            if (!IsTamedHorse()) return UseResult::Pass;
            return BaseMobInteract(player, held);
        }
        std::unique_ptr<Animal> CreateBaby() override {
            return std::make_unique<SkeletonHorse>(m_level);
        }
        // MC SkeletonHorse.canAgeUp: false — a skeleton foal stays one.
        bool CanAgeUp() const override { return false; }
        // MC SkeletonHorse.getWaterSlowDown: 0.96 — it walks the sea floor
        // (no FloatGoal) nearly unslowed.
        float GetWaterSlowDown() const override { return 0.96f; }
        // MC SkeletonHorse.getSwimSound: on the bottom, the wading step —
        // or, ridden, the same five-then-every-third gallop count as on land
        // (the water gallop); the swim stroke otherwise.
        const char* GetSwimSound() const override;
    protected:
        // MC SkeletonHorse.playSwimSound: 0.3 on the bottom, else capped quiet.
        void PlaySwimSound(float volume) override {
            AbstractHorse::PlaySwimSound(onGround ? 0.3f : std::min(0.1f, volume * 25.0f));
        }
        // MC SkeletonHorse.randomizeAttributes: the jump only.
        void RandomizeAttributes(JavaRandom& rng) override;
        // MC SkeletonHorse.playJumpSound: the water jump while in water.
        void PlayJumpSound() override {
            if (IsInWater()) PlaySound(SoundEvents::SKELETON_HORSE_JUMP_WATER, 0.4f, 1.0f);
            else AbstractHorse::PlayJumpSound();
        }

    private:
        bool  m_isTrap = false;
        int   m_trapTime = 0;
        // The live SkeletonTrapGoal while trapped (owned by the selector).
        Goal* m_trapGoal = nullptr;
        // The goal un-traps the horse from inside its own tick; removing it
        // there would free the running goal, so the removal waits for the
        // next AiStep, before the selector runs again.
        bool  m_trapGoalRemovalPending = false;
    };

    class ZombieHorse : public AbstractHorse {
    public:
        explicit ZombieHorse(EntityLevel* level)
            : AbstractHorse(EntityTypeId::ZombieHorse, level) {}
        // MC ZombieHorse.canUseSlot: every slot, tamed or not.
        bool CanUseSlot(EquipmentSlot) const override { return true; }
        // MC ZombieHorse.removeWhenFarAway: true — the one equine that
        // despawns (it only exists via /summon or a rider).
        bool RemoveWhenFarAway(double) const override { return true; }
        std::unique_ptr<Animal> CreateBaby() override {
            return std::make_unique<ZombieHorse>(m_level);
        }
        // MC ZombieHorse.finalizeSpawn: a NATURAL spawn arrives with a zombie
        // rider holding an iron spear. Defined in Monsters.cpp (it builds a
        // Zombie).
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;
        // MC ZombieHorse.interact: any interaction makes it persistent, then
        // ZombieHorse.mobInteract (feeding red mushrooms, makeMad) →
        // AbstractHorse's.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        // MC ZombieHorse.isMobControlled: a mob (its zombie) rides it.
        bool IsMobControlled() const override;
        // MC ZombieHorse.canBeLeashed: tamed, or not carrying its zombie.
        bool CanBeLeashed() const override { return IsTamedHorse() || !IsMobControlled(); }
        // MC ZombieHorse.canFallInLove / canAgeUp: false.
        bool CanFallInLove() const override { return false; }
        bool CanAgeUp() const override { return false; }
        // EntityTypeTags.BURN_IN_DAYLIGHT; MC ZombieHorse.sunProtectionSlot:
        // what shades it is its BODY armour.
        bool BurnsInDaylight() const override { return true; }
        EquipmentSlot SunProtectionSlot() const override { return EquipmentSlot::BODY; }
        // MC ZombieHorse.chargeSpeedModifier — how much faster the spear
        // charge of its rider runs.
        float ChargeSpeedModifier() const override { return 1.4f; }
        const char* GetEatingSound() const override { return SoundEvents::ZOMBIE_HORSE_EAT; }
        const char* GetAngrySound() const override { return SoundEvents::ZOMBIE_HORSE_ANGRY; }
    protected:
        // MC ZombieHorse.randomizeAttributes: its own jump and speed rolls.
        void RandomizeAttributes(JavaRandom& rng) override;
    };

} // namespace Game

