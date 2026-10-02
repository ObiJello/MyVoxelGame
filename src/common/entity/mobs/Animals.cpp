// File: src/common/entity/mobs/Animals.cpp
#include "common/entity/mobs/Animals.hpp"
#include "server/advancements/CriteriaTriggers.hpp"
#include "common/entity/mobs/FarmSoundVariants.hpp"
#include "common/core/Log.hpp"
#include "common/text/Language.hpp"
#include "common/entity/mobs/Monsters.hpp"   // ZombifiedPiglin (Pig::ThunderHit), Skeleton (the trap)
#include "common/entity/LightningBolt.hpp"
#include "common/world/enchantment/EnchantmentDefinitions.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/RangedGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/goals/FoxGoals.hpp"
#include "common/entity/ai/goals/TurtleGoals.hpp"
#include "common/entity/ai/goals/PandaGoals.hpp"
#include "common/entity/ai/goals/CatGoals.hpp"
#include "common/entity/ai/goals/HorseGoals.hpp"
#include "common/entity/ai/goals/LlamaGoals.hpp"
#include "common/entity/ai/goals/MoveToBlockGoal.hpp"
#include "common/entity/ai/goals/TamableGoals.hpp"
#include "common/entity/ai/goals/ParrotGoals.hpp"
#include "common/entity/ai/goals/TraderGoals.hpp"
#include "common/entity/npc/WanderingTrader.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/projectile/LlamaSpit.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/ai/navigation/FlyingPathNavigation.hpp"
#include "common/entity/ai/navigation/AmphibiousPathNavigation.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/ConsumableBehavior.hpp"
#include "common/data/DataComponents.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/loot/ChestLootTables.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/entity/GeneratedItemAttributes.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/world/pathfinder/Path.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string_view>

namespace Game {

    // ── Cow ────────────────────────────────────────────────────────────────

    void Cow::CreateAttributes(AttributeMap& out) {
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,    10.0);
        out.Register(Attribute::MovementSpeed, 0.2);
    }

    Cow::Cow(EntityLevel* level) : Animal(EntityTypeId::Cow, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Cow::IsFood(uint32_t itemId) const {
        return itemId == Items::Wheat;   // ItemTags.COW_FOOD
    }

    std::unique_ptr<Animal> Cow::CreateBaby() {
        auto baby = std::make_unique<Cow>(m_level);
        // MC Cow.getBreedOffspring: random.nextBoolean() ? own variant :
        // the partner's. A spawn-egg baby (no partner) copies this parent.
        TemperatureVariant v = m_variant;
        if (m_breedPartnerVariant >= 0 && m_level && m_level->Random().NextBool()) {
            v = static_cast<TemperatureVariant>(m_breedPartnerVariant);
        }
        m_breedPartnerVariant = -1;
        baby->SetVariant(v);
        return baby;
    }

    void Cow::SpawnChildFromBreeding(Animal& partner) {
        if (const auto* other = dynamic_cast<const Cow*>(&partner)) {
            m_breedPartnerVariant = static_cast<int8_t>(other->GetVariant());
        }
        Animal::SpawnChildFromBreeding(partner);
        m_breedPartnerVariant = -1;
    }

    void Cow::RegisterGoals() {
        // MC AbstractCow.registerGoals, priority for priority.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 2.0));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.25, false));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.25));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
    }

    // ── Mooshroom ──────────────────────────────────────────────────────────

    UseResult Mooshroom::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC MushroomCow.mobInteract. Skipped branches, each waiting on its
        // system: bowl → (suspicious) mushroom stew needs the filled-result
        // item flow (and AbstractCow's bucket → milk with it); the
        // brown mooshroom's flower feeding needs the stew effects.
        if (held.itemId == Items::Shears && ReadyForShearing()) {
            if (m_level && m_level->IsClientSide()) return UseResult::Success;
            Shear();
            GameEvent(GameEventId::Shear, &player);
            // MC: itemStack.hurtAndBreak(1, player, hand.asEquipmentSlot()).
            HurtAndBreak(held, 1, player, EquipmentSlot::MAINHAND);
            return UseResult::Success;
        }
        return Animal::MobInteract(player, held);
    }

    void Mooshroom::Shear(SoundSource soundSource) {
        if (!m_level) return;
        // MC MushroomCow.shear: convertTo(COW), then the SHEAR_MOOSHROOM
        // loot table — five mushrooms, each popped a metre up as its OWN
        // stack of one, MC's copyWithCount(1) loop. `this` is discarded by
        // the conversion, so everything it needs is copied out first.
        // shear/mooshroom/red or /brown — the variant's own mushroom.
        static const ItemID kRedMushroom =
            RecipeManager::ItemFromSlug("red_mushroom");
        static const ItemID kBrownMushroom =
            RecipeManager::ItemFromSlug("brown_mushroom");
        const ItemID mushroom = m_variant == Variant::Brown ? kBrownMushroom : kRedMushroom;
        EntityLevel* level = m_level;
        const glm::dvec3 dropPos = position + glm::dvec3(0.0, 1.0, 0.0);
        const glm::dvec3 midBody = position + glm::dvec3(0.0, static_cast<double>(GetBbHeight()) * 0.5, 0.0);
        // MC shear: level.playSound(null, this, MOOSHROOM_SHEAR, source, 1, 1).
        level->PlaySoundFromEntity(nullptr, *this, SoundEvents::MOOSHROOM_SHEAR, soundSource, 1.0f, 1.0f);
        if (ConvertTo(std::make_unique<Cow>(level))) {
            // The EXPLOSION poof at the swap (sendParticles, 1, at getY(0.5)).
            level->SendParticles(ParticleOptions(ParticleKind::Explosion), false, false, midBody.x, midBody.y,
                                 midBody.z, 1, 0.0, 0.0, 0.0, 0.0);
            for (int i = 0; i < 5; ++i) {
                level->SpawnItemDrop(dropPos, mushroom, 1);
            }
        }
    }

    void Mooshroom::ThunderHit(Entity* bolt) {
        // MC MushroomCow.thunderHit — NOT super: no fire, no damage.
        if (!m_level || m_level->IsClientSide()) return;
        const Uuid boltUuid = bolt ? bolt->GetUuid() : Uuid{};
        if (bolt && boltUuid == m_lastLightningBoltUuid) return;
        SetVariant(m_variant == Variant::Red ? Variant::Brown : Variant::Red);
        m_lastLightningBoltUuid = boltUuid;
        PlaySound(SoundEvents::MOOSHROOM_CONVERT, 2.0f, 1.0f);
    }

    std::unique_ptr<Animal> Mooshroom::CreateBaby() {
        auto baby = std::make_unique<Mooshroom>(m_level);
        // MC getOffspringVariant. A spawn-egg baby (no partner) copies this
        // parent through the same rule with itself as the mate.
        const Variant mate = m_breedPartnerVariant >= 0 ? static_cast<Variant>(m_breedPartnerVariant) : m_variant;
        Variant v = m_variant;
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            if (m_variant == mate && rng.NextInt(1024) == 0) {
                v = m_variant == Variant::Brown ? Variant::Red : Variant::Brown;
            } else {
                v = rng.NextBool() ? m_variant : mate;
            }
        }
        m_breedPartnerVariant = -1;
        baby->SetVariant(v);
        return baby;
    }

    void Mooshroom::SpawnChildFromBreeding(Animal& partner) {
        if (const auto* other = dynamic_cast<const Mooshroom*>(&partner)) {
            m_breedPartnerVariant = static_cast<int8_t>(other->GetVariant());
        }
        Animal::SpawnChildFromBreeding(partner);
        m_breedPartnerVariant = -1;
    }

    // ── Pig ────────────────────────────────────────────────────────────────

    void Pig::CreateAttributes(AttributeMap& out) {
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,    10.0);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Pig::Pig(EntityLevel* level) : Animal(EntityTypeId::Pig, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Pig::IsFood(uint32_t itemId) const {
        // ItemTags.PIG_FOOD
        return itemId == Items::Carrot || itemId == Items::Potato || itemId == Items::Beetroot;
    }

    std::unique_ptr<Animal> Pig::CreateBaby() {
        auto baby = std::make_unique<Pig>(m_level);
        // MC Pig.getBreedOffspring: random.nextBoolean() ? own variant :
        // the partner's. A spawn-egg baby (no partner) copies this parent.
        TemperatureVariant v = m_variant;
        if (m_breedPartnerVariant >= 0 && m_level && m_level->Random().NextBool()) {
            v = static_cast<TemperatureVariant>(m_breedPartnerVariant);
        }
        m_breedPartnerVariant = -1;
        baby->SetVariant(v);
        return baby;
    }

    void Pig::ThunderHit(Entity* bolt) {
        if (!m_level || m_level->IsClientSide()) return;
        if (m_level->GetDifficulty() == Difficulty::Peaceful || IsRemoved()) {
            Animal::ThunderHit(bolt);
            return;
        }
        // convertTo(ZOMBIFIED_PIGLIN, ConversionParams.single(this,
        // keepEquipment false, preserveCanPickUpLoot true), zp -> {
        // populateDefaultEquipmentSlots(random, difficultyAt(pos));
        // setPersistenceRequired(); }).
        auto zp = std::make_unique<ZombifiedPiglin>(m_level);
        ZombifiedPiglin* piglin = zp.get();
        CopyConversionState(*piglin);
        if (IsBaby()) piglin->SetBaby(true);   // ConversionType.convertCommon
        piglin->PopulateDefaultEquipmentSlots(m_level->Random(),
                                              m_level->GetCurrentDifficultyAt(BlockPosition()));
        piglin->SetPersistenceRequired(true);
        FinishConversion(std::move(zp));
    }

    void Pig::SpawnChildFromBreeding(Animal& partner) {
        if (const auto* other = dynamic_cast<const Pig*>(&partner)) {
            m_breedPartnerVariant = static_cast<int8_t>(other->GetVariant());
        }
        Animal::SpawnChildFromBreeding(partner);
        m_breedPartnerVariant = -1;
    }

    void Pig::RegisterGoals() {
        // MC Pig.registerGoals — no priority 2, and TWO tempt goals at 4: a
        // held carrot-on-a-stick works even though it is not pig food.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.25));
        m_goalSelector.AddGoal(3, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(4, std::make_unique<TemptGoal>(this, 1.2, false,
                                                              Items::CarrotOnAStick));
        m_goalSelector.AddGoal(4, std::make_unique<TemptGoal>(this, 1.2, false));
        m_goalSelector.AddGoal(5, std::make_unique<FollowParentGoal>(this, 1.1));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
    }

    // ── Pig riding (MC Pig: ItemSteerable over ItemBasedSteering) ─────────

    bool Pig::CanBeSteeredBy(const RiderControl& rider) const {
        // MC getControllingPassenger: isSaddled() and the first passenger is
        // a player with player.isHolding(CARROT_ON_A_STICK) — either hand.
        return IsSaddled() &&
               (rider.mainHandItem == Items::CarrotOnAStick || rider.offHandItem == Items::CarrotOnAStick);
    }

    glm::dvec3 Pig::GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) {
        (void)rider; (void)selfInput;
        return glm::dvec3(0.0, 0.0, 1.0);
    }

    void Pig::TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) {
        Animal::TickRidden(rider, riddenInput);
        // setRot(controller.getYRot(), controller.getXRot() * 0.5F) — Entity
        // .setRot keeps each angle `% 360` (Java's remainder: fmod).
        yRot = std::fmod(rider.yRot, 360.0f);
        xRot = std::fmod(rider.xRot * 0.5f, 360.0f);
        yRotO = yBodyRot = yHeadRot = yRot;
        m_steering.TickBoost();
    }

    float Pig::GetRiddenSpeed(const RiderControl& rider) const {
        (void)rider;
        return static_cast<float>(GetAttributeValue(Attribute::MovementSpeed) * 0.225 *
                                  static_cast<double>(m_steering.BoostFactor()));
    }

    bool Pig::Boost() {
        return m_level && m_steering.Boost(m_level->Random());
    }

    void Pig::SetCarriedBlockRaw(uint32_t raw) {
        // Client: MC Pig.onSyncedDataUpdated(DATA_BOOST_TIME) → onSynced.
        if (m_level && m_level->IsClientSide()) {
            m_steering.ApplySyncedBoostTimeTotal(static_cast<int>(raw));
        }
    }

    UseResult Pig::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Pig.mobInteract: no food in hand, saddled, nobody aboard and
        // not sneaking (isSecondaryUseActive) — the player climbs on.
        const bool hasFood = IsFood(held.itemId);
        if (!hasFood && IsSaddled() && !IsVehicle() && !HorseTaming::IsSecondaryUseActive(player)) {
            if (m_level && !m_level->IsClientSide()) m_level->StartPlayerRiding(player, *this);
            return UseResult::Success;
        }
        // super.mobInteract; a PASS reaches the held item's own
        // interactLivingEntity (the saddle's equip) on the server, exactly
        // MC's `isEquippableInSlot(SADDLE) ? interactLivingEntity : PASS`.
        return Animal::MobInteract(player, held);
    }

    // ── Sheep ──────────────────────────────────────────────────────────────

    void Sheep::CreateAttributes(AttributeMap& out) {
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     8.0);
        out.Register(Attribute::MovementSpeed, 0.23);
    }

    Sheep::Sheep(EntityLevel* level) : Animal(EntityTypeId::Sheep, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Sheep::IsFood(uint32_t itemId) const {
        return itemId == Items::Wheat;   // ItemTags.SHEEP_FOOD
    }

    std::unique_ptr<Animal> Sheep::CreateBaby() {
        auto baby = std::make_unique<Sheep>(m_level);
        // MC mixes the parents' dye colours; with only one parent reachable
        // here the child inherits this one's colour, which is MC's result
        // whenever both parents match — the common case.
        baby->SetColor(GetColor());
        return baby;
    }

    void Sheep::SetColor(uint8_t color) {
        m_woolData = static_cast<uint8_t>((m_woolData & 0xF0) | (color & 0x0F));
    }

    void Sheep::SetSheared(bool sheared) {
        if (sheared) m_woolData |= 0x10;
        else         m_woolData = static_cast<uint8_t>(m_woolData & ~0x10);
    }

    namespace {

        // DyeColor ordinals, so the weight tables below read as MC's do.
        enum : uint8_t {
            kWhite = 0, kOrange, kMagenta, kLightBlue, kYellow, kLime, kPink,
            kGray, kLightGray, kCyan, kPurple, kBlue, kBrown, kGreen, kRed,
            kBlack,
        };

        // MC BiomeTags.SPAWNS_WARM_VARIANT_FARM_ANIMALS and
        // SPAWNS_COLD_VARIANT_FARM_ANIMALS, flattened from
        // data/minecraft/tags/worldgen/biome/*.json (both tags nest others —
        // #is_jungle, #is_savanna, #is_nether, #is_badlands, #is_end — and
        // this engine has no biome-tag resolver, so they are expanded here).
        // Regenerate from those files if the tags change.
        constexpr std::string_view kWarmBiomes[] = {
            "badlands", "bamboo_jungle", "basalt_deltas", "crimson_forest",
            "deep_lukewarm_ocean", "desert", "eroded_badlands", "jungle",
            "lukewarm_ocean", "mangrove_swamp", "nether_wastes", "savanna",
            "savanna_plateau", "soul_sand_valley", "sparse_jungle",
            "warm_ocean", "warped_forest", "windswept_savanna",
            "wooded_badlands",
        };
        constexpr std::string_view kColdBiomes[] = {
            "cold_ocean", "deep_cold_ocean", "deep_dark",
            "deep_frozen_ocean", "end_barrens", "end_highlands",
            "end_midlands", "frozen_ocean", "frozen_peaks", "frozen_river",
            "grove", "ice_spikes", "jagged_peaks", "old_growth_pine_taiga",
            "old_growth_spruce_taiga", "small_end_islands", "snowy_beach",
            "snowy_plains", "snowy_slopes", "snowy_taiga", "stony_peaks",
            "taiga", "the_end", "windswept_forest",
            "windswept_gravelly_hills", "windswept_hills",
        };

        bool BiomeIn(std::string_view biome, const std::string_view* list, size_t n) {
            for (size_t i = 0; i < n; ++i) if (list[i] == biome) return true;
            return false;
        }

    } // namespace

    uint8_t Sheep::RandomSpawnColor(JavaRandom& rng, std::string_view biome) {
        // MC SheepColorSpawnRules. Three configurations, each a weighted list
        // out of 100 whose 82-weight entry is itself a 499:1 split between the
        // configuration's own "common" colour and pink. That nesting is why
        // the numbers are out of 50000 here: 82 x 500 keeps the 0.164% pink an
        // exact integer instead of a rounding.
        //
        //   temperate  black 5, gray 5, light_gray 5, brown 3, common WHITE 82
        //   warm       gray 5, light_gray 5, white 5, black 3, common BROWN 82
        //   cold       light_gray 5, gray 5, white 5, brown 3, common BLACK 82
        uint8_t a, b, c, d, common;
        if (BiomeIn(biome, kWarmBiomes, std::size(kWarmBiomes))) {
            a = kGray; b = kLightGray; c = kWhite; d = kBlack; common = kBrown;
        } else if (BiomeIn(biome, kColdBiomes, std::size(kColdBiomes))) {
            a = kLightGray; b = kGray; c = kWhite; d = kBrown; common = kBlack;
        } else {
            a = kBlack; b = kGray; c = kLightGray; d = kBrown; common = kWhite;
        }

        const int roll = rng.NextInt(50000);
        if (roll <  2500) return a;   // 5%
        if (roll <  5000) return b;   // 5%
        if (roll <  7500) return c;   // 5%
        if (roll <  9000) return d;   // 3%
        // The remaining 82% splits 499:1 between the common colour and pink.
        return ((roll - 9000) % 500 == 0) ? kPink : common;
    }

    // ── Farm-animal temperature variants (MC 1.21.5) ──────────────────────

    TemperatureVariant FarmAnimalVariantForBiome(std::string_view biome) {
        if (BiomeIn(biome, kWarmBiomes, std::size(kWarmBiomes))) return TemperatureVariant::Warm;
        if (BiomeIn(biome, kColdBiomes, std::size(kColdBiomes))) return TemperatureVariant::Cold;
        return TemperatureVariant::Temperate;
    }

    namespace {
        // MC Cow/Pig/Chicken.finalizeSpawn: VariantUtils.selectVariantToSpawn
        // (SpawnContext.create(level, blockPosition())) — the biome under the
        // mob at the moment it is finalized, for every spawn reason (natural,
        // spawn egg, /summon), which is why an egg used in a snowy taiga gives
        // a cold cow in vanilla.
        TemperatureVariant SpawnVariantFor(const Mob& mob, EntityLevel* level) {
            if (!level) return TemperatureVariant::Temperate;
            const IBlockAccess* blocks = level->Blocks();
            if (!blocks) return TemperatureVariant::Temperate;
            const glm::ivec3 p = mob.BlockPosition();
            return FarmAnimalVariantForBiome(BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name);
        }
    }

    std::shared_ptr<SpawnGroupData>
    Cow::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        SetVariant(SpawnVariantFor(*this, m_level));
        // CowSoundVariants.pickRandomSoundVariant: registry.getRandom.
        if (m_level) {
            m_soundVariant = static_cast<uint8_t>(m_level->Random().NextInt(FarmSoundVariants::Count(FarmSoundVariants::Mob::Cow)));
        }
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    std::shared_ptr<SpawnGroupData>
    Pig::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        SetVariant(SpawnVariantFor(*this, m_level));
        // PigSoundVariants.pickRandomSoundVariant: registry.getRandom.
        if (m_level) {
            m_soundVariant = static_cast<uint8_t>(m_level->Random().NextInt(FarmSoundVariants::Count(FarmSoundVariants::Mob::Pig)));
        }
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    std::shared_ptr<SpawnGroupData>
    Chicken::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        SetVariant(SpawnVariantFor(*this, m_level));
        // ChickenSoundVariants.pickRandomSoundVariant: registry.getRandom.
        if (m_level) {
            m_soundVariant = static_cast<uint8_t>(m_level->Random().NextInt(FarmSoundVariants::Count(FarmSoundVariants::Mob::Chicken)));
        }
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    // ── The farm animals' sound variants (FarmSoundVariants.hpp) ─────────
    //
    // Pig / Chicken: the variant's adult set, the shared baby set for a
    // baby. Cow: no baby set — a moody calf moos moody. Classic (and every
    // baby's) sounds are the type's own (EntitySounds).

    const char* Pig::GetAmbientSound() const {
        if (IsBaby() || m_soundVariant == 0) return Animal::GetAmbientSound();
        return m_soundVariant == 1 ? SoundEvents::ENTITY_PIG_BIG_AMBIENT : SoundEvents::ENTITY_PIG_MINI_AMBIENT;
    }
    const char* Pig::GetHurtSound(MobDamageSource source) const {
        if (IsBaby() || m_soundVariant == 0) return Animal::GetHurtSound(source);
        return m_soundVariant == 1 ? SoundEvents::ENTITY_PIG_BIG_HURT : SoundEvents::ENTITY_PIG_MINI_HURT;
    }
    const char* Pig::GetDeathSound() const {
        if (IsBaby() || m_soundVariant == 0) return Animal::GetDeathSound();
        return m_soundVariant == 1 ? SoundEvents::ENTITY_PIG_BIG_DEATH : SoundEvents::ENTITY_PIG_MINI_DEATH;
    }
    void Pig::PlayEatingSound() {
        // MC Pig.playEatingSound: makeSound(the set's eat sound).
        if (IsBaby()) { MakeSound(SoundEvents::PIG_EAT_BABY); return; }
        MakeSound(m_soundVariant == 1 ? SoundEvents::ENTITY_PIG_BIG_EAT
                : m_soundVariant == 2 ? SoundEvents::ENTITY_PIG_MINI_EAT : SoundEvents::ENTITY_PIG_EAT);
    }

    const char* Cow::GetAmbientSound() const {
        return m_soundVariant == 1 ? SoundEvents::ENTITY_COW_MOODY_AMBIENT : Animal::GetAmbientSound();
    }
    const char* Cow::GetHurtSound(MobDamageSource source) const {
        return m_soundVariant == 1 ? SoundEvents::ENTITY_COW_MOODY_HURT : Animal::GetHurtSound(source);
    }
    const char* Cow::GetDeathSound() const {
        return m_soundVariant == 1 ? SoundEvents::ENTITY_COW_MOODY_DEATH : Animal::GetDeathSound();
    }
    void Cow::PlayStepSound(const glm::ivec3& pos, BlockState state) {
        // MC Cow.playStepSound: playSound(the set's step sound, 0.15, 1).
        if (m_soundVariant == 1) { PlaySound(SoundEvents::ENTITY_COW_MOODY_STEP, 0.15f, 1.0f); return; }
        Animal::PlayStepSound(pos, state);
    }

    const char* Chicken::GetAmbientSound() const {
        if (IsBaby() || m_soundVariant == 0) return Animal::GetAmbientSound();
        return SoundEvents::ENTITY_CHICKEN_PICKY_AMBIENT;
    }
    const char* Chicken::GetHurtSound(MobDamageSource source) const {
        if (IsBaby() || m_soundVariant == 0) return Animal::GetHurtSound(source);
        return SoundEvents::ENTITY_CHICKEN_PICKY_HURT;
    }
    const char* Chicken::GetDeathSound() const {
        if (IsBaby() || m_soundVariant == 0) return Animal::GetDeathSound();
        return SoundEvents::ENTITY_CHICKEN_PICKY_DEATH;
    }

    std::shared_ptr<SpawnGroupData>
    Sheep::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Sheep.finalizeSpawn — which EntityType.create calls for EVERY
        // spawn reason, natural and spawn-egg and /summon alike. That is why a
        // spawn egg in vanilla can produce a grey or brown sheep (and, once in
        // roughly 600, a pink one) rather than always white. Each sheep rolls
        // its own colour — the group token passes through untouched.
        if (!m_level) return Animal::FinalizeSpawn(reason, std::move(groupData));
        std::string_view biome = "plains";
        if (const IBlockAccess* blocks = m_level->Blocks()) {
            const glm::ivec3 p = BlockPosition();
            biome = BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name;
        }
        SetColor(RandomSpawnColor(m_level->Random(), biome));
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    uint32_t Sheep::WoolItemForColor(uint8_t color) {
        // Wool is a BLOCK in this engine, not a pure item, so the drop goes
        // through ItemRegistry::FromBlock. MC DyeColor order — the block ids
        // are alphabetical in BlockDefs.inc and therefore NOT contiguous in
        // dye order, which is why this is an explicit table.
        static const BlockID kWool[16] = {
            BlockID::WhiteWool,     BlockID::OrangeWool,
            BlockID::MagentaWool,   BlockID::LightBlueWool,
            BlockID::YellowWool,    BlockID::LimeWool,
            BlockID::PinkWool,      BlockID::GrayWool,
            BlockID::LightGrayWool, BlockID::CyanWool,
            BlockID::PurpleWool,    BlockID::BlueWool,
            BlockID::BrownWool,     BlockID::GreenWool,
            BlockID::RedWool,       BlockID::BlackWool,
        };
        return ItemRegistry::FromBlock(kWool[color & 0x0F]);
    }

    bool Sheep::ReadyForShearing() const {
        return IsAlive() && !IsSheared() && !IsBaby();
    }

    void Sheep::Shear(SoundSource soundSource) {
        if (!m_level) return;

        // MC data/minecraft/loot_table/shearing/sheep/<color>.json — one pool,
        // `rolls: uniform(1, 3)`, a single entry of the matching wool.
        //
        // MC drops each roll as its OWN stack of one (`drop.copyWithCount(1)`
        // inside the per-count loop) rather than one stack of three, so the
        // wool scatters instead of landing in a pile. Item entities merge on
        // their own a moment later, which is exactly what vanilla looks like.
        // MC Sheep.shear: level.playSound(null, this, SHEEP_SHEAR, source,
        // 1, 1) — PLAYERS from a player's shears.
        m_level->PlaySoundFromEntity(nullptr, *this, SoundEvents::SHEEP_SHEAR, soundSource, 1.0f, 1.0f);
        JavaRandom& rng = m_level->Random();
        const int rolls = 1 + rng.NextInt(3);
        const uint32_t wool = WoolItemForColor(GetColor());

        // MC spawnAtLocation(level, stack, 1.0F) — a metre above the feet, so
        // the wool pops out of the fleece rather than through the floor.
        const glm::dvec3 dropPos = position + glm::dvec3(0.0, 1.0, 0.0);
        for (int i = 0; i < rolls; ++i) {
            m_level->SpawnItemDrop(dropPos, wool, 1);
        }

        SetSheared(true);
    }

    UseResult Sheep::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Sheep.mobInteract: only shears are handled here; everything else
        // falls through so the held item (dye) gets its turn.
        if (held.itemId != Items::Shears) return Animal::MobInteract(player, held);

        // MC returns CONSUME on the client and for a sheep that is not ready
        // (already sheared, or a lamb) — the click is swallowed either way, so
        // it does not fall through and get eaten by something else.
        if (m_level && m_level->IsClientSide()) return UseResult::Consume;
        if (!ReadyForShearing()) return UseResult::Consume;

        Shear();
        GameEvent(GameEventId::Shear, &player);   // MC: gameEvent(SHEAR, player)
        // MC `itemStack.hurtAndBreak(1, player, hand.asEquipmentSlot())`.
        HurtAndBreak(held, 1, player, EquipmentSlot::MAINHAND);
        return UseResult::Success;
    }

    void Sheep::RegisterGoals() {
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.25));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.1, false));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.1));

        // Kept as a raw pointer so CustomServerAiStep can read the animation
        // counter — MC does exactly this, for the same reason.
        auto eat = std::make_unique<EatBlockGoal>(this);
        m_eatBlockGoal = eat.get();
        m_goalSelector.AddGoal(5, std::move(eat));

        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
    }

    void Sheep::CustomServerAiStep() {
        // Pull the goal's counter onto the entity so the renderer has one place
        // to read from and the value can be synced.
        m_eatAnimationTick = m_eatBlockGoal ? m_eatBlockGoal->GetEatAnimationTick() : 0;
    }

    void Sheep::HandleEntityEvent(uint8_t id) {
        // MC Sheep.handleEntityEvent: `if (id == 10) this.eatAnimationTick = 40;`
        //
        // The full 40, NOT the goal's adjustedTickDelay(40). The server's
        // counter is halved because goals evaluate every other tick and it only
        // has to time the block edit; the client's is the ANIMATION and runs
        // every tick, so the two are deliberately different numbers.
        if (id == 10) {
            m_eatAnimationTick = EatBlockGoal::kEatAnimationTicks;
            return;
        }
        Animal::HandleEntityEvent(id);
    }

    void Sheep::AiStep() {
        // MC Sheep.aiStep. The client drives the graze animation itself from
        // the single event 10 above — nothing streams the counter, so without
        // this countdown the head would dip and stay down forever.
        if (m_level && m_level->IsClientSide()) {
            m_eatAnimationTick = std::max(0, m_eatAnimationTick - 1);
        }
        Animal::AiStep();
    }

    void Sheep::OnEatBlock() {
        SetSheared(false);
        // Grazing accelerates a lamb's growth by 60 seconds — the mechanic that
        // lets a player speed up a flock by keeping them on grass. MC
        // Sheep.ate gates it on canAgeUp: an age-locked lamb grazes for wool
        // only.
        if (CanAgeUp()) AgeUp(60);
    }

    float Sheep::GetHeadEatPositionScale(float partialTick) const {
        // MC Sheep.getHeadEatPositionScale. The head dips over the last 4 ticks
        // and holds; the numbers are the animation curve, not tunables.
        if (m_eatAnimationTick <= 0) return 0.0f;
        if (m_eatAnimationTick >= 4 && m_eatAnimationTick <= EatBlockGoal::kEatAnimationTicks - 4) {
            return 1.0f;
        }
        if (m_eatAnimationTick < 4) {
            return (static_cast<float>(m_eatAnimationTick) - partialTick) / 4.0f;
        }
        return -(static_cast<float>(m_eatAnimationTick - EatBlockGoal::kEatAnimationTicks) - partialTick) / 4.0f;
    }

    float Sheep::GetHeadEatAngleScale(float partialTick) const {
        if (m_eatAnimationTick > 4 &&
            m_eatAnimationTick <= EatBlockGoal::kEatAnimationTicks - 4) {
            // /32, not /4: the sweep runs across the whole 32-tick hold, so a
            // 4 here makes the head waggle eight times too fast.
            const float t = (static_cast<float>(m_eatAnimationTick) - 4.0f - partialTick) / 32.0f;
            return Mth::kPi / 5.0f + 0.21991149f * std::sin(t * 28.7f);
        }
        if (m_eatAnimationTick > 0) return Mth::kPi / 5.0f;
        return xRot * Mth::kDegToRad;
    }

    // ── Chicken ────────────────────────────────────────────────────────────

    void Chicken::CreateAttributes(AttributeMap& out) {
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     4.0);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Chicken::Chicken(EntityLevel* level) : Animal(EntityTypeId::Chicken, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();

        // MC: chickens treat water as free to path through, because they float
        // on it. Without this they refuse to cross a one-block puddle.
        SetPathfindingMalus(PathType::Water, 0.0f);

        if (level) m_eggTime = level->Random().NextInt(6000) + 6000;

        RegisterGoals();
    }

    bool Chicken::IsFood(uint32_t itemId) const {
        // ItemTags.CHICKEN_FOOD — every seed.
        return itemId == Items::WheatSeeds || itemId == Items::MelonSeeds ||
               itemId == Items::PumpkinSeeds || itemId == Items::BeetrootSeeds ||
               itemId == Items::TorchflowerSeeds || itemId == Items::PitcherPod;
    }

    std::unique_ptr<Animal> Chicken::CreateBaby() {
        auto baby = std::make_unique<Chicken>(m_level);
        // MC Chicken.getBreedOffspring: random.nextBoolean() ? own variant :
        // the partner's. A spawn-egg baby (no partner) copies this parent.
        TemperatureVariant v = m_variant;
        if (m_breedPartnerVariant >= 0 && m_level && m_level->Random().NextBool()) {
            v = static_cast<TemperatureVariant>(m_breedPartnerVariant);
        }
        m_breedPartnerVariant = -1;
        baby->SetVariant(v);
        return baby;
    }

    void Chicken::SpawnChildFromBreeding(Animal& partner) {
        if (const auto* other = dynamic_cast<const Chicken*>(&partner)) {
            m_breedPartnerVariant = static_cast<int8_t>(other->GetVariant());
        }
        Animal::SpawnChildFromBreeding(partner);
        m_breedPartnerVariant = -1;
    }

    void Chicken::RegisterGoals() {
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.4));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.1));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
    }

    void Chicken::AiStep() {
        Animal::AiStep();

        // ── Wing flap ──────────────────────────────────────────────────────
        m_oFlap = m_flap;
        m_oFlapSpeed = m_flapSpeed;

        // Flap speed ramps UP in the air and decays on the ground, so a chicken
        // that jumps starts flapping immediately and settles after landing.
        m_flapSpeed += (onGround ? -1.0f : 4.0f) * 0.3f;
        m_flapSpeed = std::clamp(m_flapSpeed, 0.0f, 1.0f);

        if (!onGround && m_flapping < 1.0f) m_flapping = 1.0f;
        m_flapping *= 0.9f;

        // The slow fall: descending motion is damped every tick, which is why
        // chickens never take fall damage. This is a MOVEMENT rule, not a
        // rendering one, so it must run on both sides.
        if (!onGround && velocity.y < 0.0) velocity.y *= 0.6;

        m_flap += m_flapping * 2.0f;

        // ── Egg laying ─────────────────────────────────────────────────────
        // MC Chicken.aiStep gates the lay on !isChickenJockey() — the ride of
        // a baby-zombie jockey never leaves eggs behind.
        if (!IsEffectiveAi() || !IsAlive() || IsBaby() || IsChickenJockey()) return;
        if (--m_eggTime > 0) return;

        m_level->SpawnItemDrop(position, Items::Egg, 1);
        {
            JavaRandom& rng = m_level->Random();
            PlaySound(SoundEvents::CHICKEN_EGG, 1.0f, (rng.NextFloat() - rng.NextFloat()) * 0.2f + 1.0f);
        }
        GameEvent(GameEventId::EntityPlace);   // MC Chicken.aiStep: gameEvent(ENTITY_PLACE)
        m_eggTime = m_level->Random().NextInt(6000) + 6000;
    }

    float Chicken::GetFlap(float partialTick) const {
        return m_oFlap + partialTick * (m_flap - m_oFlap);
    }

    float Chicken::GetFlapSpeed(float partialTick) const {
        return m_oFlapSpeed + partialTick * (m_flapSpeed - m_oFlapSpeed);
    }

    // ── Parrot ─────────────────────────────────────────────────────────────

    const char* Parrot::VariantTexture(Variant variant) {
        // MC ParrotRenderer.getVariantTexture — note GRAY's sheet is "grey".
        switch (variant) {
            case Variant::RedBlue:    return "assets/textures/entity/parrot/parrot_red_blue.png";
            case Variant::Blue:       return "assets/textures/entity/parrot/parrot_blue.png";
            case Variant::Green:      return "assets/textures/entity/parrot/parrot_green.png";
            case Variant::YellowBlue: return "assets/textures/entity/parrot/parrot_yellow_blue.png";
            case Variant::Gray:       return "assets/textures/entity/parrot/parrot_grey.png";
        }
        return "assets/textures/entity/parrot/parrot_red_blue.png";
    }

    void Parrot::CreateAttributes(AttributeMap& out) {
        // MC Parrot.createAttributes on the animal base.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     6.0);
        out.Register(Attribute::FlyingSpeed,   0.4);
        out.Register(Attribute::MovementSpeed, 0.2);
        out.Register(Attribute::AttackDamage,  3.0);
    }

    Parrot::Parrot(EntityLevel* level)
        : Animal(EntityTypeId::Parrot, level), TamableAnimal(this) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();

        // MC's constructor wiring: FlyingMoveControl(10, false) and the fire/
        // cocoa path maluses.
        SetMoveControl(std::make_unique<FlyingMoveControl>(this, 10, false));
        SetPathfindingMalus(PathType::DangerFire, -1.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);
        SetPathfindingMalus(PathType::Cocoa,      -1.0f);

        // MC createNavigation: FlyingPathNavigation with canOpenDoors(false)
        // (the default — no door pathing here anyway) and canFloat(true).
        auto navigation = std::make_unique<FlyingPathNavigation>(this, level);
        navigation->SetCanFloat(true);
        SetNavigation(std::move(navigation));

        RegisterGoals();
    }

    void Parrot::RegisterGoals() {
        // MC Parrot.registerGoals, priority for priority.
        m_goalSelector.AddGoal(0, std::make_unique<TamableAnimalPanicGoal>(
                                      this, this, 1.25));
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(2, std::make_unique<SitWhenOrderedToGoal>(this, this));
        m_goalSelector.AddGoal(2, std::make_unique<FollowOwnerGoal>(
                                      this, this, 1.0, 5.0f, 1.0f));
        m_goalSelector.AddGoal(2, std::make_unique<ParrotWanderGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<LandOnOwnersShoulderGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<FollowMobGoal>(this, 1.0, 3.0f, 7.0f));
    }

    std::shared_ptr<SpawnGroupData>
    Parrot::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Parrot.finalizeSpawn: Util.getRandom(Variant.values(), random)
        // — one nextInt(5) — for EVERY reason, a spawn egg included. (MC
        // also hands super an AgeableMobGroupData(false): no baby roll, and
        // a parrot cannot be a baby anyway.)
        if (m_level) {
            SetVariant(VariantById(m_level->Random().NextInt(kVariantCount)));
        }
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    namespace {

        // MC ItemTags.PARROT_FOOD — the six seeds.
        bool IsParrotFood(uint32_t itemId) {
            return itemId == Items::WheatSeeds || itemId == Items::MelonSeeds ||
                   itemId == Items::PumpkinSeeds ||
                   itemId == Items::BeetrootSeeds ||
                   itemId == Items::TorchflowerSeeds ||
                   itemId == Items::PitcherPod;
        }

        // MC Parrot.MOB_SOUND_MAP. The happy ghast maps to SoundEvents.EMPTY:
        // imitated, but silent ("" here).
        struct ParrotImitation {
            EntityTypeId type;
            const char*  event;
        };
        constexpr ParrotImitation kParrotImitations[] = {
            {EntityTypeId::Blaze,          SoundEvents::PARROT_IMITATE_BLAZE},
            {EntityTypeId::Bogged,         SoundEvents::PARROT_IMITATE_BOGGED},
            {EntityTypeId::Breeze,         SoundEvents::PARROT_IMITATE_BREEZE},
            {EntityTypeId::CamelHusk,      SoundEvents::PARROT_IMITATE_CAMEL_HUSK},
            {EntityTypeId::CaveSpider,     SoundEvents::PARROT_IMITATE_SPIDER},
            {EntityTypeId::Creaking,       SoundEvents::PARROT_IMITATE_CREAKING},
            {EntityTypeId::Creeper,        SoundEvents::PARROT_IMITATE_CREEPER},
            {EntityTypeId::Drowned,        SoundEvents::PARROT_IMITATE_DROWNED},
            {EntityTypeId::ElderGuardian,  SoundEvents::PARROT_IMITATE_ELDER_GUARDIAN},
            {EntityTypeId::EnderDragon,    SoundEvents::PARROT_IMITATE_ENDER_DRAGON},
            {EntityTypeId::Endermite,      SoundEvents::PARROT_IMITATE_ENDERMITE},
            {EntityTypeId::Evoker,         SoundEvents::PARROT_IMITATE_EVOKER},
            {EntityTypeId::Ghast,          SoundEvents::PARROT_IMITATE_GHAST},
            {EntityTypeId::HappyGhast,     ""},
            {EntityTypeId::Guardian,       SoundEvents::PARROT_IMITATE_GUARDIAN},
            {EntityTypeId::Hoglin,         SoundEvents::PARROT_IMITATE_HOGLIN},
            {EntityTypeId::Husk,           SoundEvents::PARROT_IMITATE_HUSK},
            {EntityTypeId::Illusioner,     SoundEvents::PARROT_IMITATE_ILLUSIONER},
            {EntityTypeId::MagmaCube,      SoundEvents::PARROT_IMITATE_MAGMA_CUBE},
            {EntityTypeId::Parched,        SoundEvents::PARROT_IMITATE_PARCHED},
            {EntityTypeId::Phantom,        SoundEvents::PARROT_IMITATE_PHANTOM},
            {EntityTypeId::Piglin,         SoundEvents::PARROT_IMITATE_PIGLIN},
            {EntityTypeId::PiglinBrute,    SoundEvents::PARROT_IMITATE_PIGLIN_BRUTE},
            {EntityTypeId::Pillager,       SoundEvents::PARROT_IMITATE_PILLAGER},
            {EntityTypeId::Ravager,        SoundEvents::PARROT_IMITATE_RAVAGER},
            {EntityTypeId::Shulker,        SoundEvents::PARROT_IMITATE_SHULKER},
            {EntityTypeId::Silverfish,     SoundEvents::PARROT_IMITATE_SILVERFISH},
            {EntityTypeId::Skeleton,       SoundEvents::PARROT_IMITATE_SKELETON},
            {EntityTypeId::Slime,          SoundEvents::PARROT_IMITATE_SLIME},
            {EntityTypeId::Spider,         SoundEvents::PARROT_IMITATE_SPIDER},
            {EntityTypeId::Stray,          SoundEvents::PARROT_IMITATE_STRAY},
            {EntityTypeId::Vex,            SoundEvents::PARROT_IMITATE_VEX},
            {EntityTypeId::Vindicator,     SoundEvents::PARROT_IMITATE_VINDICATOR},
            {EntityTypeId::Warden,         SoundEvents::PARROT_IMITATE_WARDEN},
            {EntityTypeId::Witch,          SoundEvents::PARROT_IMITATE_WITCH},
            {EntityTypeId::Wither,         SoundEvents::PARROT_IMITATE_WITHER},
            {EntityTypeId::WitherSkeleton, SoundEvents::PARROT_IMITATE_WITHER_SKELETON},
            {EntityTypeId::Zoglin,         SoundEvents::PARROT_IMITATE_ZOGLIN},
            {EntityTypeId::Zombie,         SoundEvents::PARROT_IMITATE_ZOMBIE},
            {EntityTypeId::ZombieHorse,    SoundEvents::PARROT_IMITATE_ZOMBIE_HORSE},
            {EntityTypeId::ZombieNautilus, SoundEvents::PARROT_IMITATE_ZOMBIE_NAUTILUS},
            {EntityTypeId::ZombieVillager, SoundEvents::PARROT_IMITATE_ZOMBIE_VILLAGER},
        };

        // MC MOB_SOUND_MAP.containsKey / get; null when the type is absent.
        const char* ImitatedSound(EntityTypeId type) {
            for (const ParrotImitation& i : kParrotImitations) {
                if (i.type == type) return i.event;
            }
            return nullptr;
        }

    } // namespace

    float Parrot::GetPitch(JavaRandom& random) {
        // MC Parrot.getPitch.
        return (random.NextFloat() - random.NextFloat()) * 0.2f + 1.0f;
    }

    const char* Parrot::GetAmbient(EntityLevel& level, JavaRandom& random) {
        // MC Parrot.getAmbient: outside peaceful, 1 call in 1000 imitates a
        // random mob of the map instead of the parrot's own chatter.
        if (level.GetDifficulty() != Difficulty::Peaceful && random.NextInt(1000) == 0) {
            constexpr int count = static_cast<int>(std::size(kParrotImitations));
            return kParrotImitations[random.NextInt(count)].event;
        }
        return SoundEvents::PARROT_AMBIENT;
    }

    bool Parrot::ImitateNearbyMobs(EntityLevel& level, const Entity& entity) {
        // MC Parrot.imitateNearbyMobs: half the time, a random imitable Mob
        // within 20 blocks is voiced at the entity (0.7, getPitch). True
        // once a voice was chosen — even the happy ghast's silent one — so
        // the shoulder parrot skips its own chatter that tick.
        JavaRandom& random = level.Random();
        if (!entity.IsAlive() || entity.IsSilent() || random.NextInt(2) != 0) return false;
        AABB box = entity.GetAABB();
        box.min -= glm::vec3(20.0f);
        box.max += glm::vec3(20.0f);
        std::vector<Entity*> nearby;
        level.GetEntitiesInBox(box, nullptr, nearby);
        std::vector<const Entity*> mobs;
        for (const Entity* e : nearby) {
            // getEntitiesOfClass(Mob.class, ..., NOT_PARROT_PREDICATE): every
            // type in the map is a Mob.
            if (e && ImitatedSound(e->GetType())) mobs.push_back(e);
        }
        if (mobs.empty()) return false;
        const Entity* mob = mobs[static_cast<size_t>(random.NextInt(static_cast<int>(mobs.size())))];
        if (mob->IsSilent()) return false;
        const char* event = ImitatedSound(mob->GetType());
        const float pitch = GetPitch(random);
        if (event && event[0]) {
            level.PlaySound(nullptr, entity.position, event, entity.GetSoundSource(), 0.7f, pitch);
        }
        return true;
    }

    const char* Parrot::GetAmbientSound() const {
        // MC Parrot.getAmbientSound: getAmbient(level, level.getRandom()).
        if (!m_level) return SoundEvents::PARROT_AMBIENT;
        return GetAmbient(*m_level, m_level->Random());
    }

    float Parrot::GetVoicePitch() const {
        // MC Parrot.getVoicePitch — getPitch(this.random), no baby shift.
        return m_level ? GetPitch(m_level->Random()) : 1.0f;
    }

    bool Parrot::IsFlapping() const {
        // MC Parrot.isFlapping.
        return m_flyDist > m_nextFlap;
    }

    void Parrot::OnFlap() {
        // MC Parrot.onFlap.
        PlaySound(SoundEvents::PARROT_FLY, 0.15f, 1.0f);
        m_nextFlap = m_flyDist + m_flapSpeed / 2.0f;
    }

    UseResult Parrot::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Parrot.mobInteract, verbatim shape.
        const bool clientSide = m_level && m_level->IsClientSide();

        if (!IsTame() && IsParrotFood(held.itemId)) {
            UsePlayerItem(held);
            if (!IsSilent() && m_level) {
                JavaRandom& rng = m_level->Random();
                m_level->PlaySound(nullptr, position, SoundEvents::PARROT_EAT, GetSoundSource(),
                                   1.0f, 1.0f + (rng.NextFloat() - rng.NextFloat()) * 0.2f);
            }
            if (!clientSide && m_level) {
                // MC: 1-in-10 — parrots are the hard tame.
                if (m_level->Random().NextInt(10) == 0) {
                    Tame(player);
                    BroadcastTamingResult(true);
                } else {
                    BroadcastTamingResult(false);
                }
            }
            return UseResult::Success;
        }

        if (held.itemId == Items::Cookie) {   // ItemTags.PARROT_POISONOUS_FOOD
            // MC: the cookie is eaten, the parrot gets 900 ticks of poison
            // and is then killed outright (an invulnerable parrot survives
            // unless the feeder is in creative).
            UsePlayerItem(held);
            AddEffect(MobEffectInstance{MobEffectId::Poison, 900});
            if (player.IsCreative() || !IsInvulnerable()) {
                Hurt(MobDamageSource::PlayerAttack,
                     std::numeric_limits<float>::max(), &player);
            }
            return UseResult::Success;
        }

        if (!IsFlying() && IsTame() && IsOwnedBy(player)) {
            if (!clientSide) SetOrderedToSit(!IsOrderedToSit());
            return UseResult::Success;
        }

        return Animal::MobInteract(player, held);
    }

    bool Parrot::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Parrot.hurtServer: a hit parrot stands up before taking it.
        if (m_level && !m_level->IsClientSide()) SetOrderedToSit(false);
        return Animal::Hurt(source, amount, attacker);
    }

    void Parrot::DoPush(Entity& other) {
        // MC Parrot.doPush: `if (!(entity instanceof Player)) super.doPush`.
        if (!other.IsPlayer()) Animal::DoPush(other);
    }

    void Parrot::Tick() {
        // MC ShoulderRidingEntity.tick.
        ++m_rideCooldownCounter;
        Animal::Tick();
    }

    void Parrot::SetRecordPlayingNearby(const glm::ivec3& pos, bool playing) {
        // MC Parrot.setRecordPlayingNearby, widened: the client tells EVERY
        // loaded parrot of a song start (JukeboxSongPlayback), so it keeps the
        // jukebox and dances whenever it is within range; a stop forgets that
        // jukebox only.
        const auto it = std::find(m_jukeboxes.begin(), m_jukeboxes.end(), pos);
        if (playing) {
            if (it == m_jukeboxes.end()) m_jukeboxes.push_back(pos);
        } else if (it != m_jukeboxes.end()) {
            m_jukeboxes.erase(it);
        }
        // The dance itself follows on the next AiStep (UpdatePartyState).
    }

    bool Parrot::IsJukeboxStillPlaying(const glm::ivec3& pos) const {
        // The block is still a jukebox and — client-side — its song still
        // plays (one lookup against the stored position, no scan).
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks || blocks->GetBlock(pos.x, pos.y, pos.z) != BlockID::Jukebox) return false;
        return !m_level->IsClientSide() || m_level->IsJukeboxPlaying(pos);
    }

    bool Parrot::IsWithinJukeboxRange(const glm::ivec3& pos) const {
        // MC BlockPos.closerToCenterThan(position, 3.46).
        const glm::dvec3 d = glm::dvec3(pos) + glm::dvec3(0.5) - position;
        return glm::dot(d, d) < kJukeboxRange * kJukeboxRange;
    }

    void Parrot::UpdatePartyState() {
        // Event-driven. A parrot learns of jukeboxes from each song's start
        // (every loaded parrot is told, SetRecordPlayingNearby) or, once,
        // when it enters this client mid-song (every jukebox the client is
        // playing). It keeps each while that song plays — MC dropped it the
        // moment the parrot strayed out of range, so one short flight ended
        // the dance for good — and dances whenever it is within range of one
        // (the renderer shows PARTY only while perched). Per tick: a block
        // read, a playing lookup and a distance check per known jukebox.
        if (m_level && m_level->IsClientSide() && !m_jukeboxSearched) {
            m_jukeboxSearched = true;
            std::vector<glm::ivec3> playing;
            m_level->GetPlayingJukeboxes(playing);
            for (const glm::ivec3& pos : playing) {
                if (std::find(m_jukeboxes.begin(), m_jukeboxes.end(), pos) == m_jukeboxes.end()) {
                    m_jukeboxes.push_back(pos);
                }
            }
            if (m_jukeboxes.empty()) {
                Log::Info("[Parrot] #%d first-tick check: %zu playing jukeboxes known, stored=none",
                          GetId(), playing.size());
            } else {
                Log::Info("[Parrot] #%d first-tick check: %zu playing jukeboxes known, stored=(%d,%d,%d)%s",
                          GetId(), playing.size(), m_jukeboxes.front().x, m_jukeboxes.front().y,
                          m_jukeboxes.front().z, m_jukeboxes.size() > 1 ? " +more" : "");
            }
        }
        std::erase_if(m_jukeboxes, [this](const glm::ivec3& p) { return !IsJukeboxStillPlaying(p); });
        const bool wasParty = m_partyParrot;
        m_partyParrot = std::any_of(m_jukeboxes.begin(), m_jukeboxes.end(),
                                    [this](const glm::ivec3& p) { return IsWithinJukeboxRange(p); });

        // Diagnostics (client): the dance switching, at most once a second
        // per parrot — a parrot hovering on the range edge cannot flood.
        if (m_partyParrot != wasParty && m_level && m_level->IsClientSide() &&
            tickCount - m_lastDanceLogTick >= 20) {
            m_lastDanceLogTick = tickCount;
            double best = -1.0;
            for (const glm::ivec3& p : m_jukeboxes) {
                const double d = glm::length(glm::dvec3(p) + glm::dvec3(0.5) - position);
                if (best < 0.0 || d < best) best = d;
            }
            Log::Info("[Parrot] #%d dance %s (dist=%.2f, jukeboxes=%zu, flying=%d)", GetId(),
                      m_partyParrot ? "on" : "off", best, m_jukeboxes.size(), IsFlying() ? 1 : 0);
        }
    }

    void Parrot::AiStep() {
        // MC Parrot.aiStep: the party check first.
        UpdatePartyState();

        // The 1-in-400 imitate-nearby-mobs roll. MC rolls it on both sides,
        // but the imitation's playSound(null, ...) is heard only from the
        // server, so the client skips the entity query.
        if (m_level && !m_level->IsClientSide() && m_level->Random().NextInt(400) == 0) {
            ImitateNearbyMobs(*m_level, *this);
        }
        Animal::AiStep();
        CalculateFlapping();
    }

    void Parrot::CalculateFlapping() {
        // MC Parrot.calculateFlapping, constants verbatim.
        m_oFlap = m_flap;
        m_oFlapSpeed = m_flapSpeed;
        m_flapSpeed += ((!onGround && !IsPassenger()) ? 4.0f : -1.0f) * 0.3f;
        m_flapSpeed = std::clamp(m_flapSpeed, 0.0f, 1.0f);

        if (!onGround && m_flapping < 1.0f) m_flapping = 1.0f;
        m_flapping *= 0.9f;

        // The glide: descending motion is damped every tick, which together
        // with the empty checkFallDamage is why parrots never fall hard. A
        // MOVEMENT rule, so it runs on both sides.
        if (!onGround && velocity.y < 0.0) velocity.y *= 0.6;

        m_flap += m_flapping * 2.0f;
    }

    float Parrot::GetFlapAngle(float partialTick) const {
        // MC ParrotRenderer.extractRenderState — the lerp happens BEFORE the
        // sin, exactly as MC computes it.
        const float flap = Mth::Lerp(partialTick, m_oFlap, m_flap);
        const float flapSpeed = Mth::Lerp(partialTick, m_oFlapSpeed, m_flapSpeed);
        return (std::sin(flap) + 1.0f) * flapSpeed;
    }

    // ── Rabbit ─────────────────────────────────────────────────────────────

    namespace {

        constexpr EntityTypeId kRabbitWolfAvoid[] = { EntityTypeId::Wolf };

        // MC's `Monster.class` avoid predicate has no marker type here;
        // MobCategory::Monster in the entity type table is the same set.
        // Built once; AvoidEntityGoal requires the list to outlive the goal.
        const std::vector<EntityTypeId>& MonsterCategoryTypes() {
            static const std::vector<EntityTypeId> kList = [] {
                std::vector<EntityTypeId> list;
                for (int i = 0; i < kEntityTypeCount; ++i) {
                    if (IsMonsterCategory(kEntityTypeTable[i].category)) {
                        list.push_back(static_cast<EntityTypeId>(i));
                    }
                }
                return list;
            }();
            return kList;
        }

        // ItemTags.RABBIT_FOOD's third entry is the dandelion, a block item
        // with no GeneratedItemList constant — resolved once by slug, the same
        // way GenericMobs resolves its food lists.
        ItemID DandelionItem() {
            static const ItemID kId = RecipeManager::ItemFromSlug("dandelion");
            return kId;
        }

    } // namespace

    void Rabbit::CreateAttributes(AttributeMap& out) {
        // MC Rabbit.createAttributes: MAX_HEALTH 3, MOVEMENT_SPEED 0.3,
        // ATTACK_DAMAGE 3 on the animal base.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     3.0);
        out.Register(Attribute::MovementSpeed, 0.3);
        out.Register(Attribute::AttackDamage,  3.0);
    }

    Rabbit::Rabbit(EntityLevel* level) : Animal(EntityTypeId::Rabbit, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor wiring: the rabbit's own jump and move controls,
        // and the navigation parked at speed 0 until a hop is planned.
        m_jumpControl = std::make_unique<RabbitJumpControl>(this);
        SetMoveControl(std::make_unique<RabbitMoveControl>(this));
        SetSpeedModifier(0.0);
        RegisterGoals();
    }

    bool Rabbit::IsFood(uint32_t itemId) const {
        // MC ItemTags.RABBIT_FOOD: carrot, golden carrot, dandelion.
        return itemId == Items::Carrot || itemId == Items::GoldenCarrot ||
               itemId == DandelionItem();
    }

    namespace {
        struct RabbitGroupData : SpawnGroupData {
            Rabbit::Variant variant;
            explicit RabbitGroupData(Rabbit::Variant v) : variant(v) {}
        };

        // MC Rabbit.getRandomRabbitVariant: #spawns_white_rabbits (80%
        // white, else white-splotched), #spawns_gold_rabbits (gold), else
        // brown 50% / salt 40% / black 10%. The two biome tags as the data
        // pack lists them.
        Rabbit::Variant RandomRabbitVariant(const Mob& mob, EntityLevel* level) {
            if (!level) return Rabbit::Variant::Brown;
            const int roll = level->Random().NextInt(100);
            std::string_view biome;
            if (const IBlockAccess* blocks = level->Blocks()) {
                const glm::ivec3 p = mob.BlockPosition();
                biome = BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name;
            }
            if (biome.rfind("minecraft:", 0) == 0) biome.remove_prefix(10);
            static constexpr std::string_view kWhite[] = {"snowy_plains", "ice_spikes", "frozen_ocean", "snowy_taiga",
                                                          "frozen_river", "snowy_beach", "frozen_peaks", "jagged_peaks",
                                                          "snowy_slopes", "grove"};
            if (std::find(std::begin(kWhite), std::end(kWhite), biome) != std::end(kWhite)) {
                return roll < 80 ? Rabbit::Variant::White : Rabbit::Variant::WhiteSplotched;
            }
            if (biome == "desert") return Rabbit::Variant::Gold;
            return roll < 50 ? Rabbit::Variant::Brown : (roll < 90 ? Rabbit::Variant::Salt : Rabbit::Variant::Black);
        }
    }

    const char* Rabbit::VariantName(Variant v) {
        switch (v) {
            case Variant::Brown:          return "brown";
            case Variant::White:          return "white";
            case Variant::Black:          return "black";
            case Variant::WhiteSplotched: return "white_splotched";
            case Variant::Gold:           return "gold";
            case Variant::Salt:           return "salt";
            case Variant::Evil:           return "evil";
        }
        return "brown";
    }

    void Rabbit::SetVariant(Variant v) {
        if (v == Variant::Evil) {
            // MC setVariant(EVIL): armour 8, MeleeAttackGoal(1.4, true) at 4,
            // HurtByTargetGoal(alert others) at 1, NearestAttackableTarget
            // players and wolves at 2, +5 attack damage, the name.
            m_attributes.SetBaseValue(Attribute::Armor, 8.0);
            if (!m_evilGoalsAdded) {
                m_evilGoalsAdded = true;
                m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.4, true));
                m_targetSelector.AddGoal(1, [this] {
                    auto goal = std::make_unique<HurtByTargetGoal>(this);
                    goal->SetAlertOthers();
                    return goal;
                }());
                m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
                static constexpr EntityTypeId kWolf[] = { EntityTypeId::Wolf };
                m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, kWolf, 1, true));
            }
            m_attributes.RemoveModifier(Attribute::AttackDamage, ModifierId::RabbitEvilAttackPower);
            m_attributes.AddModifier(Attribute::AttackDamage,
                                     AttributeModifier{static_cast<uint32_t>(ModifierId::RabbitEvilAttackPower), 5.0,
                                                       AttributeOperation::AddValue});
            if (!HasCustomName()) {
                SetCustomName(Language::GetOrDefault("entity.minecraft.killer_bunny", "The Killer Bunny"));
            }
        } else {
            m_attributes.RemoveModifier(Attribute::AttackDamage, ModifierId::RabbitEvilAttackPower);
        }
        m_variant = v;
    }

    std::shared_ptr<SpawnGroupData>
    Rabbit::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC finalizeSpawn: the biome's roll, or the group's.
        Variant variant = RandomRabbitVariant(*this, m_level);
        if (const auto* group = dynamic_cast<const RabbitGroupData*>(groupData.get())) {
            variant = group->variant;
        } else {
            groupData = std::make_shared<RabbitGroupData>(variant);
        }
        SetVariant(variant);
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    void Rabbit::SpawnChildFromBreeding(Animal& partner) {
        if (const auto* other = dynamic_cast<const Rabbit*>(&partner)) {
            m_breedPartnerVariant = static_cast<int>(other->GetVariant());
        }
        Animal::SpawnChildFromBreeding(partner);
        m_breedPartnerVariant = -1;
    }

    std::unique_ptr<Animal> Rabbit::CreateBaby() {
        // MC getBreedOffspring: the biome's roll; 19 in 20 a parent's
        // variant instead (the partner's or this one's, evenly).
        auto baby = std::make_unique<Rabbit>(m_level);
        Variant variant = RandomRabbitVariant(*this, m_level);
        if (m_level && m_level->Random().NextInt(20) != 0) {
            if (m_breedPartnerVariant >= 0 && m_level->Random().NextBool()) {
                variant = static_cast<Variant>(m_breedPartnerVariant);
            } else {
                variant = m_variant;
            }
        }
        baby->SetVariant(variant);
        return baby;
    }

    SoundSource Rabbit::GetSoundSource() const {
        return m_variant == Variant::Evil ? SoundSource::Hostile : SoundSource::Neutral;
    }

    bool Rabbit::DoHurtTarget(Entity& target) {
        // MC Rabbit.playAttackSound: the killer bunny's bite.
        if (m_variant == Variant::Evil && m_level) {
            JavaRandom& rng = m_level->Random();
            PlaySound(SoundEvents::RABBIT_ATTACK, 1.0f, (rng.NextFloat() - rng.NextFloat()) * 0.2f + 1.0f);
        }
        return Animal::DoHurtTarget(target);
    }

    void Rabbit::RegisterGoals() {
        // MC Rabbit.registerGoals:
        //   1 ClimbOnTopOfPowderSnowGoal — SKIPPED: no powder snow.
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<RabbitPanicGoal>(this, 2.2));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 0.8));
        // MC TemptGoal(1.0, RABBIT_FOOD, canScare=false) — the food predicate
        // is IsFood above.
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(4, std::make_unique<RabbitAvoidEntityGoal>(
                                      this, 8.0f, 2.2, 2.2));
        m_goalSelector.AddGoal(4, std::make_unique<RabbitAvoidEntityGoal>(
                                      this, kRabbitWolfAvoid, 1, 10.0f, 2.2, 2.2));
        const std::vector<EntityTypeId>& monsters = MonsterCategoryTypes();
        m_goalSelector.AddGoal(4, std::make_unique<RabbitAvoidEntityGoal>(
                                      this, monsters.data(),
                                      static_cast<int>(monsters.size()),
                                      4.0f, 2.2, 2.2));
        m_goalSelector.AddGoal(5, std::make_unique<RaidGardenGoal>(this));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.6));
        m_goalSelector.AddGoal(11, std::make_unique<LookAtPlayerGoal>(this, 10.0f));
    }

    float Rabbit::GetJumpPower() const {
        // MC Rabbit.getJumpPower: 0.2 while ambling (speed <= 0.6), 0.3 by
        // default, 0.5 whenever the plan climbs — all fed to
        // super.getJumpPower(base / 0.42F), i.e. scaled against the ordinary
        // jump impulse.
        float baseJumpPower = 0.3f;
        if (m_moveControl->GetSpeedModifier() <= 0.6) {
            baseJumpPower = 0.2f;
        }

        const Path* path = GetNavigation().GetPath();
        if (path && !path->IsDone()) {
            const glm::dvec3 currentPos = path->GetNextEntityPos(*this);
            if (currentPos.y > position.y + 0.5) {
                baseJumpPower = 0.5f;
            }
        }

        if (horizontalCollision ||
            (jumping && m_moveControl->GetWantedY() > position.y + 0.5)) {
            baseJumpPower = 0.5f;
        }

        return Animal::GetJumpPower() * (baseJumpPower / 0.42f);
    }

    void Rabbit::JumpFromGround() {
        // MC Rabbit.jumpFromGround: super, a forward kick when barely moving
        // so a standing hop still travels, then event 1 arms every watcher's
        // animation clock.
        Animal::JumpFromGround();
        const double speedModifier = m_moveControl->GetSpeedModifier();
        if (speedModifier > 0.0) {
            const double current = velocity.x * velocity.x + velocity.z * velocity.z;
            if (current < 0.01) {
                MoveRelative(0.1f, glm::dvec3(0.0, 0.0, 1.0));
            }
        }

        if (m_level && !m_level->IsClientSide()) {
            m_level->BroadcastEntityEvent(*this, 1);
        }
    }

    float Rabbit::GetJumpCompletion(float partialTick) const {
        // MC Rabbit.getJumpCompletion, verbatim.
        return m_jumpDuration == 0
                   ? 0.0f
                   : (static_cast<float>(m_jumpTicks) + partialTick) /
                         static_cast<float>(m_jumpDuration);
    }

    void Rabbit::SetSpeedModifier(double speed) {
        // MC Rabbit.setSpeedModifier — both the navigation AND the move
        // control learn the speed, so the current plan keeps it too.
        GetNavigation().SetSpeedModifier(speed);
        m_moveControl->SetWantedPosition(m_moveControl->GetWantedX(),
                                         m_moveControl->GetWantedY(),
                                         m_moveControl->GetWantedZ(), speed);
    }

    void Rabbit::SetJumping(bool jump) {
        // MC Rabbit.setJumping — super sets the flag; a jump plays RABBIT_JUMP
        // at 0.8x voice pitch. (playAttackSound is the EVIL variant's, which
        // the variant system does not model.)
        jumping = jump;
        if (jump && m_level) {
            JavaRandom& rng = m_level->Random();
            PlaySound(SoundEvents::RABBIT_JUMP, GetSoundVolume(),
                      ((rng.NextFloat() - rng.NextFloat()) * 0.2f + 1.0f) * 0.8f);
        }
    }

    // MC 26.1 Rabbit.JUMP_DURATION — 15 ticks (10 before the rabbit remodel;
    // the hop clip is 0.75 s long, and the clock is what drives it).
    static constexpr int kRabbitJumpDuration = 15;

    void Rabbit::StartJumping() {
        // MC Rabbit.startJumping, verbatim.
        SetJumping(true);
        m_jumpDuration = kRabbitJumpDuration;
        m_jumpTicks = 0;
    }

    void Rabbit::CustomServerAiStep() {
        // MC Rabbit.customServerAiStep — the hop planner, the killer bunny's
        // jump at its target included.
        if (m_jumpDelayTicks > 0) {
            --m_jumpDelayTicks;
        }

        // MC: moreCarrotTicks -= random.nextInt(3), floored at 0 (averages 1
        // per tick) — the decay that re-opens RaidGardenGoal's appetite.
        if (m_moreCarrotTicks > 0 && m_level) {
            m_moreCarrotTicks -= m_level->Random().NextInt(3);
            if (m_moreCarrotTicks < 0) m_moreCarrotTicks = 0;
        }

        if (onGround) {
            if (!m_wasOnGround) {
                SetJumping(false);
                CheckLandingDelay();
            }

            // The killer bunny leaps at a target within four blocks.
            if (m_variant == Variant::Evil && m_jumpDelayTicks == 0) {
                LivingEntity* target = GetTarget();
                if (target && DistanceToSqr(*target) < 16.0) {
                    FacePoint(target->position.x, target->position.z);
                    m_moveControl->SetWantedPosition(target->position.x, target->position.y, target->position.z,
                                                     m_moveControl->GetSpeedModifier());
                    StartJumping();
                    m_wasOnGround = true;
                }
            }

            auto& jumpControl = static_cast<RabbitJumpControl&>(GetJumpControl());
            if (!jumpControl.WantJump()) {
                if (m_moveControl->HasWanted() && m_jumpDelayTicks == 0) {
                    const Path* path = GetNavigation().GetPath();
                    glm::dvec3 pos(m_moveControl->GetWantedX(),
                                   m_moveControl->GetWantedY(),
                                   m_moveControl->GetWantedZ());
                    if (path && !path->IsDone()) {
                        pos = path->GetNextEntityPos(*this);
                    }

                    FacePoint(pos.x, pos.z);
                    StartJumping();
                }
            } else if (!jumpControl.CanJump()) {
                EnableJumpControl();
            }
        }

        m_wasOnGround = onGround;
    }

    void Rabbit::FacePoint(double x, double z) {
        // MC Rabbit.facePoint, verbatim: snap the body toward the hop target.
        yRot = static_cast<float>(std::atan2(z - position.z, x - position.x) *
                                  (180.0 / Mth::kPi)) - 90.0f;
    }

    void Rabbit::EnableJumpControl() {
        static_cast<RabbitJumpControl&>(GetJumpControl()).SetCanJump(true);
    }

    void Rabbit::DisableJumpControl() {
        static_cast<RabbitJumpControl&>(GetJumpControl()).SetCanJump(false);
    }

    void Rabbit::SetLandingDelay() {
        // MC: a fleeing rabbit (speed >= 2.2) barely pauses between hops.
        if (m_moveControl->GetSpeedModifier() < 2.2) {
            m_jumpDelayTicks = 10;
        } else {
            m_jumpDelayTicks = 1;
        }
    }

    void Rabbit::CheckLandingDelay() {
        SetLandingDelay();
        DisableJumpControl();
    }

    void Rabbit::AiStep() {
        // MC Rabbit.aiStep: super, then the animation clock — on BOTH sides,
        // which is what the renderer's jumpCompletion reads.
        Animal::AiStep();
        if (m_jumpTicks != m_jumpDuration) {
            ++m_jumpTicks;
        } else if (m_jumpDuration != 0) {
            m_jumpTicks = 0;
            m_jumpDuration = 0;
            SetJumping(false);
        }
    }

    void Rabbit::HandleEntityEvent(uint8_t id) {
        if (id == 1) {
            // MC Rabbit.handleEntityEvent(1): spawnSprintParticle (the
            // dust kicked up off the block underfoot) + start the jump.
            SpawnSprintParticle();
            m_jumpDuration = kRabbitJumpDuration;
            m_jumpTicks = 0;
        } else {
            Animal::HandleEntityEvent(id);
        }
    }

    void Rabbit::SetupAnimationStates() {
        // MC 26.1 Rabbit.setupAnimationStates + shouldPlayIdleAnimation: no
        // head tilt on a lead (Rabbit.setLeashData stops the clip too).
        if (IsLeashed()) Anim(MobAnim::IdleHeadTilt).Stop();
        if (m_idleAnimationTimeout <= 0 && !IsLeashed() && !IsNoAi()) {
            m_idleAnimationTimeout = m_level->Random().NextInt(40) + 180;
            Anim(MobAnim::IdleHeadTilt).Start(tickCount);
        } else if (m_jumpTicks > 0) {
            Anim(MobAnim::Hop).StartIfStopped(tickCount);
            Anim(MobAnim::IdleHeadTilt).Stop();
        } else {
            --m_idleAnimationTimeout;
            Anim(MobAnim::Hop).Stop();
        }
    }

    // ── PolarBear ──────────────────────────────────────────────────────────

    namespace {

        constexpr EntityTypeId kPolarBearFoxTargets[] = { EntityTypeId::Fox };

        // MC gives PolarBear's PanicGoal a damage-type predicate: a CUB
        // panics at PANIC_CAUSES (any damage), an ADULT only at
        // PANIC_ENVIRONMENTAL_CAUSES (fire and friends — never an attack; an
        // attacked adult fights instead). Mapped onto this port's damage
        // model: "environmental" is damage with no attacking mob behind it.
        class PolarBearPanicGoal : public PanicGoal {
        public:
            PolarBearPanicGoal(PolarBear* bear, double speedModifier)
                : PanicGoal(bear, speedModifier), m_bear(bear) {}

            // Keeps the base name so PathfinderMob::IsPanicking still sees it.
            const char* Name() const override { return "PanicGoal"; }

        protected:
            bool ShouldPanic() const override {
                if (!m_bear->HasLastDamageSource()) return false;
                if (m_bear->IsBaby()) return true;
                return m_bear->GetLastHurtByMob() == nullptr;
            }

        private:
            PolarBear* m_bear;
        };

    } // namespace

    void PolarBear::CreateAttributes(AttributeMap& out) {
        // MC PolarBear.createAttributes: MAX_HEALTH 30, FOLLOW_RANGE 20,
        // MOVEMENT_SPEED 0.25, ATTACK_DAMAGE 6 on the animal base.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,    30.0);
        out.Register(Attribute::FollowRange,  20.0);
        out.Register(Attribute::MovementSpeed, 0.25);
        out.Register(Attribute::AttackDamage,  6.0);
    }

    PolarBear::PolarBear(EntityLevel* level)
        : Animal(EntityTypeId::PolarBear, level), NeutralMob(this) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void PolarBear::StartPersistentAngerTimer() {
        // MC PERSISTENT_ANGER_TIME = TimeUtil.rangeOfSeconds(20, 39).
        if (!m_level) return;
        SetTimeToRemainAngry(400 + m_level->Random().NextInt(381));
    }

    void PolarBear::AiStep() {
        // MC PolarBear.aiStep ends with updatePersistentAnger(level, true)
        // on the server (the stand-animation lerp lives in Tick here, where
        // the rest of the client lerps run).
        Animal::AiStep();
        if (m_level && !m_level->IsClientSide()) {
            UpdatePersistentAnger(/*stayAngryIfTargetPresent=*/true);
        }
    }

    std::unique_ptr<Animal> PolarBear::CreateBaby() {
        return std::make_unique<PolarBear>(m_level);
    }

    void PolarBear::RegisterGoals() {
        // MC PolarBear.registerGoals (super.registerGoals() is empty). Note
        // MC registers NO BreedGoal and isFood is false — polar bears never
        // breed; cubs come only from spawning.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PolarBearMeleeAttackGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PolarBearPanicGoal>(this, 2.0));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.25));
        m_goalSelector.AddGoal(5, std::make_unique<RandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<PolarBearHurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<PolarBearAttackPlayersGoal>(this));
        // MC 3: NearestAttackableTargetGoal(Player, 10, true, false,
        // this::isAngryAt) — the grudge hunt, armed only by being hit.
        auto angryAt = std::make_unique<NearestAttackableTargetGoal>(
            this, /*mustSee=*/true, /*mustReach=*/false, /*randomInterval=*/10);
        angryAt->SetSelector([](Mob& mob, const LivingEntity& target) {
            return static_cast<PolarBear&>(mob).IsAngryAt(target);
        });
        m_targetSelector.AddGoal(3, std::move(angryAt));
        m_targetSelector.AddGoal(4, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kPolarBearFoxTargets, 1,
                                        /*mustSee=*/true, /*mustReach=*/true,
                                        /*randomInterval=*/10));
        m_targetSelector.AddGoal(5, std::make_unique<ResetUniversalAngerTargetGoal>(
                                        this, /*alertOthersOfSameType=*/false));
    }

    void PolarBear::PlayWarningSound() {
        // MC PolarBear.playWarningSound — one growl per 40 ticks.
        if (m_warningSoundTicks <= 0) {
            MakeSound(SoundEvents::POLAR_BEAR_WARNING);
            m_warningSoundTicks = 40;
        }
    }

    void PolarBear::Tick() {
        // MC PolarBear.tick: super, then the client-side stand-animation ramp
        // (one tick per step, 0..6 — STAND_ANIMATION_TICKS). MC's
        // refreshDimensions() on change is implicit here: the AABB reads
        // GetBbHeight live. (updatePersistentAnger needs the anger system.)
        Animal::Tick();
        if (m_level && m_level->IsClientSide()) {
            m_clientSideStandAnimationO = m_clientSideStandAnimation;
            if (IsStanding()) {
                m_clientSideStandAnimation =
                    std::clamp(m_clientSideStandAnimation + 1.0f, 0.0f, 6.0f);
            } else {
                m_clientSideStandAnimation =
                    std::clamp(m_clientSideStandAnimation - 1.0f, 0.0f, 6.0f);
            }
        }

        if (m_warningSoundTicks > 0) {
            --m_warningSoundTicks;
        }
    }

    float PolarBear::BaseBbHeight() const {
        // MC PolarBear.getDefaultDimensions: while the stand animation runs,
        // the hitbox is 1 + (anim / 6) times taller. Client-only in effect —
        // the server's animation stays at 0, exactly as in MC.
        const float base = Animal::BaseBbHeight();
        if (m_clientSideStandAnimation > 0.0f) {
            const float standFactor = m_clientSideStandAnimation / 6.0f;
            return base * (1.0f + standFactor);
        }
        return base;
    }

    float PolarBear::GetStandingAnimationScale(float partialTick) const {
        // MC PolarBear.getStandingAnimationScale: the RAW 0..1 lerp —
        // PolarBearModel is what squares it (6.0F = STAND_ANIMATION_TICKS).
        return Mth::Lerp(partialTick, m_clientSideStandAnimationO,
                         m_clientSideStandAnimation) / 6.0f;
    }

    // ── Wolf ───────────────────────────────────────────────────────────────

    namespace {
        // MC Wolf.registerGoals target 7: NearestAttackableTargetGoal
        // (AbstractSkeleton.class, false) — the abstract class flattened to
        // its members, matching the generated def's own flattening.
        constexpr EntityTypeId kWolfSkeletonTargets[] = {
            EntityTypeId::Skeleton, EntityTypeId::Stray,
            EntityTypeId::WitherSkeleton, EntityTypeId::Bogged,
            EntityTypeId::Parched,
        };

        // MC Wolf.PREY_SELECTOR: sheep, rabbit, fox.
        constexpr EntityTypeId kWolfPreyTypes[] = {
            EntityTypeId::Sheep, EntityTypeId::Rabbit, EntityTypeId::Fox,
        };

        // MC `Llama.class` for WolfAvoidEntityGoal — TraderLlama extends it.
        constexpr EntityTypeId kWolfAvoidLlamaTypes[] = {
            EntityTypeId::Llama, EntityTypeId::TraderLlama,
        };

        // MC Wolf.WolfAvoidEntityGoal(this, Llama.class, 24, 1.5, 1.5): a
        // WILD wolf backs off from a llama whose strength beats a nextInt(5)
        // roll, and drops its target while it does (start and every tick).
        class WolfAvoidEntityGoal : public AvoidEntityGoal {
        public:
            explicit WolfAvoidEntityGoal(Wolf* wolf)
                : AvoidEntityGoal(wolf, kWolfAvoidLlamaTypes,
                                  static_cast<int>(std::size(kWolfAvoidLlamaTypes)),
                                  24.0f, 1.5, 1.5),
                  m_wolf(wolf) {}

            bool CanUse() override {
                if (!AvoidEntityGoal::CanUse()) return false;
                const auto* llama = dynamic_cast<const Llama*>(ToAvoid());
                if (!llama) return false;
                if (m_wolf->IsTame()) return false;
                EntityLevel* level = m_wolf->Level();
                return level && llama->GetStrength() >= level->Random().NextInt(5);
            }

            void Start() override {
                m_wolf->SetTarget(nullptr);
                AvoidEntityGoal::Start();
            }

            void Tick() override {
                m_wolf->SetTarget(nullptr);
                AvoidEntityGoal::Tick();
            }

            const char* Name() const override { return "WolfAvoidEntityGoal"; }

        private:
            Wolf* m_wolf;
        };

        // MC DamageTypeTags.BYPASSES_WOLF_ARMOR over this engine's sources:
        // #bypasses_invulnerability (the void), cramming, drown, magic /
        // indirect_magic, thorns and wither. (dry_out, freeze, in_wall,
        // outside_border and starve have no source here.)
        bool BypassesWolfArmor(MobDamageSource source) {
            switch (source) {
                case MobDamageSource::Void:
                case MobDamageSource::Cramming:
                case MobDamageSource::Drown:
                case MobDamageSource::Magic:
                case MobDamageSource::Wither:
                case MobDamageSource::Thorns:
                    return true;
                default:
                    return false;
            }
        }

        // Vec3.xRot / yRot, as spawnItemParticles uses them.
        glm::dvec3 XRot(const glm::dvec3& v, float radians) {
            const double c = std::cos(radians), s = std::sin(radians);
            return { v.x, v.y * c + v.z * s, v.z * c - v.y * s };
        }
        glm::dvec3 YRot(const glm::dvec3& v, float radians) {
            const double c = std::cos(radians), s = std::sin(radians);
            return { v.x * c + v.z * s, v.y, v.z * c - v.x * s };
        }

        // Anim byte bits (see the class note in Animals.hpp).
        constexpr uint8_t kWolfAnimInterested = 0x04;
        constexpr uint8_t kWolfAnimAngry      = 0x08;
    } // namespace

    Wolf::Wolf(EntityLevel* level)
        : GenericAnimal(EntityTypeId::Wolf, level), NeutralMob(this),
          TamableAnimal(this) {
        // MC Wolf(type, level): setTame(false, false) — the untamed default
        // this mixin already holds — and powder snow is off limits both in
        // and on top of it.
        SetPathfindingMalus(PathType::PowderSnow, -1.0f);
        SetPathfindingMalus(PathType::DangerPowderSnow, -1.0f);
        // The GenericAnimal constructor registered the def-driven animal set
        // (tempt, follow-parent, an any-damage panic, the flattened target
        // list). MC's wolf has none of that shape; its registerGoals is the
        // whole table, so both selectors start from nothing.
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        RegisterWolfGoals();
    }

    void Wolf::RegisterWolfGoals() {
        // MC Wolf.registerGoals, priority for priority.
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<TamableAnimalPanicGoal>(
                                      this, this, 1.5,
                                      TamableAnimalPanicGoal::Causes::EnvironmentalOnly));
        m_goalSelector.AddGoal(2, std::make_unique<SitWhenOrderedToGoal>(this, this));
        m_goalSelector.AddGoal(3, std::make_unique<WolfAvoidEntityGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<LeapAtTargetGoal>(this, 0.4f));
        m_goalSelector.AddGoal(5, std::make_unique<MeleeAttackGoal>(this, 1.0, true));
        m_goalSelector.AddGoal(6, std::make_unique<FollowOwnerGoal>(
                                      this, this, 1.0, 10.0f, 2.0f));
        m_goalSelector.AddGoal(7, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(8, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(9, std::make_unique<BegGoal>(this, 8.0f));
        m_goalSelector.AddGoal(10, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(10, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<OwnerHurtByTargetGoal>(this, this));
        m_targetSelector.AddGoal(2, std::make_unique<OwnerHurtTargetGoal>(this, this));
        // MC 3: HurtByTargetGoal(this).setAlertOthers() — the whole pack.
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(3, std::move(hurtBy));
        // MC 4: NearestAttackableTargetGoal(Player, 10, true, false,
        // this::isAngryAt) — the neutral-until-provoked player hunt.
        auto angryAt = std::make_unique<NearestAttackableTargetGoal>(
            this, /*mustSee=*/true, /*mustReach=*/false, /*randomInterval=*/10);
        angryAt->SetSelector([](Mob& mob, const LivingEntity& target) {
            return static_cast<Wolf&>(mob).IsAngryAt(target);
        });
        m_targetSelector.AddGoal(4, std::move(angryAt));
        // MC 5: NonTameRandomTargetGoal(Animal, false, PREY_SELECTOR) — the
        // wild hunt for sheep/rabbit/fox; 6: beached baby turtles. Both
        // stand down for a tamed wolf (the goal's own gate).
        m_targetSelector.AddGoal(5, std::make_unique<NonTameRandomTargetGoal>(
                                        this, kWolfPreyTypes,
                                        static_cast<int>(std::size(kWolfPreyTypes)),
                                        /*mustSee=*/false,
                                        /*babyOnLandOnly=*/false));
        m_targetSelector.AddGoal(6, std::make_unique<NonTameRandomTargetGoal>(
                                        this, EntityTypeId::Turtle,
                                        /*mustSee=*/false,
                                        /*babyOnLandOnly=*/true));
        // MC 7: skeletons on sight, no line-of-sight requirement.
        m_targetSelector.AddGoal(7, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kWolfSkeletonTargets,
                                        static_cast<int>(std::size(kWolfSkeletonTargets)),
                                        /*mustSee=*/false));
        m_targetSelector.AddGoal(8, std::make_unique<ResetUniversalAngerTargetGoal>(
                                        this, /*alertOthersOfSameType=*/true));
    }

    void Wolf::StartPersistentAngerTimer() {
        // MC PERSISTENT_ANGER_TIME = TimeUtil.rangeOfSeconds(20, 39).
        if (!m_level) return;
        SetTimeToRemainAngry(400 + m_level->Random().NextInt(381));
    }

    bool Wolf::IsAngryState() const {
        if (m_level && m_level->IsClientSide()) return m_clientAngry;
        return IsAngry();
    }

    // ── Wire bytes ──────────────────────────────────────────────────────────

    uint8_t Wolf::GetAnimStateByte() const {
        return static_cast<uint8_t>(GetTamableAnimByte() |
                                    (m_interested ? kWolfAnimInterested : 0) |
                                    (IsAngry() ? kWolfAnimAngry : 0) |
                                    (m_collarColor << 4));
    }

    void Wolf::SetAnimStateByte(uint8_t v) {
        SetTamableAnimByte(v);
        m_interested  = (v & kWolfAnimInterested) != 0;
        m_clientAngry = (v & kWolfAnimAngry) != 0;
        m_collarColor = static_cast<uint8_t>((v >> 4) & 0x0F);
    }

    uint8_t Wolf::GetVariantByte() const {
        return static_cast<uint8_t>(static_cast<uint8_t>(m_variant) |
                                    (static_cast<uint8_t>(m_soundVariant) << 4));
    }

    void Wolf::SetVariantByte(uint8_t v) {
        const uint8_t coat = v & 0x0F;
        const uint8_t sound = (v >> 4) & 0x07;
        m_variant = coat < WolfVariants::kCount ? static_cast<WolfVariants::Variant>(coat)
                                                : WolfVariants::kDefault;
        m_soundVariant = sound < WolfSoundVariants::kCount
                             ? static_cast<WolfSoundVariants::SoundVariant>(sound)
                             : WolfSoundVariants::SoundVariant::Classic;
    }

    std::string Wolf::GetTexturePath() const {
        // MC Wolf.getTexture: tame → the tame sheet, else angry → the angry
        // sheet, else the wild sheet.
        return WolfVariants::TexturePath(m_variant, IsTame(), IsAngryState());
    }

    // ── Tick ────────────────────────────────────────────────────────────────

    void Wolf::AiStep() {
        // A beg tilt restored from the save (SetRenderPhase) has no BegGoal
        // behind it: drop it on the first server step after the world runs
        // again, BEFORE the goals — a player still offering a bone restarts
        // the goal (and the tilt) the same step; otherwise it would never end.
        if (m_restoredInterest && m_level && !m_level->IsClientSide()) m_interested = false;
        m_restoredInterest = false;
        GenericAnimal::AiStep();
        if (!m_level || m_level->IsClientSide()) return;

        // MC Wolf.aiStep: a wet wolf standing still on the ground starts to
        // shake, and tells every watcher (entity event 8).
        if (m_isWet && !m_isShaking && GetNavigation().IsDone() && onGround) {
            m_isShaking = true;
            m_shakeAnim = 0.0f;
            m_shakeAnimO = 0.0f;
            m_level->BroadcastEntityEvent(*this, 8);
        }

        if (m_healLoadedAnger) HealLoadedTameAnger();
        UpdatePersistentAnger(/*stayAngryIfTargetPresent=*/true);
    }

    void Wolf::HealLoadedTameAnger() {
        // A tamed wolf in MC only ever targets through the owner goals,
        // HurtByTargetGoal, the anger-gated player hunt and the skeleton
        // goal; sheep/rabbit/fox (and beached baby turtles) are
        // NonTameRandomTargetGoal's alone. A tamed wolf that starts life here
        // holding such a grudge was saved in the broken state.
        // (An owner-ordered hunt of a sheep saved mid-fight is dropped too —
        // the owner hits it again and the wolf rejoins.)
        const auto isWildPrey = [](const Entity& e) {
            return IsPrey(e.GetType()) || e.GetType() == EntityTypeId::Turtle;
        };

        if (!IsTame()) { m_healLoadedAnger = false; return; }

        // Taken on the first step, before UpdatePersistentAnger can turn a
        // target this session's goals chose into a grudge: what is held here
        // is the saved one. (The target itself is not saved — only the
        // grudge — so a freshly chosen target is this session's business.)
        if (!m_healAngerUuidTaken) {
            m_healAngerUuidTaken = true;
            m_healAngerUuid = AngryAtRef().GetUuid();
        }

        // The grudge ended, or was replaced by one this session started —
        // nothing left that came from the save.
        if (AngryAtRef().Empty() || AngryAtRef().GetUuid() != m_healAngerUuid) {
            m_healLoadedAnger = false;
            return;
        }

        // Resolve the saved grudge; an entity not loaded yet (the sheep's
        // chunk still streaming in) is retried next step. One that never
        // loads simply runs out with the anger window.
        LivingEntity* angryAt = GetPersistentAngerTarget();
        if (!angryAt) return;
        if (isWildPrey(*angryAt)) {
            // MC stopBeingAngry: grudge, target, last attacker and the anger
            // window all go — IsAngry() turns false, so the anim byte's
            // angry bit (client tail/texture) clears on the next sync.
            StopBeingAngry();
        }
        m_healLoadedAnger = false;
    }

    void Wolf::Tick() {
        GenericAnimal::Tick();
        if (!IsAlive() || !m_level) return;

        // MC Wolf.tick: the beg head-tilt eases 40% of the way to its target.
        m_interestedAngleO = m_interestedAngle;
        if (m_interested) {
            m_interestedAngle += (1.0f - m_interestedAngle) * 0.4f;
        } else {
            m_interestedAngle += (0.0f - m_interestedAngle) * 0.4f;
        }

        // MC isInWaterOrRain: water or rain soaks the wolf (and interrupts a
        // shake); out of both, a wet wolf shakes itself dry.
        if (IsInWaterOrRain()) {
            m_isWet = true;
            if (m_isShaking && !m_level->IsClientSide()) {
                m_level->BroadcastEntityEvent(*this, 56);
                CancelShake();
            }
        } else if ((m_isWet || m_isShaking) && m_isShaking) {
            JavaRandom& rng = m_level->Random();
            if (m_shakeAnim == 0.0f) {
                // MC playSound(WOLF_SHAKE, getSoundVolume(), ...) — the
                // server's copy is the one everyone hears; the RNG draws
                // happen on both sides, as in MC.
                const float pitch = (rng.NextFloat() - rng.NextFloat()) * 0.2f + 1.0f;
                PlaySound(SoundEvents::WOLF_SHAKE, GetSoundVolume(), pitch);
                GameEvent(GameEventId::EntityAction);   // MC Wolf.aiStep's shake
            }

            m_shakeAnimO = m_shakeAnim;
            m_shakeAnim += 0.05f;
            if (m_shakeAnimO >= 2.0f) {
                m_isWet = false;
                m_isShaking = false;
                m_shakeAnimO = 0.0f;
                m_shakeAnim = 0.0f;
            }

            if (m_shakeAnim > 0.4f) {
                // The water flying off the coat: SPLASH at back height, as
                // many as sin((shake - 0.4) * PI) * 7, carried along with
                // the wolf. The loop and its draws run on both sides, as in
                // MC; addParticle is the client's alone (the server level
                // ignores it).
                const float yt = static_cast<float>(position.y);
                const int shakeCount = static_cast<int>(
                    std::sin((m_shakeAnim - 0.4f) * 3.1415927f) * 7.0f);
                const float halfWidth = GetBbWidth() * 0.5f;
                for (int i = 0; i < shakeCount; ++i) {
                    const float xo = (rng.NextFloat() * 2.0f - 1.0f) * halfWidth;
                    const float zo = (rng.NextFloat() * 2.0f - 1.0f) * halfWidth;
                    m_level->AddParticle(ParticleKind::Splash,
                                         position.x + xo, static_cast<double>(yt + 0.8f),
                                         position.z + zo,
                                         velocity.x, velocity.y, velocity.z);
                }
            }
        }
    }

    void Wolf::CancelShake() {
        m_isShaking = false;
        m_shakeAnim = 0.0f;
        m_shakeAnimO = 0.0f;
    }

    void Wolf::Die(MobDamageSource source, Entity* attacker) {
        // MC Wolf.die: the shake state is reset before the death proper.
        m_isWet = false;
        m_isShaking = false;
        m_shakeAnimO = 0.0f;
        m_shakeAnim = 0.0f;
        GenericAnimal::Die(source, attacker);
    }

    float Wolf::GetWetShade(float partialTick) const {
        // MC Wolf.getWetShade: 1 when dry; 0.75 darkening back toward 1 as
        // the shake runs out.
        if (!m_isWet) return 1.0f;
        return std::min(0.75f + Mth::Lerp(partialTick, m_shakeAnimO, m_shakeAnim) / 2.0f * 0.25f,
                        1.0f);
    }

    float Wolf::GetShakeAnim(float partialTick) const {
        return Mth::Lerp(partialTick, m_shakeAnimO, m_shakeAnim);
    }

    float Wolf::GetHeadRollAngle(float partialTick) const {
        // MC Wolf.getHeadRollAngle: lerp(interestedAngleO..interestedAngle)
        // * 0.15π.
        return Mth::Lerp(partialTick, m_interestedAngleO, m_interestedAngle) *
               0.15f * 3.1415927f;
    }

    float Wolf::GetTailAngle() const {
        // MC Wolf.getTailAngle, verbatim.
        if (IsAngryState()) return 1.5393804f;
        if (IsTame()) {
            const float maxHealth = GetMaxHealth();
            const float damageRatio = (maxHealth - GetHealth()) / maxHealth;
            return (0.55f - damageRatio * 0.4f) * 3.1415927f;
        }
        return 0.62831855f;   // DEFAULT_TAIL_ANGLE
    }

    void Wolf::HandleEntityEvent(uint8_t id) {
        if (id == 8) {
            // MC: begin the shake on every watcher.
            m_isShaking = true;
            m_shakeAnim = 0.0f;
            m_shakeAnimO = 0.0f;
            return;
        }
        if (id == 56) {
            CancelShake();
            return;
        }
        if (HandleTamableEntityEvent(id)) return;
        if (id == EntityEventForEquipmentBreak(EquipmentSlot::BODY)) {
            // MC LivingEntity.handleEntityEvent(65) → breakItem(BODY item):
            // its BREAK_SOUND (item.wolf_armor.break) played locally, and
            // spawnItemParticles(stack, 5) at the eyes. The emptied slot's
            // update may land before the event, so the last armour this
            // client saw stands in for it.
            const ItemStack& broken = !m_bodyArmor.IsEmpty() ? m_bodyArmor : m_lastBodyArmorSeen;
            if (broken.IsEmpty() || !m_level) return;
            PlayItemBreakSound(GetBreakSound(broken).c_str());
            JavaRandom& rng = m_level->Random();
            const float xr = -xRot * 0.017453292f;
            const float yr = -yRot * 0.017453292f;
            const ParticleOptions options = ParticleOptions::Item(broken.itemId);
            for (int i = 0; i < 5; ++i) {
                glm::dvec3 d((static_cast<double>(rng.NextFloat()) - 0.5) * 0.1,
                             static_cast<double>(rng.NextFloat()) * 0.1 + 0.1, 0.0);
                d = YRot(XRot(d, xr), yr);
                const double y1 = static_cast<double>(-rng.NextFloat()) * 0.6 - 0.3;
                glm::dvec3 p((static_cast<double>(rng.NextFloat()) - 0.5) * 0.3, y1, 0.6);
                p = YRot(XRot(p, xr), yr);
                p += glm::dvec3(position.x, GetEyeY(), position.z);
                m_level->AddParticle(options, p.x, p.y, p.z, d.x, d.y + 0.05, d.z);
            }
            return;
        }
        GenericAnimal::HandleEntityEvent(id);
    }

    // ── Sounds ──────────────────────────────────────────────────────────────

    const char* Wolf::GetAmbientSound() const {
        // MC Wolf.getAmbientSound: angry → growl; 1 in 3 → pant, or whine
        // for a tame wolf under 20 health; else the plain ambient.
        const WolfSoundVariants::SoundSet& set =
            WolfSoundVariants::Sounds(m_soundVariant, IsBaby());
        if (IsAngry()) return set.growl;
        if (m_level && m_level->Random().NextInt(3) == 0) {
            return IsTame() && GetHealth() < 20.0f ? set.whine : set.pant;
        }
        return set.ambient;
    }

    const char* Wolf::GetHurtSound(MobDamageSource source) const {
        if (CanArmorAbsorb(source)) return SoundEvents::WOLF_ARMOR_DAMAGE;
        return WolfSoundVariants::Sounds(m_soundVariant, IsBaby()).hurt;
    }

    const char* Wolf::GetDeathSound() const {
        return WolfSoundVariants::Sounds(m_soundVariant, IsBaby()).death;
    }

    // ── Interaction ─────────────────────────────────────────────────────────

    UseResult Wolf::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Entity.interact's shears branch runs before mobInteract (Mob
        // .interact → super.interact): canShearEquipment (the owner) and not
        // sneaking takes the armour off. The leash half of that method is
        // the dispatcher's; for the wolf nothing else comes between, so the
        // branch sits here, first.
        if (held.itemId == Items::Shears && IsOwnedBy(player) && !player.IsDiscrete() &&
            TryShearBodyArmor(player, held)) {
            return UseResult::Success;
        }

        if (IsTame()) {
            // Engine feature (not in MC): the owner re-voices their wolf with
            // a note block — the next sound set in registration order
            // (classic → puglin → sad → angry → grumpy → big → cute →
            // classic), announced once with the new set's ambient bark. The
            // note block is not used up. The variant rides the variant byte
            // and "sound_variant" like any other.
            if (held.itemId == ItemRegistry::FromBlock(BlockID::NoteBlock) && IsOwnedBy(player)) {
                if (m_level && !m_level->IsClientSide()) {
                    SetSoundVariant(WolfSoundVariants::Next(m_soundVariant));
                    PlaySound(WolfSoundVariants::Sounds(m_soundVariant, IsBaby()).ambient,
                              GetSoundVolume(), GetVoicePitch());
                }
                return UseResult::Success;
            }

            if (IsFood(held.itemId) && GetHealth() < GetMaxHealth()) {
                Feed(held, 2.0f, 2.0f);
                return UseResult::Success;
            }

            const int dye = DyeColorOf(held);
            if (dye < 0 || !IsOwnedBy(player)) {
                // MC isEquippableInSlot(stack, BODY): wolf armor is the one
                // item whose EQUIPPABLE is BODY with the wolf allowed.
                if (held.itemId == Items::WolfArmor && !IsWearingBodyArmor() &&
                    IsOwnedBy(player) && !IsBaby()) {
                    ItemStack one = held;
                    one.count = 1;
                    SetBodyArmorItem(one);
                    // MC onEquipItem: the equippable's equip sound for all.
                    if (m_level && !m_level->IsClientSide() && !IsSilent()) {
                        PlaySound(SoundEvents::ARMOR_EQUIP_WOLF, 1.0f, 1.0f);
                    }
                    held.count -= 1;   // itemStack.consume(1, player)
                    if (held.count <= 0) held.Clear();
                    return UseResult::Success;
                }

                // MC: the sitting owner's repair — one scute restores an
                // eighth of the armour's durability.
                if (IsInSittingPose() && IsWearingBodyArmor() && IsOwnedBy(player) &&
                    IsDamaged(m_bodyArmor) && IsValidRepairItem(m_bodyArmor, held)) {
                    held.count -= 1;   // itemStack.shrink(1)
                    if (held.count <= 0) held.Clear();
                    PlaySound(SoundEvents::WOLF_ARMOR_REPAIR, 1.0f, 1.0f);
                    const int repairUnit =
                        static_cast<int>(static_cast<float>(GetMaxDamage(m_bodyArmor)) * 0.125f);
                    SetDamageValue(m_bodyArmor, std::max(0, GetDamageValue(m_bodyArmor) - repairUnit));
                    m_bodyArmorDirty = true;
                    return UseResult::Success;
                }

                const UseResult r = Animal::MobInteract(player, held);
                if (!ConsumesAction(r) && IsOwnedBy(player)) {
                    // MC: the sit/stand toggle, with the full stop.
                    SetOrderedToSit(!IsOrderedToSit());
                    jumping = false;
                    GetNavigation().Stop();
                    SetTarget(nullptr);
                    return UseResult::Success;   // MC SUCCESS.withoutItem()
                }
                return r;
            }

            // MC: a dye in the owner's hand recolours the collar — unless it
            // is already that colour, which falls through to Animal's.
            if (static_cast<uint8_t>(dye) != m_collarColor) {
                SetCollarColor(static_cast<uint8_t>(dye));
                held.count -= 1;   // itemStack.consume(1, player)
                if (held.count <= 0) held.Clear();
                return UseResult::Success;
            }
        } else if (m_level && !m_level->IsClientSide() && held.itemId == Items::Bone &&
                   !IsAngry()) {
            UsePlayerItem(held);
            TryToTame(player);
            return UseResult::SuccessServer;
        }

        return Animal::MobInteract(player, held);
    }

    bool Wolf::TryShearBodyArmor(LivingEntity& player, ItemStack& shears) {
        // MC Mob.attemptToShearEquipment over the BODY slot: wolf armor's
        // Equippable is canBeSheared, shearing sound ARMOR_UNEQUIP_WOLF. A
        // PREVENT_ARMOR_CHANGE enchantment (Curse of Binding) keeps it on
        // for anyone not in creative.
        if (!IsWearingBodyArmor() || !m_level || m_level->IsClientSide()) return false;
        if (EnchantmentHelper::HasPreventArmorChange(m_bodyArmor) &&
            !player.IsCreative()) {
            return false;
        }

        // MC shearItem: wear the shears, empty the slot, drop the armour at
        // the passenger attachment point (0, 0.81875, -0.0625).
        const ItemStack sheared = m_bodyArmor;
        HurtAndBreak(shears, 1, player, EquipmentSlot::MAINHAND);
        SetBodyArmorItem(ItemStack{});
        m_level->SpawnItemStackDrop(position + glm::dvec3(0.0, 0.81875, -0.0625), sheared);
        PlaySound(SoundEvents::ARMOR_UNEQUIP_WOLF, 1.0f, 1.0f);
        return true;
    }

    void Wolf::TryToTame(LivingEntity& player) {
        // MC Wolf.tryToTame: 1-in-3.
        if (m_level && m_level->Random().NextInt(3) == 0) {
            Tame(player);
            GetNavigation().Stop();
            SetTarget(nullptr);
            SetOrderedToSit(true);
            BroadcastTamingResult(true);
        } else {
            BroadcastTamingResult(false);
        }
    }

    void Wolf::ApplyTamingSideEffects() {
        // MC Wolf.applyTamingSideEffects: TAME_HEALTH 40, START_HEALTH 8.
        if (IsTame()) {
            m_attributes.SetBaseValue(Attribute::MaxHealth, 40.0);
            SetHealth(40.0f);
        } else {
            m_attributes.SetBaseValue(Attribute::MaxHealth, 8.0);
        }
    }

    // ── Damage ──────────────────────────────────────────────────────────────

    bool Wolf::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Wolf.hurtServer: a hit wolf stands up before taking the damage.
        if (m_level && !m_level->IsClientSide()) SetOrderedToSit(false);
        return GenericAnimal::Hurt(source, amount, attacker);
    }

    bool Wolf::CanArmorAbsorb(MobDamageSource source) const {
        return m_bodyArmor.itemId == Items::WolfArmor && !m_bodyArmor.IsEmpty() &&
               !BypassesWolfArmor(source);
    }

    void Wolf::ActuallyHurt(MobDamageSource source, float amount, Entity* attacker) {
        if (!CanArmorAbsorb(source)) {
            // MC super.actuallyHurt. Of the sources that get past the armour
            // only thorns is not #bypasses_armor, so only thorns reaches
            // hurtArmor → doHurtEquipment(BODY): max(1, damage / 4) wear.
            if (source == MobDamageSource::Thorns && IsWearingBodyArmor() && amount > 0.0f &&
                m_level && !m_level->IsClientSide() && IsDamageableItem(m_bodyArmor)) {
                HurtAndBreak(m_bodyArmor, static_cast<int>(std::max(1.0f, amount / 4.0f)),
                             *this, EquipmentSlot::BODY);
                SetBodyArmorItem(m_bodyArmor);
            }
            // The void, cramming and drowning are #bypasses_armor: the
            // armour's ARMOR modifier must not soften them.
            if (IsWearingBodyArmor() &&
                (source == MobDamageSource::Void || source == MobDamageSource::Cramming ||
                 source == MobDamageSource::Drown)) {
                SwapEquipmentModifiers(m_attributes, EquipmentSlot::BODY, m_bodyArmorModifiersFrom, ItemStack{});
                m_bodyArmorModifiersFrom = ItemStack{};
                GenericAnimal::ActuallyHurt(source, amount, attacker);
                SetBodyArmorItem(m_bodyArmor);   // re-applies the modifiers
                return;
            }
            GenericAnimal::ActuallyHurt(source, amount, attacker);
            return;
        }

        // MC Wolf.actuallyHurt: the armour takes ceil(damage) durability and
        // the wolf nothing; crossing a Crackiness step cracks audibly with a
        // puff of scute shards.
        if (!m_level || m_level->IsClientSide()) return;
        const int damageBefore = GetDamageValue(m_bodyArmor);
        const int maxDamage = GetMaxDamage(m_bodyArmor);
        HurtAndBreak(m_bodyArmor, static_cast<int>(std::ceil(amount)), *this, EquipmentSlot::BODY);
        SetBodyArmorItem(m_bodyArmor);

        const WolfArmorCrackiness before = WolfArmorCrackinessByDamage(damageBefore, maxDamage);
        const WolfArmorCrackiness after = IsDamageableItem(m_bodyArmor)
            ? WolfArmorCrackinessByDamage(GetDamageValue(m_bodyArmor), GetMaxDamage(m_bodyArmor))
            : WolfArmorCrackiness::None;
        if (before != after) {
            PlaySound(SoundEvents::WOLF_ARMOR_CRACK, 1.0f, 1.0f);
            m_level->SendParticles(ParticleOptions::Item(Items::ArmadilloScute),
                                   position.x, position.y + 1.0, position.z,
                                   20, 0.2, 0.1, 0.2, 0.1);
        }
    }

    // ── Body armor ──────────────────────────────────────────────────────────

    void Wolf::SetBodyArmorItem(const ItemStack& stack) {
        if (&stack != &m_bodyArmor) m_bodyArmor = stack;
        if (m_bodyArmor.count <= 0 || m_bodyArmor.itemId == Items::Air) m_bodyArmor = ItemStack{};
        if (!m_bodyArmor.IsEmpty()) m_lastBodyArmorSeen = m_bodyArmor;
        m_bodyArmorDirty = true;

        // MC LivingEntity.collectEquipmentChanges: the BODY slot's item
        // attribute modifiers ("When equipped: +11 Armor") come and go with
        // the piece. Server-side state; the client never reads armour.
        SwapEquipmentModifiers(m_attributes, EquipmentSlot::BODY, m_bodyArmorModifiersFrom, m_bodyArmor);
        m_bodyArmorModifiersFrom = m_bodyArmor;
    }

    ItemStack* Wolf::EquipmentInSlot(EquipmentSlot slot) {
        return slot == EquipmentSlot::BODY ? &m_bodyArmor : nullptr;
    }

    void Wolf::DropCustomDeathLoot(EntityLevel& level) {
        // MC Mob.dropCustomDeathLoot: the BODY slot was set with
        // setItemSlotAndDropWhenKilled (a guaranteed drop), so the armour
        // always comes off — unless it carries PREVENT_EQUIPMENT_DROP
        // (Curse of Vanishing).
        if (m_bodyArmor.IsEmpty()) return;
        if (!EnchantmentHelper::HasPreventEquipmentDrop(m_bodyArmor)) {
            level.SpawnItemStackDrop(position, m_bodyArmor);
        }
        SetBodyArmorItem(ItemStack{});
    }

    // ── Targeting / breeding / spawning ────────────────────────────────────

    bool Wolf::WantsToAttack(const LivingEntity& target,
                             const LivingEntity& owner) const {
        // MC Wolf.wantsToAttack, condition for condition.
        const EntityTypeId type = target.GetType();
        if (type == EntityTypeId::Creeper || type == EntityTypeId::Ghast ||
            type == EntityTypeId::ArmorStand) {
            return false;
        }
        if (const auto* wolf = dynamic_cast<const Wolf*>(&target)) {
            // `!wolfTarget.isTame() || wolfTarget.getOwner() != owner`.
            return !wolf->IsTame() || wolf->GetOwner() != &owner;
        }
        // MC: a player target, when the owner is a player, only if the owner
        // canHarmPlayer — ServerPlayer.canHarmPlayer is the pvp rule (this
        // engine has no teams to add the friendly-fire half).
        if (target.IsPlayer() && owner.IsPlayer() && !Rules::GetBool(Rules::Id::Pvp)) {
            return false;
        }
        if (const auto* horse = dynamic_cast<const AbstractHorse*>(&target)) {
            if (horse->IsTamedHorse()) return false;
        }
        if (const auto* tamable = dynamic_cast<const TamableAnimal*>(&target)) {
            if (tamable->IsTame()) return false;
        }
        return true;
    }

    bool Wolf::CanMate(const Animal& other) const {
        // MC Wolf.canMate: both tame, the partner not sitting, both in love.
        if (&other == this) return false;
        if (!IsTame()) return false;
        const auto* wolf = dynamic_cast<const Wolf*>(&other);
        if (!wolf || !wolf->IsTame()) return false;
        if (wolf->IsInSittingPose()) return false;
        return IsInLove() && other.IsInLove();
    }

    void Wolf::SpawnChildFromBreeding(Animal& partner) {
        m_breedPartner = dynamic_cast<const Wolf*>(&partner);
        GenericAnimal::SpawnChildFromBreeding(partner);
        m_breedPartner = nullptr;
    }

    std::unique_ptr<Animal> Wolf::CreateBaby() {
        // MC Wolf.getBreedOffspring(level, partner). Without a noted partner
        // (a spawn egg on an adult) the partner is this wolf itself, which
        // is what SpawnEggItem hands MC's getBreedOffspring too.
        auto baby = std::make_unique<Wolf>(m_level);
        const Wolf& partner = m_breedPartner ? *m_breedPartner : *this;
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            baby->SetVariant(rng.NextBool() ? m_variant : partner.m_variant);
            if (IsTame()) {
                baby->SetOwnerUuid(GetOwnerUuid());
                baby->SetTame(true, /*includeSideEffects=*/true);
                baby->SetCollarColor(GetMixedDyeColor(m_collarColor, partner.m_collarColor, rng));
            }
            baby->SetSoundVariant(WolfSoundVariants::PickRandom(rng));
        }
        return baby;
    }

    std::shared_ptr<SpawnGroupData>
    Wolf::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (!m_level) return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
        JavaRandom& rng = m_level->Random();

        // MC: a pack shares the first member's coat (WolfPackData);
        // otherwise VariantUtils.selectVariantToSpawn for the biome here.
        if (auto* pack = dynamic_cast<WolfPackData*>(groupData.get())) {
            SetVariant(pack->variant);
        } else {
            std::string_view biome = "plains";
            if (const IBlockAccess* blocks = m_level->Blocks()) {
                const glm::ivec3 p = BlockPosition();
                biome = BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name;
            }
            SetVariant(WolfVariants::SelectToSpawn(biome, rng));
            groupData = std::make_shared<WolfPackData>(m_variant);
        }

        SetSoundVariant(WolfSoundVariants::PickRandom(rng));
        return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
    }

    // ── Llama ──────────────────────────────────────────────────────────────

    Llama::Llama(EntityTypeId type, EntityLevel* level)
        : GenericAnimal(type, level), HorseTaming(this) {
        // MC Llama.createAttributes = AbstractChestedHorse
        // .createBaseChestedHorseAttributes: AbstractHorse's base
        // (JUMP_STRENGTH 0.7, MAX_HEALTH 53, MOVEMENT_SPEED 0.225, STEP_HEIGHT
        // 1, SAFE_FALL_DISTANCE 6, FALL_DAMAGE_MULTIPLIER 0.5) with the
        // chested speed 0.175 and jump 0.5. The def row carries health, speed
        // and step; the rest are MC's too.
        m_attributes.Register(Attribute::JumpStrength, 0.5);
        m_attributes.Register(Attribute::SafeFallDistance, 6.0);
        m_attributes.Register(Attribute::FallDamageMultiplier, 0.5);
        m_health = GetMaxHealth();
        // MC Llama's constructor: getNavigation().setRequiredPathLength(40).
        GetNavigation().SetRequiredPathLength(40.0f);

        // MC Llama.registerGoals replaces the table outright (the Bat
        // precedent for a promoted class disowning its generic base goals).
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        RegisterLlamaGoals();
    }

    void Llama::RegisterLlamaGoals() {
        // MC Llama.registerGoals, priority for priority (it does not call
        // AbstractHorse.addBehaviourGoals: no MountPanicGoal, a plain
        // PanicGoal(1.2) at 3).
        static const ItemID kHayBlock = RecipeManager::ItemFromSlug("hay_block");
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<RunAroundLikeCrazyGoal>(this, this, 1.2));
        m_goalSelector.AddGoal(2, std::make_unique<LlamaFollowCaravanGoal>(this, 2.0999999046325684));
        m_goalSelector.AddGoal(3, std::make_unique<RangedAttackGoal>(this, this, 1.25, 40, 20.0f));
        m_goalSelector.AddGoal(3, std::make_unique<PanicGoal>(this, 1.2));
        m_goalSelector.AddGoal(4, std::make_unique<BreedGoal>(this, 1.0));
        // MC TemptGoal(1.25, ItemTags.LLAMA_TEMPT_ITEMS = [hay_block]).
        m_goalSelector.AddGoal(5, std::make_unique<TemptGoal>(this, 1.25, false, kHayBlock));
        m_goalSelector.AddGoal(6, std::make_unique<FollowParentGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.7));
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(9, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<LlamaHurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<LlamaAttackWolfGoal>(this));
    }

    bool Llama::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool wasHurt = GenericAnimal::Hurt(source, amount, attacker);
        // MC AbstractHorse.hurtServer: `random.nextInt(3) == 0 →
        // standIfPossible()`; a llama cannot rear, but the roll is drawn.
        if (wasHurt && m_level && !m_level->IsClientSide()) {
            (void)m_level->Random().NextInt(3);
        }
        return wasHurt;
    }

    bool Llama::CauseFallDamage(double fallDist, float damageMultiplier) {
        // MC Llama.causeFallDamage.
        if (m_level && m_level->IsClientSide()) return false;
        const int damage = CalculateFallDamage(fallDist, damageMultiplier);
        if (damage <= 0) return false;
        if (fallDist >= 6.0) {
            Hurt(MobDamageSource::Fall, static_cast<float>(damage), nullptr);
            // (MC's propagateFallToPassengers here is Entity::CheckFallDamage's
            // in this engine — the riders already took this landing.)
        }
        PlayBlockFallSound();
        return true;
    }

    void Llama::AiStep() {
        // MC AbstractHorse.aiStep: moveTail on a 1-in-200 roll (both sides;
        // the llama model draws no tail swish, the roll is the RNG stream's).
        if (m_level) (void)m_level->Random().NextInt(200);
        GenericAnimal::AiStep();
        if (m_level && !m_level->IsClientSide() && IsAlive()) {
            if (m_level->Random().NextInt(900) == 0 && deathTime == 0) Heal(1.0f);
            // canEatGrass is false; followMommy needs the Bred flag, which
            // only an MC save's "Bred" sets.
        }
    }

    bool Llama::CanParent() const {
        // MC AbstractHorse.canParent.
        return !IsVehicle() && !IsPassenger() && IsTamed() && !IsBaby() &&
               GetHealth() >= GetMaxHealth() && IsInLove();
    }

    bool Llama::CanMate(const Animal& other) const {
        // MC Llama.canMate: `partner != this && partner instanceof Llama`
        // (a trader llama included) and both canParent.
        if (&other == this) return false;
        const auto* llama = dynamic_cast<const Llama*>(&other);
        return llama && CanParent() && llama->CanParent();
    }

    void Llama::PerformRangedAttack(LivingEntity& target, float power) {
        (void)power;
        Spit(target);
    }

    namespace {
        // MC Llama.LlamaGroupData — the coat the first llama of a pack
        // rolled, shared by the rest — over AgeableMobGroupData(true): the
        // first member spawns adult, each later one a baby on a 5 % roll.
        struct LlamaGroupData : SpawnGroupData {
            explicit LlamaGroupData(Llama::Variant v) : variant(v) {}
            Llama::Variant variant;
            float babySpawnChance = 0.05f;
            int   groupSize = 0;
        };
        // MC AgeableMob.AgeableMobGroupData(false) — what TraderLlama hands
        // Llama.finalizeSpawn: no LlamaGroupData, so the coat is rolled.
        struct TraderLlamaGroupData : SpawnGroupData {};
    }

    std::shared_ptr<SpawnGroupData>
    Llama::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            // MC Llama.setRandomStrength.
            const int maxStrength = rng.NextFloat() < 0.04f ? 5 : 3;
            SetStrength(1 + rng.NextInt(maxStrength));
            // The coat: the pack's, else Util.getRandom(Variant.values()).
            auto* pack = dynamic_cast<LlamaGroupData*>(groupData.get());
            if (pack) {
                SetVariant(pack->variant);
            } else {
                const Variant variant = VariantById(rng.NextInt(kVariantCount));
                SetVariant(variant);
                auto fresh = std::make_shared<LlamaGroupData>(variant);
                pack = fresh.get();
                groupData = std::move(fresh);
            }
            // AbstractHorse.finalizeSpawn → AbstractChestedHorse
            // .randomizeAttributes: MAX_HEALTH = generateMaxHealth (15..30);
            // the health follows the lowered maximum.
            m_attributes.SetBaseValue(Attribute::MaxHealth, static_cast<double>(GenerateMaxHealth(rng)));
            m_health = GetMaxHealth();
            // AgeableMob.finalizeSpawn: later pack members roll a baby.
            if (pack->groupSize > 0 && rng.NextFloat() <= pack->babySpawnChance) {
                SetAge(kBabyStartAge);
            }
            ++pack->groupSize;
        }
        return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
    }

    void Llama::MakeMad() {
        // MC AbstractHorse.makeMad: !isStanding → standIfPossible (a llama
        // cannot rear) and makeSound(getAngrySound()).
        if (m_level && !m_level->IsClientSide()) MakeSound(SoundEvents::LLAMA_ANGRY);
    }

    UseResult Llama::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC AbstractChestedHorse.mobInteract: not ridden, not the tamed
        // sneak (inventory), not a baby with a golden dandelion — food →
        // fedFood, any other item on an untamed llama → makeMad (a chest on
        // a tamed one → equipChest: no chested inventory here); then
        // AbstractHorse.mobInteract: the item's own interaction, else
        // doPlayerRide.
        const bool clientSide = m_level && m_level->IsClientSide();
        const bool shouldOpenInventory = !IsBaby() && IsTamed() && IsSecondaryUseActive(player);
        if (IsEquineVehicle() || shouldOpenInventory ||
            (IsBaby() && held.itemId == ItemRegistry::FromBlock(BlockID::GoldenDandelion))) {
            // AbstractHorse.mobInteract: unridden and grown, the tamed
            // sneak opens the inventory.
            if (shouldOpenInventory && !IsEquineVehicle()) {
                OpenCustomInventoryScreen(player);
                return UseResult::Success;
            }
            return GenericAnimal::MobInteract(player, held);
        }
        if (!held.IsEmpty()) {
            if (IsFood(held.itemId)) return FedFood(player, held);
            if (!IsTamed()) {
                MakeMad();
                return UseResult::Success;
            }
            // AbstractChestedHorse: a chest on a tamed llama without one.
            if (!HasChest() && held.itemId == ItemRegistry::FromBlock(BlockID::Chest)) {
                if (!clientSide) MountInventory::EquipChest(*this, held, SoundEvents::LLAMA_CHEST);
                return UseResult::Success;
            }
        }
        if (IsBaby()) return GenericAnimal::MobInteract(player, held);
        if (!held.IsEmpty()) {
            if (const auto interact = ItemRegistry::Get(held.itemId).interactLivingEntity) {
                const UseResult r = interact(held, *this);
                if (ConsumesAction(r)) return r;
            }
            // isEquippableInSlot(stack, BODY) && !isWearingBodyArmor() → a
            // carpet goes on (equipBodyArmor).
            if (IsEquippableInSlot(held, EquipmentSlot::BODY) && !HasItemInSlot(EquipmentSlot::BODY)) {
                EquipBodyArmor(player, held);
                return UseResult::Success;
            }
        }
        if (!clientSide) DoPlayerRide(player);
        return UseResult::Success;
    }

    void Llama::EquipBodyArmor(LivingEntity& player, ItemStack& held) {
        // MC AbstractHorse.equipBodyArmor (setItemSlotAndDropWhenKilled of
        // one carpet; the LLAMA_SWAG equip sound is the carpet's own).
        (void)player;
        if (!m_level || m_level->IsClientSide() || !IsEquippableInSlot(held, EquipmentSlot::BODY)) return;
        ItemStack one = held;
        one.count = 1;
        held.count -= 1;
        if (held.count <= 0) held.Clear();
        SetItemSlotAndDropWhenKilled(EquipmentSlot::BODY, one);
    }

    void Llama::OpenCustomInventoryScreen(LivingEntity& player) {
        // MC AbstractHorse.openCustomInventoryScreen.
        if (!m_level || m_level->IsClientSide()) return;
        if ((!IsVehicle() || HasPassenger(player)) && IsTamed()) {
            m_level->OpenMountInventory(player, *this);
        }
    }

    UseResult Llama::FedFood(LivingEntity& player, ItemStack& held) {
        const bool ate = HandleEating(player, held);
        if (ate) UsePlayerItem(held);
        const bool clientSide = m_level && m_level->IsClientSide();
        return !ate && !clientSide ? UseResult::Pass : UseResult::SuccessServer;
    }

    bool Llama::HandleEating(LivingEntity& player, const ItemStack& held) {
        // MC Llama.handleEating, verbatim.
        static const ItemID kHayBlock = RecipeManager::ItemFromSlug("hay_block");
        const bool clientSide = m_level && m_level->IsClientSide();
        int   ageUpSeconds = 0;
        int   temper = 0;
        float heal = 0.0f;
        bool  itemUsed = false;
        if (held.itemId == Items::Wheat) {
            ageUpSeconds = 10; temper = 3; heal = 2.0f;
        } else if (held.itemId == kHayBlock) {
            ageUpSeconds = 90; temper = 6; heal = 10.0f;
            if (IsTamed() && GetAge() == 0 && CanFallInLove()) {
                itemUsed = true;
                SetInLove(&player);
            }
        }
        if (GetHealth() < GetMaxHealth() && heal > 0.0f) {
            Heal(heal);
            itemUsed = true;
        }
        if (IsBaby() && ageUpSeconds > 0 && !IsAgeLocked()) {
            if (!clientSide) {
                SendHappyVillagerParticle();
                AgeUp(ageUpSeconds);
                itemUsed = true;
            }
        }
        if (temper > 0 && (itemUsed || !IsTamed()) && GetTemper() < GetMaxTemper() && !clientSide) {
            ModifyTemper(temper);
            itemUsed = true;
        }
        if (itemUsed && !IsSilent() && m_level) {
            JavaRandom& rng = m_level->Random();
            m_level->PlaySound(nullptr, position, SoundEvents::LLAMA_EAT, GetSoundSource(),
                               1.0f, 1.0f + (rng.NextFloat() - rng.NextFloat()) * 0.2f);
        }
        return itemUsed;
    }

    void Llama::DoPlayerRide(LivingEntity& player) {
        // MC AbstractHorse.doPlayerRide (a llama never eats grass or rears,
        // so the eating/standing resets are no-ops for it).
        if (m_level && !m_level->IsClientSide()) StartPlayerRide(player);
    }

    glm::dvec3 Llama::PlayerRiderPosition() const {
        // EntityTypes LLAMA/TRADER_LLAMA .passengerAttachments(0, 1.37, -0.3),
        // EntityAttachments.get → .yRot(-yRot rad); a baby's is (0, height
        // - 0.25, -0.3) at half scale (a baby cannot be mounted anyway).
        const double a = -static_cast<double>(yRot) * static_cast<double>(Mth::kDegToRad);
        const double y = IsBaby() ? (1.87 - 0.25) * 0.5 : 1.37;
        const double z = IsBaby() ? -0.3 * 0.5 : -0.3;
        const glm::dvec3 seat(z * std::sin(a), y, z * std::cos(a));
        return position + seat - glm::dvec3(0.0, kPlayerVehicleAttachmentY, 0.0);
    }

    void Llama::HandleEntityEvent(uint8_t id) {
        if (id == 7) { SpawnTamingParticles(true); return; }
        if (id == 6) { SpawnTamingParticles(false); return; }
        GenericAnimal::HandleEntityEvent(id);
    }

    std::unique_ptr<Llama> Llama::MakeNewLlama() {
        return std::make_unique<Llama>(m_level);
    }

    void Llama::SpawnChildFromBreeding(Animal& partner) {
        m_breedPartner = dynamic_cast<const Llama*>(&partner);
        GenericAnimal::SpawnChildFromBreeding(partner);
        m_breedPartner = nullptr;
    }

    std::unique_ptr<Animal> Llama::CreateBaby() {
        // MC Llama.getBreedOffspring. Without a noted partner (a spawn egg on
        // an adult) the partner is this llama, as for the wolf.
        std::unique_ptr<Llama> baby = MakeNewLlama();
        if (!baby) return nullptr;
        const Llama& other = m_breedPartner ? *m_breedPartner : *this;
        if (m_level) {
            // AbstractHorse.setOffspringAttributes first (its three rolls
            // come before the strength's in the stream).
            SetOffspringAttributes(*this, other, *baby);
            JavaRandom& rng = m_level->Random();
            int babyStrength = rng.NextInt(std::max(GetStrength(), other.GetStrength())) + 1;
            if (rng.NextFloat() < 0.03f) ++babyStrength;
            baby->SetStrength(babyStrength);
            baby->SetVariant(rng.NextBool() ? GetVariant() : other.GetVariant());
        }
        return baby;
    }

    void Llama::Spit(LivingEntity& target) {
        if (!m_level) return;

        auto spit = std::make_unique<LlamaSpit>(m_level);
        spit->InitFromLlama(*this);

        // MC Llama.spit: aim a third of the way up the target's box with the
        // standard 0.2-per-horizontal-block loft, velocity 1.5, inaccuracy 10.
        const double xd = target.position.x - position.x;
        const double yd = (target.position.y + target.GetBbHeight() / 3.0) -
                          spit->position.y;
        const double zd = target.position.z - position.z;
        const double yo = std::sqrt(xd * xd + zd * zd) * 0.2;

        spit->Shoot(xd, yd + yo, zd, 1.5f, 10.0f);
        if (!IsSilent()) {
            JavaRandom& rng = m_level->Random();
            m_level->PlaySound(nullptr, position, SoundEvents::LLAMA_SPIT, GetSoundSource(),
                               1.0f, 1.0f + (rng.NextFloat() - rng.NextFloat()) * 0.2f);
        }
        m_level->AddFreshEntity(std::move(spit));

        m_didSpit = true;
    }

    // ── TraderLlama ────────────────────────────────────────────────────────

    namespace {
        // MC NearestAttackableTargetGoal(this, Zombie.class, true,
        // !ZOMBIFIED_PIGLIN): the Zombie class and its subclasses bar the
        // piglin. And AbstractIllager.class: the four illagers.
        constexpr EntityTypeId kTraderLlamaZombieTargets[] = {
            EntityTypeId::Zombie, EntityTypeId::Husk, EntityTypeId::Drowned,
            EntityTypeId::ZombieVillager,
        };
        constexpr EntityTypeId kTraderLlamaIllagerTargets[] = {
            EntityTypeId::Evoker, EntityTypeId::Illusioner, EntityTypeId::Pillager,
            EntityTypeId::Vindicator,
        };
    }

    TraderLlama::TraderLlama(EntityLevel* level)
        : Llama(EntityTypeId::TraderLlama, level) {
        // MC TraderLlama.registerGoals: super.registerGoals() (Llama's
        // table, built by the Llama constructor), then its own on top.
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 2.0));
        m_targetSelector.AddGoal(1, std::make_unique<TraderLlamaDefendWanderingTraderGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(
            this, kTraderLlamaZombieTargets,
            static_cast<int>(std::size(kTraderLlamaZombieTargets)), true));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(
            this, kTraderLlamaIllagerTargets,
            static_cast<int>(std::size(kTraderLlamaIllagerTargets)), true));
    }

    std::unique_ptr<Llama> TraderLlama::MakeNewLlama() {
        auto baby = std::make_unique<TraderLlama>(m_level);
        baby->SetPersistenceRequired(true);
        return baby;
    }

    std::shared_ptr<SpawnGroupData>
    TraderLlama::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (reason == SpawnReason::Event) SetAge(0);
        if (!groupData) groupData = std::make_shared<TraderLlamaGroupData>();
        return Llama::FinalizeSpawn(reason, std::move(groupData));
    }

    WanderingTrader* TraderLlama::GetLeashedWanderingTrader() const {
        return dynamic_cast<WanderingTrader*>(GetLeashHolder());
    }

    bool TraderLlama::IsLeashedToSomethingOtherThanTheWanderingTrader() const {
        return IsLeashed() && !GetLeashedWanderingTrader();
    }

    bool TraderLlama::CanDespawn() const {
        // MC canDespawn: !isTamed && !isLeashedToSomethingOtherThanTheWanderingTrader
        // && !hasExactlyOnePlayerPassenger && !isAgeLocked && !isPersistenceRequired.
        // (A player sits in the llama's PlayerRideable seat, not in the
        // passenger list — HasExactlyOnePlayerPassenger reads the seat.)
        const std::vector<Entity*>& riders = GetPassengers();
        const bool onePlayerPassenger = (riders.size() == 1 && riders.front() && riders.front()->IsPlayer()) ||
                                        HasExactlyOnePlayerPassenger();
        return !IsTamed() && !IsLeashedToSomethingOtherThanTheWanderingTrader() && !onePlayerPassenger &&
               !IsAgeLocked() && !IsPersistenceRequired();
    }

    void TraderLlama::DoPlayerRide(LivingEntity& player) {
        // MC TraderLlama.doPlayerRide: only when not on a wandering trader's lead.
        if (!GetLeashedWanderingTrader()) Llama::DoPlayerRide(player);
    }

    void TraderLlama::MaybeDespawn() {
        // MC maybeDespawn: while on the trader's lead the clock is the
        // trader's minus one — they go together; off it, its own.
        if (!CanDespawn()) return;
        const WanderingTrader* trader = GetLeashedWanderingTrader();
        m_despawnDelay = trader ? trader->GetDespawnDelay() - 1 : m_despawnDelay - 1;
        if (m_despawnDelay <= 0) {
            RemoveLeash();
            Discard();
        }
    }

    void TraderLlama::AiStep() {
        Llama::AiStep();
        if (m_level && !m_level->IsClientSide() && !IsRemoved()) MaybeDespawn();
    }

    // ── Shared spawn-pack token ────────────────────────────────────────────

    namespace {

        // MC AgeableMob.AgeableMobGroupData — the first pack member spawns
        // adult, later members roll babyChance. MC increments the size as
        // each member finalizes; so does this.
        struct AgeableGroupData : SpawnGroupData {
            explicit AgeableGroupData(float chance) : babyChance(chance) {}
            float babyChance;
            int   size = 0;
        };

    } // namespace

    // ── Fox ────────────────────────────────────────────────────────────────

    namespace {

        constexpr EntityTypeId kFoxWolfAvoid[]  = { EntityTypeId::Wolf };
        constexpr EntityTypeId kFoxBearAvoid[]  = { EntityTypeId::PolarBear };

        // MC BiomeTags.SPAWNS_SNOW_FOXES, flattened from the data pack (no
        // biome-tag resolver here — the Sheep colour tables set the pattern).
        constexpr std::string_view kSnowFoxBiomes[] = {
            "frozen_peaks", "grove", "ice_spikes", "jagged_peaks",
            "snowy_plains", "snowy_slopes", "snowy_taiga",
        };

        // MC Fox.FoxGroupData — the pack token carrying the shared variant.
        struct FoxGroupData : SpawnGroupData {
            explicit FoxGroupData(Fox::Variant v) : variant(v) {}
            Fox::Variant variant;
            int size = 0;
        };

    } // namespace

    void Fox::CreateAttributes(AttributeMap& out) {
        // MC Fox.createAttributes: MOVEMENT_SPEED 0.3, MAX_HEALTH 10,
        // ATTACK_DAMAGE 2, SAFE_FALL_DISTANCE 5, FOLLOW_RANGE 32.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MovementSpeed,   0.3);
        out.Register(Attribute::MaxHealth,      10.0);
        out.Register(Attribute::AttackDamage,    2.0);
        out.Register(Attribute::SafeFallDistance, 5.0);
        out.Register(Attribute::FollowRange,    32.0);
    }

    Fox::Fox(EntityLevel* level) : Animal(EntityTypeId::Fox, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();

        // MC's constructor wiring: the fox's own look/move controls, the
        // zeroed danger-other maluses (foxes hunt things other mobs shy away
        // from), and canPickUpLoot (inert until mob item pickup exists).
        // setRequiredPathLength(32) is a pathfinder budget knob this port's
        // navigation does not expose.
        SetLookControl(MakeFoxLookControl(this));
        SetMoveControl(MakeFoxMoveControl(this));
        SetPathfindingMalus(PathType::DangerOther, 0.0f);
        SetPathfindingMalus(PathType::DamageOther, 0.0f);
        SetCanPickUpLoot(true);

        RegisterGoals();
    }

    bool Fox::IsFood(uint32_t itemId) const {
        // MC ItemTags.FOX_FOOD: sweet berries, glow berries.
        return itemId == Items::SweetBerries || itemId == Items::GlowBerries;
    }

    std::unique_ptr<Animal> Fox::CreateBaby() {
        auto baby = std::make_unique<Fox>(m_level);
        // MC picks randomly between the two parents' variants; only this
        // parent is reachable here, which is MC's result whenever the pair
        // matches — the common case, since packs share a biome.
        baby->SetVariant(GetVariant());
        // FoxBreedGoal.breed's addTrustedEntity calls, captured by
        // SpawnChildFromBreeding (empty on every other path).
        for (const Uuid& uuid : m_pendingOffspringTrust) baby->AddTrustedUuid(uuid);
        return baby;
    }

    bool Fox::Trusts(const LivingEntity& entity) const {
        // MC trusts → EntityReference.matches: the entity's UUID.
        const Uuid& id = entity.GetUuid();
        if (UuidIsNil(id)) return false;
        return m_trusted[0] == id || m_trusted[1] == id;
    }

    void Fox::AddTrustedEntity(const LivingEntity& entity) {
        AddTrustedUuid(entity.GetUuid());
    }

    void Fox::AddTrustedUuid(const Uuid& uuid) {
        if (UuidIsNil(uuid)) return;
        // MC addTrustedEntity: slot 1 when slot 0 is taken, else slot 0.
        if (!UuidIsNil(m_trusted[0])) m_trusted[1] = uuid;
        else                          m_trusted[0] = uuid;
    }

    std::vector<Uuid> Fox::GetTrustedUuids() const {
        std::vector<Uuid> out;
        for (const Uuid& u : m_trusted) {
            if (!UuidIsNil(u)) out.push_back(u);
        }
        return out;
    }

    void Fox::SpawnChildFromBreeding(Animal& partner) {
        // MC FoxBreedGoal.breed: animalLoveCause first; the partner's too when
        // it is a different player. getLoveCause answers only for a player
        // who is online (level.getPlayerByUUID) — GetLoveCauseId's -1.
        m_pendingOffspringTrust.clear();
        const bool mine = GetLoveCauseId() != -1;
        const bool theirs = partner.GetLoveCauseId() != -1;
        const Uuid animalCause = mine ? LoveCauseRef().GetUuid() : Uuid{};
        const Uuid partnerCause = theirs ? partner.LoveCauseRef().GetUuid() : Uuid{};
        if (mine) m_pendingOffspringTrust.push_back(animalCause);
        if (theirs && (!mine || partnerCause != animalCause)) m_pendingOffspringTrust.push_back(partnerCause);
        Animal::SpawnChildFromBreeding(partner);
        m_pendingOffspringTrust.clear();
    }

    void Fox::OnOffspringSpawnedFromEgg(LivingEntity& spawner, Mob& offspring) {
        if (auto* cub = dynamic_cast<Fox*>(&offspring)) cub->AddTrustedEntity(spawner);
    }

    void Fox::RegisterGoals() {
        // MC Fox.registerGoals, priority for priority. The inert village
        // stroll says why at its declaration in FoxGoals.hpp.
        m_goalSelector.AddGoal(0, std::make_unique<FoxFloatGoal>(this));
        m_goalSelector.AddGoal(0, std::make_unique<ClimbOnTopOfPowderSnowGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<FaceplantGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<FoxPanicGoal>(this, 2.2));
        m_goalSelector.AddGoal(3, std::make_unique<FoxBreedGoal>(this, 1.0));
        // MC's per-goal avoid gates — players: AVOID_PLAYERS (not sneaking,
        // not creative/spectator) && !trusts; wolves: !isTame; all three:
        // !isDefending. They live in FoxAvoidEntityGoal::AcceptsThreat.
        m_goalSelector.AddGoal(4, std::make_unique<FoxAvoidEntityGoal>(
                                      this, 16.0f, 1.6, 1.4));
        m_goalSelector.AddGoal(4, std::make_unique<FoxAvoidEntityGoal>(
                                      this, kFoxWolfAvoid, 1, 8.0f, 1.6, 1.4));
        m_goalSelector.AddGoal(4, std::make_unique<FoxAvoidEntityGoal>(
                                      this, kFoxBearAvoid, 1, 8.0f, 1.6, 1.4));
        m_goalSelector.AddGoal(5, std::make_unique<StalkPreyGoal>(this));
        m_goalSelector.AddGoal(6, std::make_unique<FoxPounceGoal>(this));
        m_goalSelector.AddGoal(6, std::make_unique<SeekShelterGoal>(this, 1.25));
        m_goalSelector.AddGoal(7, std::make_unique<FoxMeleeAttackGoal>(this, 1.2, true));
        m_goalSelector.AddGoal(7, std::make_unique<SleepGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<FoxFollowParentGoal>(this, 1.25));
        m_goalSelector.AddGoal(9, std::make_unique<FoxStrollThroughVillageGoal>(
                                      this, 32, 200));
        m_goalSelector.AddGoal(10, std::make_unique<FoxEatBerriesGoal>(
                                       this, 1.2, 12, 1));
        m_goalSelector.AddGoal(10, std::make_unique<LeapAtTargetGoal>(this, 0.4f));
        m_goalSelector.AddGoal(11, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                       this, 1.0));
        m_goalSelector.AddGoal(11, std::make_unique<FoxSearchForItemsGoal>(this));
        m_goalSelector.AddGoal(12, std::make_unique<FoxLookAtPlayerGoal>(this, 24.0f));
        m_goalSelector.AddGoal(13, std::make_unique<PerchAndSearchGoal>(this));
        m_targetSelector.AddGoal(3, std::make_unique<DefendTrustedTargetGoal>(this));
    }

    void Fox::SetTargetGoals() {
        // MC Fox.setTargetGoals — the variant decides which prey the fox
        // prioritises: red foxes hunt land prey first, snow foxes fish first.
        if (m_targetGoalsSet) return;
        m_targetGoalsSet = true;
        if (GetVariant() == Variant::Red) {
            m_targetSelector.AddGoal(4, std::make_unique<FoxPreyTargetGoal>(
                                            this, FoxPreyTargetGoal::Kind::LandPrey, 10));
            m_targetSelector.AddGoal(4, std::make_unique<FoxPreyTargetGoal>(
                                            this, FoxPreyTargetGoal::Kind::BabyTurtles, 10));
            m_targetSelector.AddGoal(6, std::make_unique<FoxPreyTargetGoal>(
                                            this, FoxPreyTargetGoal::Kind::Fish, 20));
        } else {
            m_targetSelector.AddGoal(4, std::make_unique<FoxPreyTargetGoal>(
                                            this, FoxPreyTargetGoal::Kind::Fish, 20));
            m_targetSelector.AddGoal(6, std::make_unique<FoxPreyTargetGoal>(
                                            this, FoxPreyTargetGoal::Kind::LandPrey, 10));
            m_targetSelector.AddGoal(6, std::make_unique<FoxPreyTargetGoal>(
                                            this, FoxPreyTargetGoal::Kind::BabyTurtles, 10));
        }
    }

    void Fox::ClearStates() {
        SetIsInterested(false);
        SetIsCrouching(false);
        SetSitting(false);
        SetSleeping(false);
        SetDefending(false);
        SetFaceplanted(false);
    }

    void Fox::SetTarget(LivingEntity* target) {
        // MC: dropping the target drops the defence.
        if (IsDefending() && target == nullptr) SetDefending(false);
        Animal::SetTarget(target);
    }

    float Fox::GetHeadRollAngle(float partialTick) const {
        return Mth::Lerp(partialTick, m_interestedAngleO, m_interestedAngle)
             * 0.11f * Mth::kPi;
    }

    float Fox::GetCrouchAmount(float partialTick) const {
        return Mth::Lerp(partialTick, m_crouchAmountO, m_crouchAmount);
    }

    bool Fox::IsPathClear(const Fox& fox, const LivingEntity& target) {
        // MC Fox.isPathClear — six sample columns along the line to the
        // target, three blocks of headroom each. MC tests canBeReplaced();
        // "not solid" is this engine's nearest equivalent (grass and water
        // pass, stone fails).
        const IBlockAccess* blocks = fox.Level() ? fox.Level()->Blocks() : nullptr;
        if (!blocks) return false;

        const double zdiff = target.position.z - fox.position.z;
        const double xdiff = target.position.x - fox.position.x;
        const double slope = xdiff != 0.0 ? zdiff / xdiff : 0.0;

        for (int i = 0; i < 6; ++i) {
            const double z = slope == 0.0 ? 0.0
                                          : zdiff * (static_cast<double>(i) / 6.0);
            const double x = slope == 0.0 ? xdiff * (static_cast<double>(i) / 6.0)
                                          : z / slope;
            for (int j = 1; j < 4; ++j) {
                const int bx = static_cast<int>(std::floor(fox.position.x + x));
                const int by = static_cast<int>(std::floor(fox.position.y + j));
                const int bz = static_cast<int>(std::floor(fox.position.z + z));
                if (blocks->IsBlockSolid(bx, by, bz)) return false;
            }
        }
        return true;
    }

    void Fox::Tick() {
        Animal::Tick();

        if (IsEffectiveAi()) {
            const bool inWater = IsInWater();
            if (inWater || GetTarget() != nullptr
                || (m_level && m_level->IsThundering())) {
                WakeUp();
            }
            if (inWater || IsSleeping()) SetSitting(false);

            // MC: a faceplanted fox (nose in the snow) kicks up the block's
            // break puff — levelEvent 2001 — one tick in five.
            if (IsFaceplanted() && m_level && !m_level->IsClientSide() && m_level->Random().NextFloat() < 0.2f &&
                m_level->Blocks()) {
                const glm::ivec3 pos = BlockPosition();
                const BlockState state = m_level->Blocks()->GetBlockState(pos.x, pos.y, pos.z);
                PlayLevelEventSound(*m_level, nullptr, LevelEvent::PARTICLES_DESTROY_BLOCK, pos,
                                    static_cast<int>(state.RawId()), &m_level->Random());
            }
        }

        // The two client-visible ramps run on BOTH sides, as in MC.
        m_interestedAngleO = m_interestedAngle;
        if (IsInterested()) {
            m_interestedAngle += (1.0f - m_interestedAngle) * 0.4f;
        } else {
            m_interestedAngle += (0.0f - m_interestedAngle) * 0.4f;
        }

        m_crouchAmountO = m_crouchAmount;
        if (IsFoxCrouching()) {
            m_crouchAmount += 0.2f;
            if (m_crouchAmount > 3.0f) m_crouchAmount = 3.0f;
        } else {
            m_crouchAmount = 0.0f;
        }
    }

    void Fox::AiStep() {
        // As the wolf's beg tilt (Wolf::AiStep): a restored interested flag
        // has no goal behind it — cleared before the goals run again.
        if (m_restoredInterest && m_level && !m_level->IsClientSide()) SetIsInterested(false);
        m_restoredInterest = false;
        if (m_level && !m_level->IsClientSide() && IsAlive() && IsEffectiveAi()) {
            // MC's mouth-item eating clock: a held food is eaten (finish-
            // UsingItem — its effects, sound and crumbs) once 600 ticks pass,
            // with a 10% chance per tick from 560 of the eating sound and
            // event 45's crumbs.
            ++m_ticksSinceEaten;
            const ItemStack itemInMouth = GetEquipment(EquipmentSlot::MAINHAND);
            if (CanEat(itemInMouth)) {
                if (m_ticksSinceEaten > 600) {
                    ItemStack eating = itemInMouth;
                    const ItemStack remainingFood = ConsumableBehavior::FinishUsingForLiving(*this, eating);
                    // MC sets the slot only when something remains; the
                    // consumed stack itself is the slot's own stack there,
                    // so an eaten last item empties it either way.
                    SetEquipment(EquipmentSlot::MAINHAND, remainingFood);
                    m_ticksSinceEaten = 0;
                } else if (m_ticksSinceEaten > 560 && m_level->Random().NextFloat() < 0.1f) {
                    PlaySound(SoundEvents::FOX_EAT, 1.0f, 1.0f);   // playEatingSound
                    m_level->BroadcastEntityEvent(*this, 45);
                }
            }

            LivingEntity* target = GetTarget();
            if (!target || !target->IsAlive()) {
                SetIsCrouching(false);
                SetIsInterested(false);
            }
        }

        if (IsSleeping() || IsImmobile()) {
            jumping = false;
            xxa = 0.0f;
            zza = 0.0f;
        }

        Animal::AiStep();
        if (IsDefending() && m_level && m_level->Random().NextFloat() < 0.05f) {
            PlaySound(SoundEvents::FOX_AGGRO, 1.0f, 1.0f);
        }
    }

    bool Fox::IsConsumableFood(const ItemStack& stack) {
        return !stack.IsEmpty() && stack.get(DataComponents::FOOD).has_value() &&
               stack.get(DataComponents::CONSUMABLE).has_value();
    }

    bool Fox::CanEat(const ItemStack& itemInMouth) const {
        // MC canEat: a consumable food, no target, on the ground, awake.
        return IsConsumableFood(itemInMouth) && GetTarget() == nullptr && onGround && !IsSleeping();
    }

    bool Fox::CanHoldItem(const ItemStack& stack) const {
        const ItemStack& held = GetEquipment(EquipmentSlot::MAINHAND);
        return held.IsEmpty() ||
               (m_ticksSinceEaten > 0 && IsConsumableFood(stack) && !IsConsumableFood(held));
    }

    void Fox::SpitOutItem(const ItemStack& stack) {
        // MC spitOutItem: an ItemEntity one look-vector ahead and a block up
        // (ItemEntity(level, x, y, z, stack): the default scatter velocity),
        // a 40-tick pickup delay, the spit sound.
        if (stack.IsEmpty() || !m_level || m_level->IsClientSide()) return;
        const float f = xRot * Mth::kDegToRad;
        const float g = -yRot * Mth::kDegToRad;
        const glm::dvec3 look(std::sin(g) * std::cos(f), -std::sin(f), std::cos(g) * std::cos(f));
        JavaRandom& r = m_level->Random();
        const glm::dvec3 velocity(r.NextDouble() * 0.2 - 0.1, 0.2, r.NextDouble() * 0.2 - 0.1);
        PlaySound(SoundEvents::FOX_SPIT, 1.0f, 1.0f);
        m_level->SpawnThrownItem(glm::dvec3(position.x + look.x, position.y + 1.0, position.z + look.z),
                                 velocity, stack, 40);
    }

    void Fox::DropItemStack(const ItemStack& stack) {
        // MC dropItemStack: a plain ItemEntity at the fox — the
        // constructor's scatter velocity and no pickup delay.
        if (stack.IsEmpty() || !m_level || m_level->IsClientSide()) return;
        JavaRandom& r = m_level->Random();
        const glm::dvec3 velocity(r.NextDouble() * 0.2 - 0.1, 0.2, r.NextDouble() * 0.2 - 0.1);
        m_level->SpawnThrownItem(position, velocity, stack, 0);
    }

    void Fox::PickUpItem(int32_t itemEntityId, const ItemStack& stack) {
        if (!CanHoldItem(stack) || !m_level) return;
        // All but one of a stack stays behind as its own drop (split(count
        // - 1)), the old mouth item is spat out, one goes in the mouth as a
        // guaranteed drop, and the item entity is used up.
        if (stack.count > 1) {
            ItemStack rest = stack;
            rest.count = stack.count - 1;
            DropItemStack(rest);
        }
        SpitOutItem(GetEquipment(EquipmentSlot::MAINHAND));
        OnItemPickup(itemEntityId, stack);
        ItemStack one = stack;
        one.count = 1;
        SetEquipment(EquipmentSlot::MAINHAND, one);
        SetGuaranteedDrop(EquipmentSlot::MAINHAND);
        TakeItemEntity(itemEntityId, stack.count);   // take + discard
        m_ticksSinceEaten = 0;
    }

    void Fox::PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty) {
        (void)difficulty;
        // MC Fox.populateDefaultEquipmentSlots, verbatim odds.
        if (!(random.NextFloat() < 0.2f)) return;
        const float odds = random.NextFloat();
        ItemID held;
        if (odds < 0.05f)      held = Items::Emerald;
        else if (odds < 0.2f)  held = Items::Egg;
        else if (odds < 0.4f)  held = random.NextBool() ? Items::RabbitFoot : Items::RabbitHide;
        else if (odds < 0.6f)  held = Items::Wheat;
        else if (odds < 0.8f)  held = Items::Leather;
        else                   held = Items::Feather;
        SetEquipment(EquipmentSlot::MAINHAND, ItemStack(held, 1));
    }

    void Fox::HandleEntityEvent(uint8_t id) {
        if (id == 45) {
            // MC: eight ITEM crumbs of the mouth item from just ahead of the
            // fox at its feet height, flung by the look rotation.
            const ItemStack& mouthItem = GetEquipment(EquipmentSlot::MAINHAND);
            if (!mouthItem.IsEmpty() && m_level) {
                JavaRandom& r = m_level->Random();
                const ParticleOptions particle = ParticleOptions::Item(mouthItem.itemId);
                const double xa = -static_cast<double>(xRot) * 0.017453292;
                const double ya = -static_cast<double>(yRot) * 0.017453292;
                const double xc = std::cos(xa), xs = std::sin(xa), yc = std::cos(ya), ys = std::sin(ya);
                const float f = xRot * Mth::kDegToRad;
                const float g = -yRot * Mth::kDegToRad;
                const double lookX = std::sin(g) * std::cos(f), lookZ = std::cos(g) * std::cos(f);
                for (int i = 0; i < 8; ++i) {
                    glm::dvec3 v((static_cast<double>(r.NextFloat()) - 0.5) * 0.1,
                                 static_cast<double>(r.NextFloat()) * 0.1 + 0.1, 0.0);
                    v = glm::dvec3(v.x, v.y * xc + v.z * xs, v.z * xc - v.y * xs);        // xRot
                    v = glm::dvec3(v.x * yc + v.z * ys, v.y, v.z * yc - v.x * ys);        // yRot
                    m_level->AddParticle(particle, position.x + lookX / 2.0, position.y,
                                         position.z + lookZ / 2.0, v.x, v.y + 0.05, v.z);
                }
            }
            return;
        }
        Animal::HandleEntityEvent(id);
    }

    void Fox::DropEquipment(EntityLevel& level) {
        // MC Fox.dropAllDeathLoot: the mouth item always comes down
        // (spawnAtLocation), outside the mob_drops gate.
        const ItemStack held = GetEquipment(EquipmentSlot::MAINHAND);
        if (!held.IsEmpty()) {
            DropItemStackAt(level.Dimension(), position, held);
            SetEquipment(EquipmentSlot::MAINHAND, ItemStack{});
        }
        Animal::DropEquipment(level);
    }

    const char* Fox::GetAmbientSound() const {
        // MC Fox.getAmbientSound: asleep it snores; at night, 1 in 10 calls
        // screech when no player is within 16 blocks. (isBrightOutside is
        // skyDarken < 4; foxes live in the overworld, which has no fixed time.)
        if (IsSleeping()) return SoundEvents::FOX_SLEEP;
        if (m_level && m_level->GetSkyDarken() >= 4 && m_level->Random().NextFloat() < 0.1f) {
            const LivingEntity* player = m_level->GetNearestPlayer(position.x, position.y, position.z, -1.0);
            AABB box = GetAABB();
            box.min -= glm::vec3(16.0f);
            box.max += glm::vec3(16.0f);
            if (!player || !box.Intersects(player->GetAABB())) return SoundEvents::FOX_SCREECH;
        }
        return SoundEvents::FOX_AMBIENT;
    }

    void Fox::PlayAmbientSound() {
        // MC Fox.playAmbientSound: the screech carries (volume 2).
        const char* ambient = GetAmbientSound();
        if (ambient == SoundEvents::FOX_SCREECH) {
            PlaySound(ambient, 2.0f, GetVoicePitch());
        } else {
            MakeSound(ambient);
        }
    }

    std::shared_ptr<SpawnGroupData>
    Fox::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Fox.finalizeSpawn: variant by biome, third-and-later pack
        // members spawn as cubs, then the variant-ordered target goals.
        Variant variant = Variant::Red;
        if (m_level) {
            if (const IBlockAccess* blocks = m_level->Blocks()) {
                const glm::ivec3 p = BlockPosition();
                const std::string_view biome =
                    BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name;
                for (std::string_view b : kSnowFoxBiomes) {
                    if (b == biome) { variant = Variant::Snow; break; }
                }
            }
        }

        bool isBaby = false;
        if (auto* foxData = dynamic_cast<FoxGroupData*>(groupData.get())) {
            variant = foxData->variant;
            if (foxData->size >= 2) isBaby = true;
            ++foxData->size;
        } else {
            auto data = std::make_shared<FoxGroupData>(variant);
            ++data->size;
            groupData = std::move(data);
        }

        SetVariant(variant);
        if (isBaby) SetAge(kBabyStartAge);
        SetTargetGoals();

        // populateDefaultEquipmentSlots — the mouth trinket — then super.
        if (m_level && !m_level->IsClientSide()) {
            PopulateDefaultEquipmentSlots(m_level->Random(), CurrentDifficulty());
        }
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    // ── Turtle ─────────────────────────────────────────────────────────────

    void Turtle::CreateAttributes(AttributeMap& out) {
        // MC Turtle.createAttributes: MAX_HEALTH 30, MOVEMENT_SPEED 0.25,
        // STEP_HEIGHT 1.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,    30.0);
        out.Register(Attribute::MovementSpeed, 0.25);
        out.Register(Attribute::StepHeight,    1.0);
    }

    Turtle::Turtle(EntityLevel* level) : Animal(EntityTypeId::Turtle, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();

        // MC's constructor wiring: water is free, doors are walls, the
        // turtle's own move control, and the amphibious navigation. (MC's
        // TurtlePathNavigation only adds an isStableDestination tweak that
        // accepts open water while a travelPos is set; the shared amphibious
        // navigation's destination test already accepts water columns.)
        SetPathfindingMalus(PathType::Water, 0.0f);
        SetPathfindingMalus(PathType::DoorIronClosed, -1.0f);
        SetPathfindingMalus(PathType::DoorWoodClosed, -1.0f);
        SetPathfindingMalus(PathType::DoorOpen, -1.0f);
        SetMoveControl(MakeTurtleMoveControl(this));
        SetNavigation(std::make_unique<AmphibiousPathNavigation>(this, level));

        RegisterGoals();
    }

    bool Turtle::IsFood(uint32_t itemId) const {
        // MC ItemTags.TURTLE_FOOD: seagrass — a block item, resolved by slug.
        static const ItemID kSeagrass = RecipeManager::ItemFromSlug("seagrass");
        return itemId == kSeagrass;
    }

    // ── SkeletonHorse: the skeleton trap ───────────────────────────────────

    namespace {

        // MC SkeletonTrapGoal.
        class SkeletonTrapGoal : public Goal {
        public:
            explicit SkeletonTrapGoal(SkeletonHorse* horse) : m_horse(horse) {}

            // level.hasNearbyAlivePlayer(x, y, z, 10): a live non-spectator
            // player within 10 blocks. (IsTrap: the goal lingers until the
            // horse's next AiStep removes it — see SkeletonHorse::SetTrap.)
            bool CanUse() override {
                EntityLevel* level = m_horse->Level();
                if (!level || level->IsClientSide() || !m_horse->IsTrap()) return false;
                std::vector<LivingEntity*> players;
                level->GetPlayers(players);
                for (const LivingEntity* p : players) {
                    if (!p || !p->IsAlive() || p->IsSpectator()) continue;
                    const glm::dvec3 d = p->position - m_horse->position;
                    if (glm::dot(d, d) < 100.0) return true;
                }
                return false;
            }

            void Tick() override {
                EntityLevel* level = m_horse->Level();
                if (!level || !m_horse->IsTrap()) return;
                const DifficultyInstance difficulty = level->GetCurrentDifficultyAt(m_horse->BlockPosition());
                m_horse->SetTrap(false);
                m_horse->SetTamedHorse(true);
                m_horse->SetAge(0);

                // The visual-only bolt on the horse.
                auto bolt = std::make_unique<LightningBolt>(level);
                bolt->position = bolt->oldPosition = m_horse->position;
                bolt->SetVisualOnly(true);
                level->AddFreshEntity(std::move(bolt));

                Skeleton* rider = CreateSkeleton(*level, difficulty, *m_horse);
                if (!rider) return;
                rider->StartRiding(*m_horse, /*force=*/false);
                for (int i = 0; i < 3; ++i) {
                    SkeletonHorse* otherHorse = CreateHorse(*level, difficulty);
                    if (!otherHorse) continue;
                    Skeleton* otherSkeleton = CreateSkeleton(*level, difficulty, *otherHorse);
                    if (!otherSkeleton) continue;
                    otherSkeleton->StartRiding(*otherHorse, /*force=*/false);
                    // otherHorse.push(triangle(0, 1.1485), 0, triangle(0, 1.1485))
                    // from the trap horse's random.
                    JavaRandom& rng = level->Random();
                    const double px = rng.Triangle(0.0, 1.1485);
                    const double pz = rng.Triangle(0.0, 1.1485);
                    otherHorse->velocity += glm::dvec3(px, 0.0, pz);
                }
            }

            const char* Name() const override { return "SkeletonTrapGoal"; }

        private:
            // createHorse: a finalized TRIGGERED skeleton horse at the trap,
            // 60 ticks invulnerable, persistent, tamed, adult.
            SkeletonHorse* CreateHorse(EntityLevel& level, const DifficultyInstance&) {
                auto horse = std::make_unique<SkeletonHorse>(&level);
                horse->position = horse->oldPosition = m_horse->position;
                horse->FinalizeSpawn(SpawnReason::Triggered, nullptr);
                horse->SetInvulnerableTime(60);
                horse->SetPersistenceRequired(true);
                horse->SetTamedHorse(true);
                horse->SetAge(0);
                SkeletonHorse* placed = horse.get();
                level.AddFreshEntity(std::move(horse));
                return placed;
            }

            // createSkeleton: a finalized TRIGGERED skeleton on `horse`,
            // 60 ticks invulnerable, persistent, an iron helmet if it has no
            // head piece, the weapon and helmet enchanted.
            Skeleton* CreateSkeleton(EntityLevel& level, const DifficultyInstance& difficulty, const AbstractHorse& horse) {
                auto skeleton = std::make_unique<Skeleton>(&level);
                skeleton->position = skeleton->oldPosition = horse.position;
                skeleton->FinalizeSpawn(SpawnReason::Triggered, nullptr);
                skeleton->SetInvulnerableTime(60);
                skeleton->SetPersistenceRequired(true);
                if (skeleton->GetEquipment(EquipmentSlot::HEAD).IsEmpty()) {
                    skeleton->SetEquipment(EquipmentSlot::HEAD, ItemStack(Items::IronHelmet, 1));
                }
                Enchant(*skeleton, EquipmentSlot::MAINHAND, difficulty);
                Enchant(*skeleton, EquipmentSlot::HEAD, difficulty);
                Skeleton* placed = skeleton.get();
                level.AddFreshEntity(std::move(skeleton));
                return placed;
            }

            // enchant: the stack's enchantments cleared, then
            // EnchantmentHelper.enchantItemFromProvider(MOB_SPAWN_EQUIPMENT) —
            // by_cost_with_difficulty over #on_mob_spawn_equipment, min_cost
            // 5, max_cost_span 17 — with no chance roll.
            static void Enchant(Skeleton& skeleton, EquipmentSlot slot, const DifficultyInstance& difficulty) {
                ItemStack stack = skeleton.GetEquipment(slot);
                if (stack.IsEmpty()) return;
                EnchantmentHelper::SetEnchantments(stack, ItemEnchantments{});
                EntityLevel* level = skeleton.Level();
                if (!level) return;
                JavaRandom& random = level->Random();
                static const std::vector<EnchantmentId> kCandidates =
                    EnchantmentDefinitions::ResolveTagOrdered("minecraft:on_mob_spawn_equipment");
                constexpr int kMinCost = 5, kMaxCostSpan = 17;
                const int maxCost = kMinCost +
                    static_cast<int>(difficulty.GetSpecialMultiplier() * static_cast<float>(kMaxCostSpan));
                const int cost = random.NextInt(maxCost - kMinCost + 1) + kMinCost;
                for (const EnchantmentInstance& instance :
                     EnchantmentHelper::SelectEnchantment(random, stack, cost, kCandidates)) {
                    EnchantmentHelper::Enchant(stack, instance.id, instance.level);
                }
                skeleton.SetEquipment(slot, stack);
            }

            SkeletonHorse* m_horse;
        };

    } // namespace

    void SkeletonHorse::SetTrap(bool trap) {
        if (trap == m_isTrap) return;
        m_isTrap = trap;
        if (trap) {
            if (m_trapGoalRemovalPending && m_trapGoal) {
                // Re-trapped before the pending removal ran: keep the goal.
                m_trapGoalRemovalPending = false;
                return;
            }
            auto goal = std::make_unique<SkeletonTrapGoal>(this);
            m_trapGoal = goal.get();
            m_goalSelector.AddGoal(1, std::move(goal));
        } else if (m_trapGoal) {
            m_trapGoalRemovalPending = true;
        }
    }

    void SkeletonHorse::AiStep() {
        // The goal's removal, deferred out of its own tick (see the header).
        if (m_trapGoalRemovalPending) {
            m_trapGoalRemovalPending = false;
            if (m_trapGoal) m_goalSelector.RemoveGoal(m_trapGoal);
            m_trapGoal = nullptr;
        }
        AbstractHorse::AiStep();
        // MC SkeletonHorse.aiStep: an unsprung, non-persistent trap despawns
        // after TRAP_MAX_LIFE ticks.
        if (m_level && !m_level->IsClientSide() && !IsPersistenceRequired() && m_isTrap &&
            m_trapTime++ >= kTrapMaxLife) {
            Discard();
        }
    }

    std::unique_ptr<Animal> Turtle::CreateBaby() {
        return std::make_unique<Turtle>(m_level);
    }

    void Turtle::DropCustomDeathLoot(EntityLevel& level) {
        if (GetLastDamageSource() == MobDamageSource::Lightning) {
            level.SpawnItemDrop(position, Items::Bowl, 1);
        }
    }

    void Turtle::ThunderHit(Entity* /*bolt*/) {
        // MC Turtle.thunderHit: hurtServer(lightningBolt, Float.MAX_VALUE).
        if (!m_level || m_level->IsClientSide()) return;
        Hurt(MobDamageSource::Lightning, std::numeric_limits<float>::max(), nullptr);
    }

    const char* Turtle::GetAmbientSound() const {
        // MC Turtle.getAmbientSound: an adult ashore grunts; otherwise the
        // generated row (none).
        if (!IsInWater() && onGround && !IsBaby()) return SoundEvents::TURTLE_AMBIENT_LAND;
        return Animal::GetAmbientSound();
    }

    bool Turtle::IsSandBlock(BlockID id) {
        // MC BlockTags.SAND, reduced to the blocks this engine defines.
        return id == BlockID::Sand || id == BlockID::RedSand
            || id == BlockID::SuspiciousSand;
    }

    bool Turtle::IsBabyOnLand(const LivingEntity& e) {
        // MC Turtle.BABY_ON_LAND_SELECTOR.
        return e.IsBaby() && !e.IsInWater();
    }

    bool Turtle::CanMate(const Animal& other) const {
        // MC canFallInLove: !hasEgg — folded into the mate test because the
        // port's love entry points do not consult a canFallInLove hook.
        if (m_hasEgg) return false;
        if (const auto* turtle = dynamic_cast<const Turtle*>(&other)) {
            if (turtle->HasEgg()) return false;
        }
        return Animal::CanMate(other);
    }

    void Turtle::SpawnChildFromBreeding(Animal& partner) {
        // MC TurtleBreedGoal.breed: no baby spawns — the goal's own turtle
        // becomes gravid and both parents cool down; the love cause gets
        // BRED_ANIMALS with no child.
        const int32_t feeder = GetLoveCauseId() != -1 ? GetLoveCauseId()
                                                      : partner.GetLoveCauseId();
        if (m_level && !m_level->IsClientSide() && feeder != -1) {
            if (Server::ServerPlayer* player = Server::CriteriaTriggers::PlayerOf(m_level->ResolveEntityById(feeder))) {
                Server::CriteriaTriggers::BredAnimals(*player, *this, partner, nullptr);
            }
        }
        SetHasEgg(true);
        SetAge(kParentAgeAfterBreeding);
        partner.SetAge(kParentAgeAfterBreeding);
        ResetLove();
        partner.ResetLove();

        // MC TurtleBreedGoal.breed (Turtle.java:462): new ExperienceOrb(...,
        // random.nextInt(7) + 1) — turtles pay breeding XP when they turn
        // gravid, not when the egg hatches.
        if (m_level && !m_level->IsClientSide()) {
            m_level->AwardExperience(position, m_level->Random().NextInt(7) + 1,
                                     feeder);
        }
    }

    float Turtle::GetWalkTargetValue(const glm::ivec3& pos) const {
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return 0.0f;
        // MC: water scores 10 (unless heading home), sand scores 10,
        // everything else the light cost.
        if (!m_goingHome && blocks->GetBlock(pos.x, pos.y, pos.z) == BlockID::Water) {
            return 10.0f;
        }
        if (IsSandBlock(blocks->GetBlock(pos.x, pos.y - 1, pos.z))) {
            return 10.0f;
        }
        return PathfindingCostFromLightLevels(*m_level, pos.x, pos.y, pos.z);
    }

    std::shared_ptr<SpawnGroupData>
    Turtle::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Turtle.finalizeSpawn: home is where you spawned.
        SetHomePos(BlockPosition());
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    void Turtle::RegisterGoals() {
        // MC Turtle.registerGoals, priority for priority.
        m_goalSelector.AddGoal(0, std::make_unique<TurtlePanicGoal>(this, 1.2));
        m_goalSelector.AddGoal(1, std::make_unique<TurtleBreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(1, std::make_unique<TurtleLayEggGoal>(this, 1.0));
        m_goalSelector.AddGoal(2, std::make_unique<TemptGoal>(this, 1.1, false));
        m_goalSelector.AddGoal(3, std::make_unique<TurtleGoToWaterGoal>(this, 1.0));
        m_goalSelector.AddGoal(4, std::make_unique<TurtleGoHomeGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<TurtleTravelGoal>(this, 1.0));
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(9, std::make_unique<TurtleRandomStrollGoal>(
                                      this, 1.0, 100));
    }

    // ── Panda ──────────────────────────────────────────────────────────────

    namespace {

        // The nearest ItemID for ItemTags.PANDA_FOOD (bamboo) — a block item.
        ItemID BambooItem() {
            static const ItemID kId = RecipeManager::ItemFromSlug("bamboo");
            return kId;
        }

    } // namespace

    Panda::Gene Panda::RandomGene(JavaRandom& rng) {
        // MC Panda.Gene.getRandom, verbatim.
        const int roll = rng.NextInt(16);
        if (roll == 0) return Gene::Lazy;
        if (roll == 1) return Gene::Worried;
        if (roll == 2) return Gene::Playful;
        if (roll == 4) return Gene::Aggressive;
        if (roll < 9)  return Gene::Weak;
        if (roll < 11) return Gene::Brown;
        return Gene::Normal;
    }

    void Panda::CreateAttributes(AttributeMap& out) {
        // MC Panda.createAttributes: MOVEMENT_SPEED 0.15, ATTACK_DAMAGE 6.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.15);
        out.Register(Attribute::AttackDamage,  6.0);
    }

    Panda::Panda(EntityLevel* level) : Animal(EntityTypeId::Panda, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor wiring: the panda's own move control, and
        // setCanPickUpLoot(true) for an adult (every panda is one while its
        // constructor runs).
        SetMoveControl(MakePandaMoveControl(this));
        if (!IsBaby()) SetCanPickUpLoot(true);
        RegisterGoals();
    }

    bool Panda::IsFood(uint32_t itemId) const {
        return itemId == BambooItem();   // ItemTags.PANDA_FOOD
    }

    std::unique_ptr<Animal> Panda::CreateBaby() {
        // Genes come from SpawnChildFromBreeding below, which knows both
        // parents; a bare CreateBaby (no partner) inherits per MC's
        // single-parent branch.
        auto baby = std::make_unique<Panda>(m_level);
        baby->SetGeneFromParents(*this, nullptr);
        baby->ApplyGeneAttributes();
        baby->m_health = baby->GetMaxHealth();
        return baby;
    }

    void Panda::SetGeneFromParents(const Panda& parent1, const Panda* parent2) {
        // MC Panda.setGeneFromParents, verbatim including the 1/32 mutations.
        JavaRandom& rng = m_level->Random();
        const auto oneOf = [&rng](const Panda& p) {
            return rng.NextBool() ? p.GetMainGene() : p.GetHiddenGene();
        };

        if (parent2 == nullptr) {
            if (rng.NextBool()) {
                SetMainGene(oneOf(parent1));
                SetHiddenGene(RandomGene(rng));
            } else {
                SetMainGene(RandomGene(rng));
                SetHiddenGene(oneOf(parent1));
            }
        } else if (rng.NextBool()) {
            SetMainGene(oneOf(parent1));
            SetHiddenGene(oneOf(*parent2));
        } else {
            SetMainGene(oneOf(*parent2));
            SetHiddenGene(oneOf(parent1));
        }

        if (rng.NextInt(32) == 0) SetMainGene(RandomGene(rng));
        if (rng.NextInt(32) == 0) SetHiddenGene(RandomGene(rng));
    }

    void Panda::ApplyGeneAttributes() {
        // MC Panda.setAttributes: weak pandas cap at 10 health, lazy ones
        // crawl at 0.07.
        if (IsWeak()) m_attributes.Register(Attribute::MaxHealth, 10.0);
        if (IsLazy()) m_attributes.Register(Attribute::MovementSpeed, 0.07);
    }

    void Panda::SpawnChildFromBreeding(Animal& partner) {
        // Animal::SpawnChildFromBreeding's body with the two-parent gene
        // pass threaded through — CreateBaby alone cannot see the partner.
        auto baby = std::make_unique<Panda>(m_level);
        baby->SetGeneFromParents(*this, dynamic_cast<Panda*>(&partner));
        baby->ApplyGeneAttributes();
        baby->m_health = baby->GetMaxHealth();

        baby->SetAge(kBabyStartAge);
        baby->position = position;
        baby->yRot = yRot;
        baby->yHeadRot = yRot;
        baby->yBodyRot = yRot;

        const int32_t feeder = GetLoveCauseId() != -1 ? GetLoveCauseId()
                                                      : partner.GetLoveCauseId();
        SetAge(kParentAgeAfterBreeding);
        partner.SetAge(kParentAgeAfterBreeding);
        ResetLove();
        partner.ResetLove();

        // finalizeSpawnChildFromBreeding: BRED_ANIMALS for the love cause.
        if (m_level && !m_level->IsClientSide() && feeder != -1) {
            if (Server::ServerPlayer* player = Server::CriteriaTriggers::PlayerOf(m_level->ResolveEntityById(feeder))) {
                Server::CriteriaTriggers::BredAnimals(*player, *this, partner, baby.get());
            }
        }

        if (m_level) m_level->AddFreshEntity(std::move(baby));

        // MC Animal.finalizeSpawnChildFromBreeding (Animal.java:224-226) —
        // the panda path pays the same 1..7 breeding XP as the base body
        // this override transcribes.
        if (m_level && !m_level->IsClientSide()) {
            m_level->AwardExperience(position, m_level->Random().NextInt(7) + 1,
                                     feeder);
        }
    }

    bool Panda::IsScared() const {
        // The worried-in-thunder verdict is the server's; the client reads
        // the synced bit (its level always answers "not thundering").
        if (m_level && m_level->IsClientSide()) return m_clientScared;
        return IsWorried() && m_level && m_level->IsThundering();
    }

    void Panda::TryToSit() {
        // MC Panda.tryToSit.
        if (!IsInWater()) {
            zza = 0.0f;
            GetNavigation().Stop();
            Sit(true);
        }
    }

    bool Panda::DoHurtTarget(Entity& target) {
        // MC Panda.doHurtTarget: a non-aggressive panda regrets the bite.
        if (!IsAggressiveGene()) m_didBite = true;
        return Animal::DoHurtTarget(target);
    }

    void Panda::PlayAttackSound() {
        // MC Panda.playAttackSound.
        PlaySound(SoundEvents::PANDA_BITE, 1.0f, 1.0f);
    }

    const char* Panda::GetAmbientSound() const {
        // MC Panda.getAmbientSound.
        if (IsAggressive()) return SoundEvents::PANDA_AGGRESSIVE_AMBIENT;
        return IsWorried() ? SoundEvents::PANDA_WORRIED_AMBIENT : SoundEvents::PANDA_AMBIENT;
    }

    UseResult Panda::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Panda.mobInteract, verbatim shape — pandas never take the
        // Animal base path (every non-food click returns PASS).
        if (IsScared()) return UseResult::Pass;

        if (IsOnBack()) {
            SetOnBack(false);
            return UseResult::Success;
        }

        // MC Panda.mobInteract's tail: a non-food click PASSes — except a
        // golden dandelion on a cub, which goes to AgeableMob's toggle.
        if (!IsFood(held.itemId)) {
            return IsBaby() && CanUseGoldenDandelion(held, true, GetAgeLockParticleTimer(), *this)
                       ? AgeableMob::MobInteract(player, held)
                       : UseResult::Pass;
        }

        // MC: feeding a panda that has a grudge target sets gotBamboo — the
        // stand-down flag PandaHurtByTargetGoal reads.
        if (GetTarget() != nullptr) m_gotBamboo = true;

        const int age = GetAge();
        const bool clientSide = m_level && m_level->IsClientSide();
        if (IsBaby()) {
            // An age-locked cub takes no bamboo (nothing to grow), and the
            // click is not swallowed either — MC's isBaby branch there is
            // PASS, and a PASS is what lets the flower below be reached.
            if (!CanAgeUp()) return UseResult::Pass;
            UsePlayerItem(held);
            AgeUp(GetSpeedUpSecondsWhenFeeding(-age), /*forced=*/true);
        } else if (!clientSide && age == 0 && CanFallInLove()) {
            UsePlayerItem(held);
            SetInLove(&player);
        } else {
            if (clientSide) return UseResult::Pass;
            if (IsSitting() || IsInWater()) return UseResult::Pass;
            // Sit, start chewing, drop what the paw held (unless the feeder
            // has infinite materials), and hold one of the fed item.
            TryToSit();
            Eat(true);
            const ItemStack current = GetEquipment(EquipmentSlot::MAINHAND);
            if (!current.IsEmpty() && !player.IsCreative() && m_level) {
                DropItemStackAt(m_level->Dimension(), position, current);   // spawnAtLocation
            }
            SetEquipment(EquipmentSlot::MAINHAND, ItemStack(held.itemId, 1));
            UsePlayerItem(held);
        }

        return UseResult::SuccessServer;
    }

    bool Panda::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Panda.hurtServer: a hit panda stops sitting.
        Sit(false);
        return Animal::Hurt(source, amount, attacker);
    }

    void Panda::Tick() {
        Animal::Tick();

        const bool serverSide = m_level && !m_level->IsClientSide();

        if (serverSide && IsWorried()) {
            // MC: worried pandas sit out thunderstorms, and stop chewing.
            if (m_level->IsThundering() && !IsInWater()) {
                Sit(true);
                Eat(false);
            } else if (!IsEatingPanda()) {
                Sit(false);
            }
        }

        if (serverSide && GetTarget() == nullptr) {
            // MC: gotBamboo/didBite clear once the grudge target is gone.
            m_gotBamboo = false;
            m_didBite = false;
        }

        if (m_unhappyCounter > 0) {
            if (GetTarget()) {
                LivingEntity* target = GetTarget();
                GetLookControl().SetLookAt(target->position.x,
                                           target->GetEyeY(),
                                           target->position.z, 90.0f, 90.0f);
            }
            if (m_unhappyCounter == 29 || m_unhappyCounter == 14) {
                PlaySound(SoundEvents::PANDA_CANT_BREED, 1.0f, 1.0f);
            }
            --m_unhappyCounter;
        }

        // The sneeze clock runs on BOTH sides (the client's flag comes off
        // the anim byte); the client copy draws the SNEEZE puff.
        if (IsSneezing()) {
            ++m_sneezeCounter;
            if (m_sneezeCounter > 20) {
                Sneeze(false);
                // MC afterSneeze: the SNEEZE puff in front of the snout,
                // carried by the panda's motion.
                if (m_level) {
                    const double bodyRad = static_cast<double>(yBodyRot) * 0.017453292;
                    const double reach = (static_cast<double>(GetBbWidth()) + 1.0) * 0.5;
                    m_level->AddParticle(ParticleKind::Sneeze, position.x - reach * std::sin(bodyRad),
                                         GetEyeY() - 0.10000000149011612, position.z + reach * std::cos(bodyRad),
                                         velocity.x, 0.0, velocity.z);
                }
                // MC afterSneeze: the sneeze itself.
                PlaySound(SoundEvents::PANDA_SNEEZE, 1.0f, 1.0f);
                if (serverSide) {
                    // MC afterSneeze: startle every grounded adult panda
                    // within 10 blocks into a hop.
                    AABB box = GetAABB();
                    box.min -= glm::vec3(10.0f);
                    box.max += glm::vec3(10.0f);
                    std::vector<Entity*> nearby;
                    m_level->GetEntitiesInBox(box, this, nearby);
                    for (Entity* e : nearby) {
                        auto* panda = dynamic_cast<Panda*>(e);
                        if (!panda || panda->IsBaby() || !panda->onGround) continue;
                        if (panda->IsInWater() || !panda->CanPerformAction()) continue;
                        panda->JumpFromGround();
                    }
                    // MC afterSneeze: mob_drops on → the panda_sneeze gift.
                    if (Rules::GetBool(Rules::Id::MobDrops)) DropSneezeGift();
                }
            } else if (m_sneezeCounter == 1) {
                PlaySound(SoundEvents::PANDA_PRE_SNEEZE, 1.0f, 1.0f);
            }
        }

        if (IsRolling()) {
            HandleRoll();
        } else {
            m_rollCounter = 0;
        }

        if (IsSitting()) xRot = 0.0f;

        UpdateRamps();
        HandleEating();
    }

    bool Panda::CanPickUpAndEat(const ItemStack& stack) {
        return !stack.IsEmpty() &&
               DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(stack.itemId),
                                "minecraft:panda_eats_from_ground");
    }

    void Panda::PickUpItem(int32_t itemEntityId, const ItemStack& stack) {
        if (!GetEquipment(EquipmentSlot::MAINHAND).IsEmpty() || !CanPickUpAndEat(stack)) return;
        OnItemPickup(itemEntityId, stack);
        SetEquipment(EquipmentSlot::MAINHAND, stack);
        SetGuaranteedDrop(EquipmentSlot::MAINHAND);
        TakeItemEntity(itemEntityId, stack.count);   // take + discard
    }

    void Panda::HandleEating() {
        // MC Panda.handleEating, both sides (the client's counter runs off
        // the synced isEating bit): a sitting, unscared panda holding food
        // starts chewing 1 tick in 80; an empty paw or standing up stops it.
        const bool holding = !GetEquipment(EquipmentSlot::MAINHAND).IsEmpty();
        JavaRandom* random = m_level ? &m_level->Random() : nullptr;
        if (!random) return;
        const bool serverSide = !m_level->IsClientSide();
        if (!IsEatingPanda() && IsSitting() && !IsScared() && holding && random->NextInt(80) == 1) {
            Eat(true);
        } else if (!holding || !IsSitting()) {
            Eat(false);
        }
        if (!IsEatingPanda()) return;
        AddEatingParticles();
        if (serverSide && m_eatCounter > 80 && random->NextInt(20) == 1) {
            // Past 100 ticks, ground food (bamboo, cake) is used up and the
            // panda stands; either way the chewing stops.
            if (m_eatCounter > 100 && CanPickUpAndEat(GetEquipment(EquipmentSlot::MAINHAND))) {
                SetEquipment(EquipmentSlot::MAINHAND, ItemStack{});
                GameEvent(GameEventId::Eat);
                Sit(false);
            }
            Eat(false);
            return;
        }
        ++m_eatCounter;
    }

    void Panda::AddEatingParticles() {
        // MC Panda.addEatingParticles: every 5th tick of chewing, the eat
        // sound and six crumbs of the held item in front of the muzzle.
        if (m_eatCounter % 5 != 0 || !m_level) return;
        JavaRandom& r = m_level->Random();
        PlaySound(SoundEvents::PANDA_EAT, 0.5f + 0.5f * static_cast<float>(r.NextInt(2)),
                  (r.NextFloat() - r.NextFloat()) * 0.2f + 1.0f);
        const ItemStack& held = GetEquipment(EquipmentSlot::MAINHAND);
        if (held.IsEmpty()) return;
        const ParticleOptions particle = ParticleOptions::Item(held.itemId);
        const double xa = -static_cast<double>(xRot) * 0.017453292;
        const double ya = -static_cast<double>(yRot) * 0.017453292;
        const double ba = -static_cast<double>(yBodyRot) * 0.017453292;
        const auto rotX = [](glm::dvec3 v, double a) {
            const double c = std::cos(a), s = std::sin(a);
            return glm::dvec3(v.x, v.y * c + v.z * s, v.z * c - v.y * s);
        };
        const auto rotY = [](glm::dvec3 v, double a) {
            const double c = std::cos(a), s = std::sin(a);
            return glm::dvec3(v.x * c + v.z * s, v.y, v.z * c - v.x * s);
        };
        for (int i = 0; i < 6; ++i) {
            glm::dvec3 v((static_cast<double>(r.NextFloat()) - 0.5) * 0.1,
                         static_cast<double>(r.NextFloat()) * 0.1 + 0.1,
                         (static_cast<double>(r.NextFloat()) - 0.5) * 0.1);
            v = rotY(rotX(v, xa), ya);
            glm::dvec3 p((static_cast<double>(r.NextFloat()) - 0.5) * 0.8,
                         static_cast<double>(-r.NextFloat()) * 0.6 - 0.3,
                         1.0 + (static_cast<double>(r.NextFloat()) - 0.5) * 0.4);
            p = rotY(p, ba) + glm::dvec3(position.x, GetEyeY() + 1.0, position.z);
            m_level->AddParticle(particle, p.x, p.y, p.z, v.x, v.y + 0.05, v.z);
        }
    }

    void Panda::DropSneezeGift() {
        // MC dropFromGiftLootTable(PANDA_SNEEZE, spawnAtLocation): the GIFT
        // parameter set (ORIGIN at the panda), the level random.
        if (!m_level || m_level->IsClientSide()) return;
        ChestLoot::LootLevelContext context;
        context.dimensionId = DimensionToRaw(m_level->Dimension());
        context.origin = position;
        std::vector<ItemStack> gifts;
        if (!ChestLoot::GetRandomItems("minecraft:gameplay/panda_sneeze", m_level->Random(), 0.0f, gifts, &context)) {
            return;
        }
        for (const ItemStack& gift : gifts) {
            if (!gift.IsEmpty()) DropItemStackAt(m_level->Dimension(), position, gift);
        }
    }

    void Panda::HandleRoll() {
        // MC Panda.handleRoll, verbatim: 32 ticks of somersault; the server
        // drives the velocity script, both sides count the clock.
        ++m_rollCounter;
        if (m_rollCounter > 32) {
            Roll(false);
            return;
        }
        if (m_level && !m_level->IsClientSide()) {
            const glm::dvec3 movement = velocity;
            if (m_rollCounter == 1) {
                const float angle = yRot * Mth::kDegToRad;
                const double multiplier = IsBaby() ? 0.1 : 0.2;
                m_rollDelta = glm::dvec3(
                    movement.x + static_cast<double>(-std::sin(angle)) * multiplier,
                    0.0,
                    movement.z + static_cast<double>(std::cos(angle)) * multiplier);
                velocity = m_rollDelta + glm::dvec3(0.0, 0.27, 0.0);
            } else if (m_rollCounter == 7 || m_rollCounter == 15
                       || m_rollCounter == 23) {
                velocity = glm::dvec3(0.0, onGround ? 0.27 : movement.y, 0.0);
            } else {
                velocity = glm::dvec3(m_rollDelta.x, movement.y, m_rollDelta.z);
            }
            needsSync = true;
        }
    }

    void Panda::UpdateRamps() {
        // MC updateSitAmount / updateOnBackAnimation / updateRollAmount.
        m_sitAmountO = m_sitAmount;
        m_sitAmount = IsSitting() ? std::min(1.0f, m_sitAmount + 0.15f)
                                  : std::max(0.0f, m_sitAmount - 0.19f);

        m_onBackAmountO = m_onBackAmount;
        m_onBackAmount = IsOnBack() ? std::min(1.0f, m_onBackAmount + 0.15f)
                                    : std::max(0.0f, m_onBackAmount - 0.19f);

        m_rollAmountO = m_rollAmount;
        m_rollAmount = IsRolling() ? std::min(1.0f, m_rollAmount + 0.15f)
                                   : std::max(0.0f, m_rollAmount - 0.19f);
    }

    float Panda::GetSitAmount(float partialTick) const {
        return Mth::Lerp(partialTick, m_sitAmountO, m_sitAmount);
    }

    float Panda::GetLieOnBackAmount(float partialTick) const {
        return Mth::Lerp(partialTick, m_onBackAmountO, m_onBackAmount);
    }

    float Panda::GetRollAmount(float partialTick) const {
        return Mth::Lerp(partialTick, m_rollAmountO, m_rollAmount);
    }

    std::shared_ptr<SpawnGroupData>
    Panda::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Panda.finalizeSpawn: both genes rolled fresh, stats applied,
        // and 20% of later pack members spawn as cubs.
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            SetMainGene(RandomGene(rng));
            SetHiddenGene(RandomGene(rng));
        }
        ApplyGeneAttributes();
        m_health = GetMaxHealth();

        if (m_level) {
            auto* data = dynamic_cast<AgeableGroupData*>(groupData.get());
            if (!data) {
                groupData = std::make_shared<AgeableGroupData>(0.2f);
                data = static_cast<AgeableGroupData*>(groupData.get());
            }
            if (data->size > 0
                && m_level->Random().NextFloat() < data->babyChance) {
                SetAge(kBabyStartAge);
            }
            ++data->size;
        }

        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    void Panda::RegisterGoals() {
        // MC Panda.registerGoals, priority for priority. PandaSitGoal is
        // inert (item-gated) — see its declaration.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<PandaPanicGoal>(this, 2.0));
        m_goalSelector.AddGoal(2, std::make_unique<PandaBreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<PandaAttackGoal>(this, 1.2, true));
        m_goalSelector.AddGoal(4, std::make_unique<TemptGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<PandaAvoidGoal>(
                                      this, 8.0f, 2.0, 2.0));
        const std::vector<EntityTypeId>& monsters = MonsterCategoryTypes();
        m_goalSelector.AddGoal(6, std::make_unique<PandaAvoidGoal>(
                                      this, monsters.data(),
                                      static_cast<int>(monsters.size()),
                                      4.0f, 2.0, 2.0));
        m_goalSelector.AddGoal(7, std::make_unique<PandaSitGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<PandaLieOnBackGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<PandaSneezeGoal>(this));
        auto look = std::make_unique<PandaLookAtPlayerGoal>(this, 6.0f);
        lookAtPlayerGoal = look.get();
        m_goalSelector.AddGoal(9, std::move(look));
        m_goalSelector.AddGoal(10, std::make_unique<RandomLookAroundGoal>(this));
        m_goalSelector.AddGoal(12, std::make_unique<PandaRollGoal>(this));
        m_goalSelector.AddGoal(13, std::make_unique<FollowParentGoal>(this, 1.25));
        m_goalSelector.AddGoal(14, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                       this, 1.0));
        m_targetSelector.AddGoal(1, std::make_unique<PandaHurtByTargetGoal>(this));
    }

    // ── Ocelot ─────────────────────────────────────────────────────────────

    namespace {

        constexpr EntityTypeId kFelineChickenTargets[] = { EntityTypeId::Chicken };

        // MC Cat/Ocelot.customServerAiStep — surface OcelotAttackGoal's
        // chosen speed as the CROUCHING pose and the sprint flag.
        void FelinePoseFromMoveControl(Mob& mob) {
            MoveControl& control = mob.GetMoveControl();
            if (control.HasWanted()) {
                const double speed = control.GetSpeedModifier();
                if (speed == 0.6) {
                    mob.SetPose(Pose::Crouching);
                    mob.SetSprinting(false);
                    return;
                }
                if (speed == 1.33) {
                    mob.SetPose(Pose::Standing);
                    mob.SetSprinting(true);
                    return;
                }
            }
            mob.SetPose(Pose::Standing);
            mob.SetSprinting(false);
        }

    } // namespace

    void Ocelot::CreateAttributes(AttributeMap& out) {
        // MC Ocelot.createAttributes: MAX_HEALTH 10, MOVEMENT_SPEED 0.3,
        // ATTACK_DAMAGE 3.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,    10.0);
        out.Register(Attribute::MovementSpeed, 0.3);
        out.Register(Attribute::AttackDamage,  3.0);
    }

    Ocelot::Ocelot(EntityLevel* level) : Animal(EntityTypeId::Ocelot, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Ocelot::IsFood(uint32_t itemId) const {
        // MC ItemTags.OCELOT_FOOD: raw cod, raw salmon.
        return itemId == Items::Cod || itemId == Items::Salmon;
    }

    std::unique_ptr<Animal> Ocelot::CreateBaby() {
        return std::make_unique<Ocelot>(m_level);
    }

    void Ocelot::RegisterGoals() {
        // MC Ocelot.registerGoals, priority for priority. The avoid goal is
        // ReassessTrustingGoals' — registered for a NON-trusting ocelot,
        // dropped the moment trust lands (MC's reassessTrustingGoals, called
        // from the constructor and from setTrusting).
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<OcelotTemptGoal>(this, 0.6, true));
        ReassessTrustingGoals();
        m_goalSelector.AddGoal(7, std::make_unique<LeapAtTargetGoal>(this, 0.3f));
        m_goalSelector.AddGoal(8, std::make_unique<OcelotAttackGoal>(this));
        m_goalSelector.AddGoal(9, std::make_unique<BreedGoal>(this, 0.8));
        m_goalSelector.AddGoal(10, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                       this, 0.8, 1.0000001e-5f));
        m_goalSelector.AddGoal(11, std::make_unique<LookAtPlayerGoal>(this, 10.0f));
        // MC: NearestAttackableTargetGoal(Chicken, mustSee=false) and the
        // baby-turtle-on-land hunt (the selector-capable goal carries it).
        m_targetSelector.AddGoal(1, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kFelineChickenTargets, 1,
                                        /*mustSee=*/false));
        m_targetSelector.AddGoal(1, std::make_unique<NonTameRandomTargetGoal>(
                                        this, EntityTypeId::Turtle,
                                        /*mustSee=*/false,
                                        /*babyOnLandOnly=*/true));
    }

    void Ocelot::CustomServerAiStep() {
        FelinePoseFromMoveControl(*this);
    }

    void Ocelot::SetTrusting(bool trusting) {
        m_trusting = trusting;
        ReassessTrustingGoals();
    }

    void Ocelot::ReassessTrustingGoals() {
        // MC Ocelot.reassessTrustingGoals. MC re-adds the SAME goal object;
        // the selector here owns its goals, so a fresh one is built each
        // time — the goal is stateless between runs.
        if (m_ocelotAvoidPlayersGoal) {
            m_goalSelector.RemoveGoal(m_ocelotAvoidPlayersGoal);
            m_ocelotAvoidPlayersGoal = nullptr;
        }
        if (!m_trusting) {
            auto avoid = std::make_unique<OcelotAvoidEntityGoal>(
                this, 16.0f, 0.8, 1.33);
            m_ocelotAvoidPlayersGoal = avoid.get();
            m_goalSelector.AddGoal(4, std::move(avoid));
        }
    }

    void Ocelot::HandleEntityEvent(uint8_t id) {
        if (id == 41 || id == 40) {
            // MC Ocelot.spawnTrustingParticles — the TamableAnimal 7-particle
            // shape: gaussian * 0.02 velocities, positions at getRandomX(1.0)
            // / getRandomY() + 0.5 / getRandomZ(1.0), Java's left-to-right
            // argument draw order.
            if (!m_level) return;
            const ParticleKind kind = (id == 41) ? ParticleKind::Heart
                                                 : ParticleKind::Smoke;
            JavaRandom& rng = m_level->Random();
            const double w = static_cast<double>(GetBbWidth());
            const double h = static_cast<double>(GetBbHeight());
            for (int i = 0; i < 7; ++i) {
                const double xa = rng.NextGaussian() * 0.02;
                const double ya = rng.NextGaussian() * 0.02;
                const double za = rng.NextGaussian() * 0.02;
                const double px = position.x + w * (2.0 * rng.NextDouble() - 1.0);
                const double py = position.y + h * rng.NextDouble() + 0.5;
                const double pz = position.z + w * (2.0 * rng.NextDouble() - 1.0);
                m_level->AddParticle(kind, px, py, pz, xa, ya, za);
            }
        } else {
            Animal::HandleEntityEvent(id);
        }
    }

    UseResult Ocelot::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Ocelot.mobInteract: only while the tempt goal is RUNNING (the
        // ocelot chose to approach), untrusting, fed its fish inside 3
        // blocks. MC's `temptGoal == null ||` guard covers pre-registerGoals
        // calls, which cannot happen here.
        if (m_goalSelector.IsRunning("OcelotTemptGoal") && !m_trusting &&
            IsFood(held.itemId) && player.DistanceToSqr(*this) < 9.0) {
            UsePlayerItem(held);
            if (m_level && !m_level->IsClientSide()) {
                if (m_level->Random().NextInt(3) == 0) {
                    SetTrusting(true);
                    m_level->BroadcastEntityEvent(*this, 41);
                } else {
                    m_level->BroadcastEntityEvent(*this, 40);
                }
            }
            return UseResult::Success;
        }
        return Animal::MobInteract(player, held);
    }

    // ── Cat ────────────────────────────────────────────────────────────────

    void Cat::CreateAttributes(AttributeMap& out) {
        // MC Cat.createAttributes: MAX_HEALTH 10, MOVEMENT_SPEED 0.3,
        // ATTACK_DAMAGE 3.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,    10.0);
        out.Register(Attribute::MovementSpeed, 0.3);
        out.Register(Attribute::AttackDamage,  3.0);
    }

    Cat::Cat(EntityLevel* level)
        : Animal(EntityTypeId::Cat, level), TamableAnimal(this) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Cat::IsFood(uint32_t itemId) const {
        // MC ItemTags.CAT_FOOD: raw cod, raw salmon.
        return itemId == Items::Cod || itemId == Items::Salmon;
    }

    bool Cat::CanMate(const Animal& other) const {
        // MC Cat.canMate: this tame, the partner a tame cat, then the base
        // same-species love test.
        if (!IsTame()) return false;
        const auto* cat = dynamic_cast<const Cat*>(&other);
        if (!cat || !cat->IsTame()) return false;
        return Animal::CanMate(other);
    }

    UseResult Cat::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Cat.mobInteract, verbatim shape.
        const bool clientSide = m_level && m_level->IsClientSide();

        if (IsTame()) {
            if (IsOwnedBy(player)) {
                const int dye = DyeColorOf(held);
                if (dye >= 0) {
                    // MC ItemTags.CAT_COLLAR_DYES (= #dyes): a new colour
                    // recolours the collar; the same colour falls through to
                    // Animal's (and so to the sit toggle).
                    if (static_cast<uint8_t>(dye) != m_collarColor) {
                        if (!clientSide) {
                            SetCollarColor(static_cast<uint8_t>(dye));
                            held.count -= 1;   // itemStack.consume(1, player)
                            if (held.count <= 0) held.Clear();
                            SetPersistenceRequired(true);
                        }
                        return UseResult::Success;
                    }
                } else if (IsFood(held.itemId) && GetHealth() < GetMaxHealth()) {
                    if (!clientSide) Feed(held, 1.0f, 1.0f);
                    return UseResult::Success;
                }

                const UseResult r = Animal::MobInteract(player, held);
                if (!ConsumesAction(r)) {
                    SetOrderedToSit(!IsOrderedToSit());
                    return UseResult::Success;
                }
                return r;
            }
        } else if (IsFood(held.itemId)) {
            if (!clientSide) {
                UsePlayerItem(held);
                TryToTame(player);
                SetPersistenceRequired(true);
            }
            return UseResult::Success;
        }

        const UseResult r = Animal::MobInteract(player, held);
        if (ConsumesAction(r)) SetPersistenceRequired(true);
        return r;
    }

    void Cat::TryToTame(LivingEntity& player) {
        // MC Cat.tryToTame: 1-in-3, and the fresh tame sits.
        if (m_level && m_level->Random().NextInt(3) == 0) {
            Tame(player);
            SetOrderedToSit(true);
            BroadcastTamingResult(true);
        } else {
            BroadcastTamingResult(false);
        }
    }

    void Cat::ReassessTameGoals() {
        // MC Cat.reassessTameGoals — the wild avoid-players goal exists only
        // while untame. MC re-adds the SAME goal object; the selector here
        // owns its goals, so a fresh one is built each time.
        if (m_avoidPlayersGoal) {
            m_goalSelector.RemoveGoal(m_avoidPlayersGoal);
            m_avoidPlayersGoal = nullptr;
        }
        if (!IsTame()) {
            auto avoid = std::make_unique<CatAvoidEntityGoal>(
                this, 16.0f, 0.8, 1.33);
            m_avoidPlayersGoal = avoid.get();
            m_goalSelector.AddGoal(4, std::move(avoid));
        }
    }

    void Cat::SpawnChildFromBreeding(Animal& partner) {
        m_breedPartner = dynamic_cast<const Cat*>(&partner);
        Animal::SpawnChildFromBreeding(partner);
        m_breedPartner = nullptr;
    }

    std::unique_ptr<Animal> Cat::CreateBaby() {
        // MC Cat.getBreedOffspring: the coat of a random parent; from a tame
        // parent the owner, the tame flag and the parents' mixed collar. A
        // spawn egg on an adult breeds it with itself, as MC's SpawnEggItem
        // hands getBreedOffspring the adult as its own partner.
        auto baby = std::make_unique<Cat>(m_level);
        const Cat& partner = m_breedPartner ? *m_breedPartner : *this;
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            baby->m_variant = rng.NextBool() ? m_variant : partner.m_variant;
            if (IsTame()) {
                baby->SetOwnerUuid(GetOwnerUuid());
                baby->SetTame(true, /*includeSideEffects=*/true);
                baby->SetCollarColor(GetMixedDyeColor(m_collarColor, partner.m_collarColor, rng));
            }
        } else {
            baby->m_variant = m_variant;
        }
        return baby;
    }

    void Cat::RegisterGoals() {
        // MC Cat.registerGoals, priority for priority. The bed-gated goals
        // (3, 5, 7) are inert — each says why in CatGoals.hpp. The avoid
        // goal is ReassessTameGoals' — registered for a WILD cat, dropped on
        // tame (MC calls reassessTameGoals from the constructor and from
        // setTame; here the tail of this function and
        // ApplyTamingSideEffects).
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<TamableAnimalPanicGoal>(
                                      this, this, 1.5));
        m_goalSelector.AddGoal(2, std::make_unique<SitWhenOrderedToGoal>(this, this));
        m_goalSelector.AddGoal(3, std::make_unique<CatRelaxOnOwnerGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<CatTemptGoal>(this, 0.6, true));
        m_goalSelector.AddGoal(5, std::make_unique<CatLieOnBedGoal>(this, 1.1, 8));
        m_goalSelector.AddGoal(6, std::make_unique<FollowOwnerGoal>(
                                      this, this, 1.0, 10.0f, 5.0f));
        m_goalSelector.AddGoal(7, std::make_unique<CatSitOnBlockGoal>(this, 0.8));
        m_goalSelector.AddGoal(8, std::make_unique<LeapAtTargetGoal>(this, 0.3f));
        m_goalSelector.AddGoal(9, std::make_unique<OcelotAttackGoal>(this));
        m_goalSelector.AddGoal(10, std::make_unique<BreedGoal>(this, 0.8));
        m_goalSelector.AddGoal(11, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                       this, 0.8, 1.0000001e-5f));
        m_goalSelector.AddGoal(12, std::make_unique<LookAtPlayerGoal>(this, 10.0f));
        m_targetSelector.AddGoal(1, std::make_unique<NonTameRandomTargetGoal>(
                                        this, EntityTypeId::Rabbit,
                                        /*mustSee=*/false,
                                        /*babyOnLandOnly=*/false));
        m_targetSelector.AddGoal(1, std::make_unique<NonTameRandomTargetGoal>(
                                        this, EntityTypeId::Turtle,
                                        /*mustSee=*/false,
                                        /*babyOnLandOnly=*/true));

        // MC's constructor calls reassessTameGoals after registerGoals —
        // this is that call: a fresh cat is wild, so the avoid goal lands.
        ReassessTameGoals();
    }

    void Cat::Tick() {
        // MC Cat.tick → handleLieDown: the purr while lying, then the ramps,
        // every tick on both sides. (The tempt-goal beg and the
        // lying-on-sleeping-player scan wait on a tempt-goal handle and
        // player sleep.)
        Animal::Tick();

        if ((IsLying() || IsRelaxStateOne()) && tickCount % 5 == 0 && m_level) {
            JavaRandom& rng = m_level->Random();
            PlaySound(IsBaby() ? SoundEvents::CAT_PURR_BABY
                      : m_soundVariant == 1 ? SoundEvents::ENTITY_CAT_ROYAL_PURR : SoundEvents::ENTITY_CAT_PURR,
                      0.6f + 0.4f * (rng.NextFloat() - rng.NextFloat()), 1.0f);
        }

        m_lieDownAmountO = m_lieDownAmount;
        m_lieDownAmountOTail = m_lieDownAmountTail;
        if (IsLying()) {
            m_lieDownAmount = std::min(1.0f, m_lieDownAmount + 0.15f);
            m_lieDownAmountTail = std::min(1.0f, m_lieDownAmountTail + 0.08f);
        } else {
            m_lieDownAmount = std::max(0.0f, m_lieDownAmount - 0.22f);
            m_lieDownAmountTail = std::max(0.0f, m_lieDownAmountTail - 0.13f);
        }

        m_relaxStateOneAmountO = m_relaxStateOneAmount;
        if (IsRelaxStateOne()) {
            m_relaxStateOneAmount = std::min(1.0f, m_relaxStateOneAmount + 0.1f);
        } else {
            m_relaxStateOneAmount = std::max(0.0f, m_relaxStateOneAmount - 0.13f);
        }
    }

    const char* Cat::GetAmbientSound() const {
        // MC Cat.getAmbientSound off the sound set (CatSoundVariant: classic
        // or royal for adults; babies share the baby set).
        const bool baby = IsBaby();
        const bool royal = !baby && m_soundVariant == 1;
        if (!IsTame()) {
            return baby ? SoundEvents::CAT_STRAY_AMBIENT_BABY
                 : royal ? SoundEvents::ENTITY_CAT_ROYAL_STRAY_AMBIENT : SoundEvents::ENTITY_CAT_STRAY_AMBIENT;
        }
        if (IsInLove()) {
            return baby ? SoundEvents::CAT_PURR_BABY : royal ? SoundEvents::ENTITY_CAT_ROYAL_PURR : SoundEvents::ENTITY_CAT_PURR;
        }
        if (m_level && m_level->Random().NextInt(4) == 0) {
            return baby ? SoundEvents::CAT_PURREOW_BABY
                 : royal ? SoundEvents::ENTITY_CAT_ROYAL_PURREOW : SoundEvents::ENTITY_CAT_PURREOW;
        }
        return baby ? SoundEvents::CAT_AMBIENT_BABY : royal ? SoundEvents::ENTITY_CAT_ROYAL_AMBIENT : SoundEvents::ENTITY_CAT_AMBIENT;
    }

    const char* Cat::GetHurtSound(MobDamageSource source) const {
        if (!IsBaby() && m_soundVariant == 1) return SoundEvents::ENTITY_CAT_ROYAL_HURT;
        return Animal::GetHurtSound(source);
    }

    const char* Cat::GetDeathSound() const {
        if (!IsBaby() && m_soundVariant == 1) return SoundEvents::ENTITY_CAT_ROYAL_DEATH;
        return Animal::GetDeathSound();
    }

    void Cat::PlayEatingSound() {
        const bool royal = !IsBaby() && m_soundVariant == 1;
        PlaySound(IsBaby() ? SoundEvents::CAT_EAT_BABY : royal ? SoundEvents::ENTITY_CAT_ROYAL_EAT : SoundEvents::ENTITY_CAT_EAT,
                  1.0f, 1.0f);
    }

    void Cat::Hiss() {
        const bool royal = !IsBaby() && m_soundVariant == 1;
        MakeSound(IsBaby() ? SoundEvents::CAT_HISS_BABY : royal ? SoundEvents::ENTITY_CAT_ROYAL_HISS : SoundEvents::ENTITY_CAT_HISS);
    }

    float Cat::GetLieDownAmount(float partialTick) const {
        return Mth::Lerp(partialTick, m_lieDownAmountO, m_lieDownAmount);
    }

    float Cat::GetLieDownAmountTail(float partialTick) const {
        return Mth::Lerp(partialTick, m_lieDownAmountOTail, m_lieDownAmountTail);
    }

    float Cat::GetRelaxStateOneAmount(float partialTick) const {
        return Mth::Lerp(partialTick, m_relaxStateOneAmountO, m_relaxStateOneAmount);
    }

    void Cat::CustomServerAiStep() {
        FelinePoseFromMoveControl(*this);
    }

    std::shared_ptr<SpawnGroupData>
    Cat::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC selectVariantToSpawn weights by structure (witch huts force
        // all_black) and moon phase; neither context exists, so the roll is
        // uniform over the 11 variants.
        if (m_level) {
            m_variant = static_cast<uint8_t>(m_level->Random().NextInt(kVariantCount));
            // CatSoundVariants.pickRandomSoundVariant.
            m_soundVariant = static_cast<uint8_t>(m_level->Random().NextInt(FarmSoundVariants::Count(FarmSoundVariants::Mob::Cat)));
        }
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    // ── AbstractHorse ──────────────────────────────────────────────────────

    namespace {

        // MC Horse.HorseGroupData: the herd's coat, over AgeableMobGroupData
        // (true) — a 5 % baby chance for the herd's later members.
        struct HorseGroupData : AgeableGroupData {
            explicit HorseGroupData(int v) : AgeableGroupData(0.05f), variant(v) {}
            int variant;
        };

        // MC ZombieHorse's jump / speed generators (BASE_JUMP_STRENGTH 0.5 +
        // 3 × PER_RANDOM_JUMP_STRENGTH; (BASE_SPEED 9 + 3 × PER_RANDOM_SPEED)
        // / SPEED_FACTOR, the factor widened from its float).
        double GenerateZombieHorseJumpStrength(JavaRandom& rng) {
            const double a = rng.NextDouble();
            const double b = rng.NextDouble();
            const double c = rng.NextDouble();
            return 0.5 + a * 0.06666666666666667 + b * 0.06666666666666667 + c * 0.06666666666666667;
        }
        double GenerateZombieHorseSpeed(JavaRandom& rng) {
            const double a = rng.NextDouble();
            const double b = rng.NextDouble();
            const double c = rng.NextDouble();
            return (9.0 + a * 1.0 + b * 1.0 + c * 1.0) / 42.15999984741211;
        }

        // MC AbstractHorse.isWoodSoundType.
        bool IsWoodSoundType(const SoundType* type) {
            return type == &SoundTypes::WOOD || type == &SoundTypes::NETHER_WOOD || type == &SoundTypes::STEM ||
                   type == &SoundTypes::CHERRY_WOOD || type == &SoundTypes::BAMBOO_WOOD;
        }

        // One server-side line per equine interaction: what the hand held
        // and what the equine did with it (diagnosing mount / feed /
        // refusals).
        void LogHorseInteract(const AbstractHorse& horse, const ItemStack& held, bool sneaking,
                              const char* outcome) {
            const EntityLevel* level = horse.Level();
            if (!level || level->IsClientSide()) return;
            Log::Info("[Horse] interact entity=%d item=%s count=%d sneaking=%d baby=%d tamed=%d temper=%d vehicle=%d -> %s",
                      horse.GetId(), held.IsEmpty() ? "empty" : std::string(ItemRegistry::Slug(held.itemId)).c_str(),
                      held.IsEmpty() ? 0 : held.count, sneaking ? 1 : 0, horse.IsBaby() ? 1 : 0,
                      horse.IsTamedHorse() ? 1 : 0, horse.GetTemper(), horse.IsVehicle() ? 1 : 0, outcome);
        }

    } // namespace

    AbstractHorse::AbstractHorse(EntityTypeId type, EntityLevel* level)
        : GenericAnimal(type, level), HorseTaming(this) {
        // MC AbstractHorse.createBaseHorseAttributes: the def carries the
        // per-type MAX_HEALTH / MOVEMENT_SPEED / STEP_HEIGHT; the rest of the
        // base supplier — JUMP_STRENGTH 0.7 (AbstractChestedHorse: 0.5),
        // SAFE_FALL_DISTANCE 6, FALL_DAMAGE_MULTIPLIER 0.5 — is registered
        // here.
        const bool chested = type == EntityTypeId::Donkey || type == EntityTypeId::Mule;
        m_attributes.Register(Attribute::JumpStrength, chested ? 0.5 : 0.7);
        m_attributes.Register(Attribute::SafeFallDistance, 6.0);
        m_attributes.Register(Attribute::FallDamageMultiplier, 0.5);

        // The GenericAnimal constructor registered the def-driven animal
        // set; MC's equine table replaces it wholesale (the Bat precedent
        // for a promoted class disowning its base goals).
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        RegisterHorseGoals();
    }

    void AbstractHorse::RegisterHorseGoals() {
        // MC AbstractHorse.registerGoals, in MC's order (ties at one
        // priority keep insertion order). RunAroundLikeCrazyGoal bolts and
        // bucks while an untamed equine carries a player — the taming loop.
        m_goalSelector.AddGoal(1, std::make_unique<RunAroundLikeCrazyGoal>(this, 1.2));
        // MC BreedGoal(1.0, AbstractHorse.class): CanMate decides the pairs
        // (horse × horse, donkey × donkey, horse × donkey).
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.7));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        if (CanPerformRearing()) {
            m_goalSelector.AddGoal(9, std::make_unique<RandomStandGoal>(this));
        }

        // addBehaviourGoals — per class (a virtual the constructor cannot
        // dispatch, so by type): the skeleton horse adds none (no FloatGoal:
        // it walks the bottom); the zombie horse floats and is tempted by its
        // food; the rest float, panic unless a mob rides them, and follow
        // HORSE_TEMPT_ITEMS (one TemptGoal per item — the shared goal carries
        // one override item, the Pig precedent).
        switch (GetType()) {
            case EntityTypeId::SkeletonHorse:
                break;
            case EntityTypeId::ZombieHorse:
                m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
                m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.25, false));
                break;
            default:
                m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
                m_goalSelector.AddGoal(1, std::make_unique<MountPanicGoal>(this, 1.2));
                m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.25, false, Items::GoldenCarrot));
                m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.25, false, Items::GoldenApple));
                m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.25, false, Items::EnchantedGoldenApple));
                break;
        }
    }

    void AbstractHorse::StandIfPossible() {
        // MC standIfPossible: `canPerformRearing() && (isEffectiveAi() ||
        // !isClientSide())` — the server, or the client that steers it
        // (its isEffectiveAi is isLocalInstanceAuthoritative).
        if (!CanPerformRearing() || !m_level) return;
        if (!m_level->IsClientSide() || IsLocallySteered()) SetStanding(20);
    }

    bool AbstractHorse::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool wasHurt = Animal::Hurt(source, amount, attacker);
        // MC hurtServer: 1-in-3 hits make the horse rear.
        if (wasHurt && m_level && !m_level->IsClientSide()
            && m_level->Random().NextInt(3) == 0) {
            StandIfPossible();
        }
        return wasHurt;
    }

    bool AbstractHorse::IsFood(uint32_t itemId) const {
        // ItemTags.HORSE_FOOD: wheat, sugar, hay_block, apple, carrot,
        // golden_carrot, golden_apple, enchanted_golden_apple. The zombie
        // horse's isFood is ItemTags.ZOMBIE_HORSE_FOOD: red_mushroom.
        if (GetType() == EntityTypeId::ZombieHorse) {
            return itemId == ItemRegistry::FromBlock(BlockID::RedMushroom);
        }
        return itemId == Items::Wheat || itemId == Items::Sugar ||
               itemId == ItemRegistry::FromBlock(BlockID::HayBlock) || itemId == Items::Apple ||
               itemId == Items::Carrot || itemId == Items::GoldenCarrot || itemId == Items::GoldenApple ||
               itemId == Items::EnchantedGoldenApple;
    }

    bool AbstractHorse::CanParent() const {
        // MC AbstractHorse.canParent.
        return !IsVehicle() && !IsPassenger() && IsTamedHorse() && !IsBaby() &&
               GetHealth() >= GetMaxHealth() && IsInLove();
    }

    void AbstractHorse::SpawnChildFromBreeding(Animal& partner) {
        m_breedPartner = dynamic_cast<const AbstractHorse*>(&partner);
        GenericAnimal::SpawnChildFromBreeding(partner);
        m_breedPartner = nullptr;
    }

    std::shared_ptr<SpawnGroupData>
    AbstractHorse::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC AbstractHorse.finalizeSpawn: AgeableMobGroupData(0.2F) when the
        // caller brought none, randomizeAttributes, then super — AgeableMob's
        // herd roll (the first member adult, later ones a baby on
        // nextFloat() <= chance).
        if (!groupData) groupData = std::make_shared<AgeableGroupData>(0.2f);
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            RandomizeAttributes(rng);
            // LivingEntity.onAttributeUpdated: health never above the new max.
            if (GetHealth() > GetMaxHealth()) SetHealth(GetMaxHealth());
            if (auto* data = dynamic_cast<AgeableGroupData*>(groupData.get())) {
                if (data->size > 0 && rng.NextFloat() <= data->babyChance) SetAge(kBabyStartAge);
                ++data->size;
            }
        }
        return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
    }

    UseResult AbstractHorse::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Horse.mobInteract / AbstractChestedHorse.mobInteract (the
        // donkey and the mule): ridden, the inventory sneak, or a foal
        // offered a golden dandelion (the age lock) → AbstractHorse's; else
        // food is fed, any other item makes an untamed one mad, and an empty
        // hand (or a tamed one's item) goes on to AbstractHorse's.
        const bool sneaking = IsSecondaryUseActive(player);
        const bool shouldOpenInventory = !IsBaby() && IsTamedHorse() && sneaking;
        if (IsVehicle() || shouldOpenInventory ||
            (IsBaby() && held.itemId == ItemRegistry::FromBlock(BlockID::GoldenDandelion))) {
            return BaseMobInteract(player, held);
        }
        if (!held.IsEmpty()) {
            if (IsFood(held.itemId)) {
                const UseResult fed = FedFood(player, held);
                LogHorseInteract(*this, held, sneaking, ConsumesAction(fed) ? "feed" : "feed-refused");
                return fed;
            }
            if (!IsTamedHorse()) {
                LogHorseInteract(*this, held, sneaking, "makeMad (non-food item, untamed)");
                MakeMad();
                return UseResult::Success;
            }
            // AbstractChestedHorse.mobInteract (the donkey, the mule): a
            // chest on a tamed one without one → equipChest.
            if (m_mountInventory.CanCarryChest() && !HasChest() &&
                held.itemId == ItemRegistry::FromBlock(BlockID::Chest)) {
                if (m_level && !m_level->IsClientSide()) {
                    MountInventory::EquipChest(*this, held, SoundEvents::DONKEY_CHEST);
                }
                LogHorseInteract(*this, held, sneaking, "equipChest");
                return UseResult::Success;
            }
        }
        return BaseMobInteract(player, held);
    }

    UseResult AbstractHorse::BaseMobInteract(LivingEntity& player, ItemStack& held) {
        // MC AbstractHorse.mobInteract: ridden or a foal → Animal's (the
        // dandelion, the baby's food); a tamed one's sneak opens the
        // inventory; otherwise the held item's own interaction first (a
        // saddle, a lead, a name tag), then the player climbs on.
        const bool sneaking = IsSecondaryUseActive(player);
        if (IsVehicle() || IsBaby()) {
            LogHorseInteract(*this, held, sneaking,
                             IsVehicle() ? "reject:already-ridden (animal)" : "reject:baby (animal)");
            return Animal::MobInteract(player, held);
        }
        if (IsTamedHorse() && sneaking) {
            // openCustomInventoryScreen.
            OpenCustomInventoryScreen(player);
            LogHorseInteract(*this, held, sneaking, "inventory");
            return UseResult::Success;
        }
        if (!held.IsEmpty()) {
            if (const auto interact = ItemRegistry::Get(held.itemId).interactLivingEntity) {
                const UseResult r = interact(held, *this);
                if (ConsumesAction(r)) {
                    LogHorseInteract(*this, held, sneaking, "item-interaction");
                    return r;
                }
            }
            // isEquippableInSlot(stack, BODY) && !isWearingBodyArmor() →
            // equipBodyArmor (horse armour; the #can_wear_horse_armor tag
            // decides who takes it).
            if (IsEquippableInSlot(held, EquipmentSlot::BODY) && !HasItemInSlot(EquipmentSlot::BODY)) {
                EquipBodyArmor(player, held);
                LogHorseInteract(*this, held, sneaking, "equipBodyArmor");
                return UseResult::Success;
            }
        }
        DoPlayerRide(player);
        if (m_level && !m_level->IsClientSide()) {
            LogHorseInteract(*this, held, sneaking, GetPlayerRider() ? "mount" : "reject:riding-refused");
        }
        return UseResult::Success;
    }

    void AbstractHorse::EquipBodyArmor(LivingEntity& player, ItemStack& held) {
        // MC equipBodyArmor: isEquippableInSlot(stack, BODY) →
        // setItemSlotAndDropWhenKilled(BODY, stack.consumeAndReturn(1,
        // player)) — the creative count comes back through Player.interactOn.
        (void)player;
        if (!m_level || m_level->IsClientSide() || !IsEquippableInSlot(held, EquipmentSlot::BODY)) return;
        ItemStack one = held;
        one.count = 1;
        held.count -= 1;
        if (held.count <= 0) held.Clear();
        SetItemSlotAndDropWhenKilled(EquipmentSlot::BODY, one);
    }

    void AbstractHorse::OpenCustomInventoryScreen(LivingEntity& player) {
        // MC openCustomInventoryScreen: server side, the mount free or
        // carrying this player, tamed → player.openHorseInventory.
        if (!m_level || m_level->IsClientSide()) return;
        if ((!IsVehicle() || HasPassenger(player)) && IsTamedHorse()) {
            m_level->OpenMountInventory(player, *this);
        }
    }

    void AbstractHorse::DoPlayerRide(LivingEntity& player) {
        // MC doPlayerRide: setEating(false), clearStanding, then (server)
        // startRiding — the riding system turns the player to the mount
        // (MC's setYRot/setXRot here and addPassenger's absSnapRotationTo).
        SetEating(false);
        ClearStanding();
        if (m_level && !m_level->IsClientSide()) StartPlayerRide(player);
    }

    glm::dvec3 AbstractHorse::PlayerRiderPosition() const {
        // EntityTypes .passengerAttachments(y) per equine; a foal's comes
        // from its BABY_DIMENSIONS: the horse (height - 0.125) × 0.7, the
        // skeleton and zombie horse (height - 0.25) × 0.7, the donkey and
        // mule (0, height + 0.03125, -0.3125) × 0.5 (AbstractChestedHorse).
        const bool baby = IsBaby();
        double attachY = 1.44375;   // horse
        double attachZ = 0.0;
        switch (GetType()) {
            case EntityTypeId::Donkey:
                attachY = baby ? (1.5 + 0.03125) * 0.5 : 1.1125;
                attachZ = baby ? -0.3125 * 0.5 : 0.0;
                break;
            case EntityTypeId::Mule:
                attachY = baby ? (1.6 + 0.03125) * 0.5 : 1.2125;
                attachZ = baby ? -0.3125 * 0.5 : 0.0;
                break;
            case EntityTypeId::SkeletonHorse:
            case EntityTypeId::ZombieHorse:
                attachY = baby ? (1.6 - 0.25) * 0.7 : 1.31875;
                break;
            default:
                attachY = baby ? (1.6 - 0.125) * 0.7 : 1.44375;
                break;
        }
        // AbstractHorse.getPassengerAttachmentPoint: the attachment, then
        // + (0, 0.15, -0.7) * standAnimO * scale (getScale() × getAgeScale():
        // 0.5 for a foal), both turned by .yRot(-yRot rad): x' = z sin a,
        // z' = z cos a.
        const double a = -static_cast<double>(yRot) * static_cast<double>(Mth::kDegToRad);
        const double ageScale = baby ? 0.5 : 1.0;
        const double lean = static_cast<double>(m_standAnimO) * ageScale;
        const double z = attachZ - 0.7 * lean;
        const glm::dvec3 seat(z * std::sin(a), attachY + 0.15 * lean, z * std::cos(a));
        return position + seat - glm::dvec3(0.0, kPlayerVehicleAttachmentY, 0.0);
    }

    void AbstractHorse::PositionRider(Entity& passenger) {
        GenericAnimal::PositionRider(passenger);
        if (LivingEntity* living = passenger.AsLiving()) living->yBodyRot = yBodyRot;
    }

    void AbstractHorse::HandleEntityEvent(uint8_t id) {
        // MC AbstractHorse.handleEntityEvent: 7 hearts, 6 smoke.
        if (id == 7) { SpawnTamingParticles(true); return; }
        if (id == 6) { SpawnTamingParticles(false); return; }
        GenericAnimal::HandleEntityEvent(id);
    }

    UseResult AbstractHorse::FedFood(LivingEntity& player, ItemStack& held) {
        // MC AbstractHorse.fedFood.
        const bool ate = HandleEating(player, held);
        if (ate) UsePlayerItem(held);
        const bool clientSide = m_level && m_level->IsClientSide();
        return !ate && !clientSide ? UseResult::Pass : UseResult::SuccessServer;
    }

    bool AbstractHorse::HandleEating(LivingEntity& player, const ItemStack& held) {
        // MC AbstractHorse.handleEating — the per-item table, verbatim.
        // hay_block and red_mushroom are BLOCK items (item id = block id).
        // red_mushroom is the zombie horse's food (ZOMBIE_HORSE_FOOD).
        const ItemID hayBlock = ItemRegistry::FromBlock(BlockID::HayBlock);
        const ItemID redMushroom = ItemRegistry::FromBlock(BlockID::RedMushroom);

        const bool clientSide = m_level && m_level->IsClientSide();
        bool  itemUsed = false;
        float heal = 0.0f;
        int   ageUpSeconds = 0;
        int   temper = 0;

        const uint32_t id = held.itemId;
        if (id == Items::Wheat) {
            heal = 2.0f; ageUpSeconds = 20; temper = 3;
        } else if (id == Items::Sugar) {
            heal = 1.0f; ageUpSeconds = 30; temper = 3;
        } else if (id == hayBlock) {
            heal = 20.0f; ageUpSeconds = 180;
        } else if (id == Items::Apple) {
            heal = 3.0f; ageUpSeconds = 60; temper = 3;
        } else if (id == redMushroom) {
            heal = 3.0f; ageUpSeconds = 0; temper = 3;
        } else if (id == Items::Carrot) {
            heal = 3.0f; ageUpSeconds = 60; temper = 3;
        } else if (id == Items::GoldenCarrot) {
            heal = 4.0f; ageUpSeconds = 60; temper = 5;
            if (!clientSide && IsTamedHorse() && GetAge() == 0 && !IsInLove()) {
                itemUsed = true;
                SetInLove(&player);
            }
        } else if (id == Items::GoldenApple || id == Items::EnchantedGoldenApple) {
            heal = 10.0f; ageUpSeconds = 240; temper = 10;
            if (!clientSide && IsTamedHorse() && GetAge() == 0 && !IsInLove()) {
                itemUsed = true;
                SetInLove(&player);
            }
        }

        if (GetHealth() < GetMaxHealth() && heal > 0.0f) {
            Heal(heal);
            itemUsed = true;
        }

        // MC AbstractHorse.handleEating: `isBaby() && ageUp > 0 && !isAgeLocked()`.
        if (IsBaby() && ageUpSeconds > 0 && !IsAgeLocked()) {
            // MC: one HAPPY_VILLAGER over the foal (getRandomX(1),
            // getRandomY() + 0.5) — sent, the client does not run this.
            if (!clientSide) {
                SendHappyVillagerParticle();
                AgeUp(ageUpSeconds);
                itemUsed = true;
            }
        }

        if (temper > 0 && (itemUsed || !IsTamedHorse()) &&
            GetTemper() < GetMaxTemper() && !clientSide) {
            ModifyTemper(temper);
            itemUsed = true;
        }

        // MC: `if (itemUsed) this.eating();`.
        if (itemUsed) {
            Eating();
            GameEvent(GameEventId::Eat);   // MC handleEating: gameEvent(EAT)
        }
        return itemUsed;
    }

    void AbstractHorse::MakeMad() {
        if (!IsStanding() && m_level && !m_level->IsClientSide()) {
            StandIfPossible();
            MakeSound(GetAngrySound());
        }
    }

    void AbstractHorse::OpenMouth() {
        // MC openMouth: server only; the flag rides the anim byte.
        if (m_level && !m_level->IsClientSide()) {
            m_mouthCounter = 1;
            m_openMouth = true;
        }
    }

    void AbstractHorse::Eating() {
        // MC eating: openMouth, then the chew sound.
        OpenMouth();
        if (IsSilent() || !m_level) return;
        const char* sound = GetEatingSound();
        if (IsEmptySound(sound)) return;
        JavaRandom& rng = m_level->Random();
        m_level->PlaySound(nullptr, position, sound, GetSoundSource(),
                           1.0f, 1.0f + (rng.NextFloat() - rng.NextFloat()) * 0.2f);
    }

    // ── Riding ─────────────────────────────────────────────────────────────

    glm::dvec3 AbstractHorse::GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) {
        (void)selfInput;
        // MC getRiddenInput: planted while rearing on the ground with no
        // jump pending and no stand-sliding; else the rider's keys, sideways
        // halved and backwards (zza <= 0) quartered.
        if (onGround && m_playerJumpPendingScale == 0.0f && IsStanding() && !m_allowStandSliding) {
            return glm::dvec3(0.0);
        }
        const float sideways = rider.xxa * kSidewaysMoveSpeedFactor;
        float forward = rider.zza;
        if (forward <= 0.0f) forward *= kBackwardsMoveSpeedFactor;
        return glm::dvec3(static_cast<double>(sideways), 0.0, static_cast<double>(forward));
    }

    void AbstractHorse::TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) {
        // MC tickRidden: getRiddenRotation = (rider xRot * 0.5, rider yRot),
        // setRot(y, x) (each % 360), yRotO = yBodyRot = yHeadRot = yRot.
        yRot = std::fmod(rider.yRot, 360.0f);
        xRot = std::fmod(rider.xRot * 0.5f, 360.0f);
        yRotO = yBodyRot = yHeadRot = yRot;
        // isLocalInstanceAuthoritative: the side that moves it.
        if (!CanSimulateMountMovement()) return;
        if (riddenInput.z <= 0.0) m_gallopSoundCounter = 0;
        if (onGround) {
            if (m_playerJumpPendingScale > 0.0f && !jumping) {
                ExecuteRidersJump(m_playerJumpPendingScale, riddenInput);
            }
            m_playerJumpPendingScale = 0.0f;
        }
    }

    float AbstractHorse::GetRiddenSpeed(const RiderControl& rider) const {
        (void)rider;
        return static_cast<float>(GetAttributeValue(Attribute::MovementSpeed));
    }

    float AbstractHorse::JumpPower(float multiplier) const {
        return static_cast<float>(GetAttributeValue(Attribute::JumpStrength)) * multiplier + GetJumpBoostPower();
    }

    void AbstractHorse::ExecuteRidersJump(float amount, const glm::dvec3& input) {
        // MC executeRidersJump: the charged jump straight up, and with the
        // forward key held a push of 0.4 × amount along the heading.
        const double impulse = static_cast<double>(JumpPower(amount));
        velocity.y = impulse;
        needsSync = true;
        if (input.z > 0.0) {
            const float angle = yRot * 0.017453292f;
            const float sin = std::sin(angle);
            const float cos = std::cos(angle);
            velocity.x += static_cast<double>(-0.4f * sin * amount);
            velocity.z += static_cast<double>(0.4f * cos * amount);
        }
    }

    void AbstractHorse::OnPlayerJump(int jumpAmount) {
        // MC onPlayerJump (the steering client): a saddled equine rears to
        // spring (a negative charge only clears the pending jump).
        if (!IsSaddled()) return;
        if (jumpAmount < 0) {
            jumpAmount = 0;
        } else {
            m_allowStandSliding = true;
            StandIfPossible();
        }
        m_playerJumpPendingScale = PlayerJumpPendingScale(jumpAmount);
    }

    void AbstractHorse::HandleStartJump(int jumpScale) {
        // MC handleStartJump (the server): the rear and the jump sound.
        (void)jumpScale;
        m_allowStandSliding = true;
        StandIfPossible();
        PlayJumpSound();
    }

    // ── Sounds ─────────────────────────────────────────────────────────────

    void AbstractHorse::PlayGallopSound(const SoundType& type) {
        PlaySound(SoundEvents::HORSE_GALLOP, type.GetVolume() * 0.15f, type.GetPitch());
    }

    void AbstractHorse::PlayStepSound(const glm::ivec3& pos, BlockState state) {
        if (state.Block() == BlockID::Water || state.Block() == BlockID::Lava) return;
        const SoundType* type = &SoundTypeOf(state);
        if (m_level && m_level->Blocks()) {
            const BlockState above = m_level->Blocks()->GetBlockState(pos.x, pos.y + 1, pos.z);
            if (above.Block() == BlockID::SnowLayer) type = &SoundTypeOf(above);
        }
        if (IsVehicle() && m_canGallop) {
            // MC: ridden, the first five steps clop, then every third one
            // gallops.
            ++m_gallopSoundCounter;
            if (m_gallopSoundCounter > 5 && m_gallopSoundCounter % 3 == 0) {
                PlayGallopSound(*type);
            } else if (m_gallopSoundCounter <= 5) {
                PlaySound(SoundEvents::HORSE_STEP_WOOD, type->GetVolume() * 0.15f, type->GetPitch());
            }
        } else if (IsWoodSoundType(type)) {
            PlaySound(SoundEvents::HORSE_STEP_WOOD, type->GetVolume() * 0.15f, type->GetPitch());
        } else {
            PlaySound(IsBaby() ? SoundEvents::HORSE_STEP_BABY : SoundEvents::HORSE_STEP,
                      type->GetVolume() * 0.15f, type->GetPitch());
        }
    }

    bool AbstractHorse::CauseFallDamage(double fallDist, float damageMultiplier) {
        if (m_level && m_level->IsClientSide()) return false;
        if (fallDist > 1.0) {
            PlaySound(IsBaby() ? SoundEvents::HORSE_LAND_BABY : SoundEvents::HORSE_LAND, 0.4f, 1.0f);
        }
        const int damage = CalculateFallDamage(fallDist, damageMultiplier);
        if (damage <= 0) return false;
        Hurt(MobDamageSource::Fall, static_cast<float>(damage), nullptr);
        // (MC's propagateFallToPassengers here is Entity::CheckFallDamage's
        // in this engine — every landing already hands the riders the fall.)
        PlayBlockFallSound();
        return true;
    }

    // ── Ticking ────────────────────────────────────────────────────────────

    void AbstractHorse::Tick() {
        Animal::Tick();

        // MC tick(): the counters, then the ramps — on BOTH sides (the
        // client's flags come off the anim byte).
        if (m_mouthCounter > 0 && ++m_mouthCounter > 30) {
            m_mouthCounter = 0;
            m_openMouth = false;
        }
        if (m_standCounter > 0 && --m_standCounter <= 0) ClearStanding();
        if (m_tailCounter > 0 && ++m_tailCounter > 8) m_tailCounter = 0;
        if (m_sprintCounter > 0 && ++m_sprintCounter > 300) m_sprintCounter = 0;

        m_eatAnimO = m_eatAnim;
        if (IsEating()) {
            m_eatAnim += (1.0f - m_eatAnim) * 0.4f + 0.05f;
            if (m_eatAnim > 1.0f) m_eatAnim = 1.0f;
        } else {
            m_eatAnim += (0.0f - m_eatAnim) * 0.4f - 0.05f;
            if (m_eatAnim < 0.0f) m_eatAnim = 0.0f;
        }

        m_standAnimO = m_standAnim;
        if (IsStanding()) {
            m_eatAnim = 0.0f;
            m_eatAnimO = m_eatAnim;
            m_standAnim += (1.0f - m_standAnim) * 0.4f + 0.05f;
            if (m_standAnim > 1.0f) m_standAnim = 1.0f;
        } else {
            m_allowStandSliding = false;
            m_standAnim += (0.8f * m_standAnim * m_standAnim * m_standAnim
                            - m_standAnim) * 0.6f - 0.05f;
            if (m_standAnim < 0.0f) m_standAnim = 0.0f;
        }

        m_mouthAnimO = m_mouthAnim;
        if (m_openMouth) {
            m_mouthAnim += (1.0f - m_mouthAnim) * 0.7f + 0.05f;
            if (m_mouthAnim > 1.0f) m_mouthAnim = 1.0f;
        } else {
            m_mouthAnim += (0.0f - m_mouthAnim) * 0.7f - 0.05f;
            if (m_mouthAnim < 0.0f) m_mouthAnim = 0.0f;
        }
    }

    void AbstractHorse::AiStep() {
        // MC aiStep: the tail roll comes FIRST and runs on both sides — each
        // side rolls its own 1-in-200, exactly as MC's shared aiStep does.
        if (m_level && m_level->Random().NextInt(200) == 0) m_tailCounter = 1;

        Animal::AiStep();

        if (m_level && !m_level->IsClientSide() && IsAlive()) {
            // MC: the slow ambient self-heal.
            if (m_level->Random().NextInt(900) == 0 && deathTime == 0) {
                Heal(1.0f);
            }

            if (CanEatGrass()) {
                // MC: `!isEating() && !isVehicle() && nextInt(300) == 0` —
                // a ridden horse (a player in the seat included) never
                // stops to graze, and draws no roll.
                if (!IsEating() && !IsVehicle() && m_level->Random().NextInt(300) == 0) {
                    const IBlockAccess* blocks = m_level->Blocks();
                    const glm::ivec3 p = BlockPosition();
                    if (blocks && blocks->GetBlock(p.x, p.y - 1, p.z) == BlockID::Grass) {
                        SetEating(true);
                    }
                }
                if (IsEating() && ++m_eatingCounter > 50) {
                    m_eatingCounter = 0;
                    SetEating(false);
                }
            }

            // MC followMommy (bred foals): navigation.createPath(mommy, 0) —
            // a path computed and never followed, and nothing in 26.3 sets
            // the bred flag outside a save; it has no effect to reproduce.
        }
    }

    float AbstractHorse::GetEatAnim(float partialTick) const {
        return Mth::Lerp(partialTick, m_eatAnimO, m_eatAnim);
    }

    float AbstractHorse::GetStandAnim(float partialTick) const {
        return Mth::Lerp(partialTick, m_standAnimO, m_standAnim);
    }

    float AbstractHorse::GetMouthAnim(float partialTick) const {
        return Mth::Lerp(partialTick, m_mouthAnimO, m_mouthAnim);
    }

    // ── Horse ──────────────────────────────────────────────────────────────

    const char* Horse::VariantTexture(int variant) {
        // HorseRenderer.LOCATION_BY_VARIANT, in Variant id order.
        static constexpr const char* kTextures[kVariantCount] = {
            "assets/textures/entity/horse/horse_white.png",
            "assets/textures/entity/horse/horse_creamy.png",
            "assets/textures/entity/horse/horse_chestnut.png",
            "assets/textures/entity/horse/horse_brown.png",
            "assets/textures/entity/horse/horse_black.png",
            "assets/textures/entity/horse/horse_gray.png",
            "assets/textures/entity/horse/horse_darkbrown.png",
        };
        return kTextures[WrapId(variant, kVariantCount)];
    }

    const char* Horse::MarkingsTexture(int markings) {
        // HorseMarkingLayer.LOCATION_BY_MARKINGS, in Markings id order.
        static constexpr const char* kTextures[kMarkingsCount] = {
            "",
            "assets/textures/entity/horse/horse_markings_white.png",
            "assets/textures/entity/horse/horse_markings_whitefield.png",
            "assets/textures/entity/horse/horse_markings_whitedots.png",
            "assets/textures/entity/horse/horse_markings_blackdots.png",
        };
        return kTextures[WrapId(markings, kMarkingsCount)];
    }

    bool Horse::CanMate(const Animal& other) const {
        // MC Horse.canMate.
        if (&other == this) return false;
        if (other.GetType() != EntityTypeId::Donkey && other.GetType() != EntityTypeId::Horse) return false;
        const auto* partner = dynamic_cast<const AbstractHorse*>(&other);
        return partner && CanParent() && partner->CanParent();
    }

    std::unique_ptr<Animal> Horse::CreateBaby() {
        // MC Horse.getBreedOffspring. Without a partner in flight (a spawn
        // egg used on the horse) the partner is the horse itself, as MC's
        // spawnOffspringFromSpawnEgg passes it.
        const AbstractHorse& partner = BreedPartner() ? *BreedPartner() : *this;
        if (partner.GetType() == EntityTypeId::Donkey) {
            auto baby = std::make_unique<Mule>(m_level);
            SetOffspringAttributes(*this, partner, *baby);
            return baby;
        }
        auto baby = std::make_unique<Horse>(m_level);
        if (const auto* horsePartner = dynamic_cast<const Horse*>(&partner); horsePartner && m_level) {
            JavaRandom& rng = m_level->Random();
            const int selectSkin = rng.NextInt(9);
            int variant;
            if (selectSkin < 4)      variant = GetVariantId();
            else if (selectSkin < 8) variant = horsePartner->GetVariantId();
            else                     variant = rng.NextInt(kVariantCount);
            const int selectMarking = rng.NextInt(5);
            int markings;
            if (selectMarking < 2)      markings = GetMarkingsId();
            else if (selectMarking < 4) markings = horsePartner->GetMarkingsId();
            else                        markings = rng.NextInt(kMarkingsCount);
            baby->SetVariantAndMarkings(variant, markings);
            SetOffspringAttributes(*this, partner, *baby);
        }
        return baby;
    }

    std::shared_ptr<SpawnGroupData>
    Horse::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Horse.finalizeSpawn: the herd shares its first member's coat;
        // each horse rolls its own markings.
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            int variant;
            if (const auto* data = dynamic_cast<const HorseGroupData*>(groupData.get())) {
                variant = data->variant;
            } else {
                variant = rng.NextInt(kVariantCount);
                groupData = std::make_shared<HorseGroupData>(variant);
            }
            SetVariantAndMarkings(variant, rng.NextInt(kMarkingsCount));
        }
        return AbstractHorse::FinalizeSpawn(reason, std::move(groupData));
    }

    void Horse::RandomizeAttributes(JavaRandom& rng) {
        // MC Horse.randomizeAttributes: MAX_HEALTH, MOVEMENT_SPEED,
        // JUMP_STRENGTH — in that order (the RNG stream).
        m_attributes.SetBaseValue(Attribute::MaxHealth, static_cast<double>(GenerateMaxHealth(rng)));
        m_attributes.SetBaseValue(Attribute::MovementSpeed, GenerateSpeed(rng));
        m_attributes.SetBaseValue(Attribute::JumpStrength, GenerateJumpStrength(rng));
    }

    void Horse::PlayGallopSound(const SoundType& type) {
        AbstractHorse::PlayGallopSound(type);
        if (m_level && m_level->Random().NextInt(10) == 0) {
            PlaySound(IsBaby() ? SoundEvents::HORSE_BREATHE_BABY : SoundEvents::HORSE_BREATHE,
                      type.GetVolume() * 0.6f, type.GetPitch());
        }
    }

    // ── Donkey / Mule (AbstractChestedHorse) ───────────────────────────────

    bool Donkey::CanMate(const Animal& other) const {
        // MC Donkey.canMate.
        if (&other == this) return false;
        if (other.GetType() != EntityTypeId::Donkey && other.GetType() != EntityTypeId::Horse) return false;
        const auto* partner = dynamic_cast<const AbstractHorse*>(&other);
        return partner && CanParent() && partner->CanParent();
    }

    std::unique_ptr<Animal> Donkey::CreateBaby() {
        // MC Donkey.getBreedOffspring: a mule from a horse, else a donkey,
        // with the inherited attributes either way.
        const AbstractHorse& partner = BreedPartner() ? *BreedPartner() : *this;
        std::unique_ptr<AbstractHorse> baby;
        if (partner.GetType() == EntityTypeId::Horse) baby = std::make_unique<Mule>(m_level);
        else                                           baby = std::make_unique<Donkey>(m_level);
        SetOffspringAttributes(*this, partner, *baby);
        return baby;
    }

    void Donkey::RandomizeAttributes(JavaRandom& rng) {
        // MC AbstractChestedHorse.randomizeAttributes: MAX_HEALTH only.
        m_attributes.SetBaseValue(Attribute::MaxHealth, static_cast<double>(GenerateMaxHealth(rng)));
    }

    std::unique_ptr<Animal> Mule::CreateBaby() {
        // MC Mule.getBreedOffspring (unreachable through breeding — a mule
        // never mates — but a spawn egg on a mule makes one).
        return std::make_unique<Mule>(m_level);
    }

    void Mule::RandomizeAttributes(JavaRandom& rng) {
        m_attributes.SetBaseValue(Attribute::MaxHealth, static_cast<double>(GenerateMaxHealth(rng)));
    }

    // ── SkeletonHorse / ZombieHorse ────────────────────────────────────────

    const char* SkeletonHorse::GetSwimSound() const {
        // MC SkeletonHorse.getSwimSound.
        if (onGround) {
            if (!IsVehicle()) return SoundEvents::SKELETON_HORSE_STEP_WATER;
            ++m_gallopSoundCounter;
            if (m_gallopSoundCounter > 5 && m_gallopSoundCounter % 3 == 0) {
                return SoundEvents::SKELETON_HORSE_GALLOP_WATER;
            }
            if (m_gallopSoundCounter <= 5) return SoundEvents::SKELETON_HORSE_STEP_WATER;
        }
        return SoundEvents::SKELETON_HORSE_SWIM;
    }

    void SkeletonHorse::RandomizeAttributes(JavaRandom& rng) {
        // MC SkeletonHorse.randomizeAttributes: JUMP_STRENGTH only.
        m_attributes.SetBaseValue(Attribute::JumpStrength, GenerateJumpStrength(rng));
    }

    UseResult ZombieHorse::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC ZombieHorse.interact: setPersistenceRequired, then (through
        // Mob.interact) ZombieHorse.mobInteract — Horse's shape without the
        // golden-dandelion clause (a zombie horse cannot be age-locked).
        if (m_level && !m_level->IsClientSide()) SetPersistenceRequired(true);
        const bool sneaking = IsSecondaryUseActive(player);
        const bool shouldOpenInventory = !IsBaby() && IsTamedHorse() && sneaking;
        if (!IsVehicle() && !shouldOpenInventory && !held.IsEmpty()) {
            if (IsFood(held.itemId)) {
                const UseResult fed = FedFood(player, held);
                LogHorseInteract(*this, held, sneaking, ConsumesAction(fed) ? "feed" : "feed-refused");
                return fed;
            }
            if (!IsTamedHorse()) {
                LogHorseInteract(*this, held, sneaking, "makeMad (non-food item, untamed)");
                MakeMad();
                return UseResult::Success;
            }
        }
        return BaseMobInteract(player, held);
    }

    bool ZombieHorse::IsMobControlled() const {
        // MC: getFirstPassenger() instanceof Mob.
        return dynamic_cast<const Mob*>(GetFirstPassenger()) != nullptr;
    }

    void ZombieHorse::RandomizeAttributes(JavaRandom& rng) {
        // MC ZombieHorse.randomizeAttributes: JUMP_STRENGTH, then
        // MOVEMENT_SPEED.
        m_attributes.SetBaseValue(Attribute::JumpStrength, GenerateZombieHorseJumpStrength(rng));
        m_attributes.SetBaseValue(Attribute::MovementSpeed, GenerateZombieHorseSpeed(rng));
    }

} // namespace Game

