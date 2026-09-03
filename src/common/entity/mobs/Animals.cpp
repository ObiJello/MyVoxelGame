// File: src/common/entity/mobs/Animals.cpp
#include "common/entity/mobs/Animals.hpp"
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
#include "common/entity/ai/goals/MoveToBlockGoal.hpp"
#include "common/entity/ai/goals/TamableGoals.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/projectile/LlamaSpit.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/ai/navigation/FlyingPathNavigation.hpp"
#include "common/entity/ai/navigation/AmphibiousPathNavigation.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/crafting/RecipeManager.hpp"
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
        return std::make_unique<Cow>(m_level);
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
        // brown-variant flower feeding needs the variant + stew effects.
        if (held.itemId == Items::Shears && ReadyForShearing()) {
            if (m_level && m_level->IsClientSide()) return UseResult::Success;
            Shear();
            // MC: itemStack.hurtAndBreak(1, ...) — no durability system yet
            // (the same note as Sheep::MobInteract).
            return UseResult::Success;
        }
        return Animal::MobInteract(player, held);
    }

    void Mooshroom::Shear() {
        if (!m_level) return;
        // MC MushroomCow.shear: convertTo(COW), then the SHEAR_MOOSHROOM
        // loot table — five mushrooms, each popped a metre up as its OWN
        // stack of one, MC's copyWithCount(1) loop. `this` is discarded by
        // the conversion, so everything it needs is copied out first.
        static const ItemID kRedMushroom =
            RecipeManager::ItemFromSlug("red_mushroom");
        EntityLevel* level = m_level;
        const glm::dvec3 dropPos = position + glm::dvec3(0.0, 1.0, 0.0);
        if (ConvertTo(std::make_unique<Cow>(level))) {
            // The EXPLOSION poof at the swap waits on particles.
            for (int i = 0; i < 5; ++i) {
                level->SpawnItemDrop(dropPos, kRedMushroom, 1);
            }
        }
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
        return std::make_unique<Pig>(m_level);
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

    void Sheep::Shear() {
        if (!m_level) return;

        // MC data/minecraft/loot_table/shearing/sheep/<color>.json — one pool,
        // `rolls: uniform(1, 3)`, a single entry of the matching wool.
        //
        // MC drops each roll as its OWN stack of one (`drop.copyWithCount(1)`
        // inside the per-count loop) rather than one stack of three, so the
        // wool scatters instead of landing in a pile. Item entities merge on
        // their own a moment later, which is exactly what vanilla looks like.
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
        // MC also does `itemStack.hurtAndBreak(1, player, hand)`. There is no
        // durability system yet (see ItemBehaviors' HurtAndBreak stub), so the
        // shears survive — the one deviation here, and it disappears the day
        // the DAMAGE component lands.
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
        // lets a player speed up a flock by keeping them on grass.
        if (IsBaby()) AgeUp(60);
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
        return std::make_unique<Chicken>(m_level);
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
        m_eggTime = m_level->Random().NextInt(6000) + 6000;
    }

    float Chicken::GetFlap(float partialTick) const {
        return m_oFlap + partialTick * (m_flap - m_oFlap);
    }

    float Chicken::GetFlapSpeed(float partialTick) const {
        return m_oFlapSpeed + partialTick * (m_flapSpeed - m_oFlapSpeed);
    }

    // ── Parrot ─────────────────────────────────────────────────────────────

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
        // MC Parrot.registerGoals, priority for priority:
        //   2 ParrotWanderGoal(1.0) — a WaterAvoidingRandomFlyingGoal whose
        //     getPosition prefers a perch beside leaves/logs (and land when in
        //     water); the base flyer wander stands in at MC's speed until the
        //     perch variant exists.
        //   3 LandOnOwnersShoulderGoal — SKIPPED: shoulder riding needs the
        //     player render side.
        //   3 FollowMobGoal(1.0, 3.0F, 7.0F) — SKIPPED: no FollowMobGoal
        //     class yet.
        m_goalSelector.AddGoal(0, std::make_unique<TamableAnimalPanicGoal>(
                                      this, this, 1.25));
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(2, std::make_unique<SitWhenOrderedToGoal>(this, this));
        m_goalSelector.AddGoal(2, std::make_unique<FollowOwnerGoal>(
                                      this, this, 1.0, 5.0f, 1.0f));
        m_goalSelector.AddGoal(2, std::make_unique<WaterAvoidingRandomFlyingGoal>(this, 1.0));
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

    } // namespace

    UseResult Parrot::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Parrot.mobInteract, verbatim shape. PARROT_EAT sound waits on
        // the sound system.
        const bool clientSide = m_level && m_level->IsClientSide();

        if (!IsTame() && IsParrotFood(held.itemId)) {
            UsePlayerItem(held);
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
            // and is then killed outright.
            UsePlayerItem(held);
            AddEffect(MobEffectInstance{MobEffectId::Poison, 900});
            Hurt(MobDamageSource::PlayerAttack,
                 std::numeric_limits<float>::max(), &player);
            return UseResult::Success;
        }

        if (!IsFlying() && IsTame() && IsOwnedBy(player)) {
            if (!clientSide) SetOrderedToSit(!IsOrderedToSit());
            return UseResult::Success;
        }

        return Animal::MobInteract(player, held);
    }

    void Parrot::AiStep() {
        // MC Parrot.aiStep also runs the jukebox party check and the
        // 1-in-400 imitate-nearby-mobs roll — both wait on the jukebox and
        // sound systems.
        Animal::AiStep();
        CalculateFlapping();
    }

    void Parrot::CalculateFlapping() {
        // MC Parrot.calculateFlapping, constants verbatim. isPassenger() is
        // always false here — no riding system.
        m_oFlap = m_flap;
        m_oFlapSpeed = m_flapSpeed;
        m_flapSpeed += (!onGround ? 4.0f : -1.0f) * 0.3f;
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
                    if (kEntityTypeTable[i].category == MobCategory::Monster) {
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

    std::unique_ptr<Animal> Rabbit::CreateBaby() {
        // MC getBreedOffspring rolls the baby's variant — the variant system
        // is not ported, so every rabbit is brown.
        return std::make_unique<Rabbit>(m_level);
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
        // MC Rabbit.setJumping — super sets the flag; the jump sound it plays
        // when true waits on the sound system.
        jumping = jump;
    }

    void Rabbit::StartJumping() {
        // MC Rabbit.startJumping, verbatim.
        SetJumping(true);
        m_jumpDuration = 10;
        m_jumpTicks = 0;
    }

    void Rabbit::CustomServerAiStep() {
        // MC Rabbit.customServerAiStep — the hop planner, verbatim minus the
        // EVIL-variant jump-at-target block (the variant system is skipped).
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
            // MC Rabbit.handleEntityEvent(1): spawnSprintParticle (no
            // particle system yet) + start the jump animation.
            m_jumpDuration = 10;
            m_jumpTicks = 0;
        } else {
            Animal::HandleEntityEvent(id);
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
        // MC PolarBear.playWarningSound — the growl itself (POLAR_BEAR_WARNING)
        // waits on the sound system; the 40-tick cadence is kept so the sound
        // drops in without behaviour changes.
        if (m_warningSoundTicks <= 0) {
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
    }

    namespace {
        // MC Wolf.PREY_SELECTOR: sheep, rabbit, fox.
        constexpr EntityTypeId kWolfPreyTypes[] = {
            EntityTypeId::Sheep, EntityTypeId::Rabbit, EntityTypeId::Fox,
        };
    }

    Wolf::Wolf(EntityLevel* level)
        : GenericAnimal(EntityTypeId::Wolf, level), NeutralMob(this),
          TamableAnimal(this) {
        // The base constructor registered the def-driven goal set, including
        // the def's UNGATED player hunt (the generator flattened MC's
        // isAngryAt selector away) and its skeleton prey. Rebuild the target
        // selector against Wolf.registerGoals so a wild wolf is neutral until
        // provoked, and add the combat + ownership goals the generic set
        // lacked.
        RegisterWolfGoals();
    }

    void Wolf::RegisterWolfGoals() {
        // MC Wolf.registerGoals, goal side (on top of the def set, which
        // already stands in for Float(1)/Panic(1)/Breed(7)/stroll(8)/
        // LookAtPlayer(10)/RandomLookAround(10); the def panic stands in for
        // TamableAnimalPanicGoal's flee half — its teleport-to-owner tick is
        // FollowOwnerGoal's machinery and fires from there):
        //   3 WolfAvoidEntityGoal(Llama, 24, 1.5, 1.5) — SKIPPED: its roll
        //     needs the llama's strength stat, which no llama here carries.
        m_goalSelector.AddGoal(2, std::make_unique<SitWhenOrderedToGoal>(this, this));
        // MC priority 9: BegGoal(this, 8.0F) — the interested head-tilt is on
        // the wire (anim byte bit 2) and the renderer feeds headRollAngle.
        m_goalSelector.AddGoal(9, std::make_unique<BegGoal>(this, 8.0f));
        m_goalSelector.AddGoal(4, std::make_unique<LeapAtTargetGoal>(this, 0.4f));
        m_goalSelector.AddGoal(5, std::make_unique<MeleeAttackGoal>(this, 1.0, true));
        m_goalSelector.AddGoal(6, std::make_unique<FollowOwnerGoal>(
                                      this, this, 1.0, 10.0f, 2.0f));

        // Target side, priority for priority.
        m_targetSelector.Clear();
        m_targetSelector.AddGoal(1, std::make_unique<OwnerHurtByTargetGoal>(this, this));
        m_targetSelector.AddGoal(2, std::make_unique<OwnerHurtTargetGoal>(this, this));
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

    void Wolf::AiStep() {
        // MC Wolf.aiStep: super, then updatePersistentAnger(level, true) on
        // the server. The trailing SetAggressive maps MC's synched isAngry()
        // onto the wire's aggressive bit — see the class comment. (The
        // wet-shake block waits on a rain query and its render pass.)
        GenericAnimal::AiStep();

        // MC Wolf.aiStep's interested spring (both sides, alive-gated): the
        // beg head-tilt eases 40% of the way to its target each tick.
        if (IsAlive()) {
            m_interestedAngleO = m_interestedAngle;
            if (m_interested) {
                m_interestedAngle += (1.0f - m_interestedAngle) * 0.4f;
            } else {
                m_interestedAngle += (0.0f - m_interestedAngle) * 0.4f;
            }
        }

        if (m_level && !m_level->IsClientSide()) {
            UpdatePersistentAnger(/*stayAngryIfTargetPresent=*/true);
            SetAggressive(IsAngry());
        }
    }

    float Wolf::GetHeadRollAngle(float partialTick) const {
        // MC Wolf.getHeadRollAngle: lerp(interestedAngleO..interestedAngle)
        // * 0.15π.
        return (m_interestedAngleO +
                (m_interestedAngle - m_interestedAngleO) * partialTick) *
               0.15f * 3.14159265358979323846f;
    }

    float Wolf::GetTailAngle() const {
        // MC Wolf.getTailAngle, verbatim.
        if (IsAngry()) return 1.5393804f;
        if (IsTame()) {
            const float maxHealth = GetMaxHealth();
            const float damageRatio = (maxHealth - GetHealth()) / maxHealth;
            return (0.55f - damageRatio * 0.4f) * 3.14159265358979323846f;
        }
        return 3.14159265358979323846f / 5.0f;
    }

    UseResult Wolf::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Wolf.mobInteract, verbatim shape. Skipped branches, each named:
        // the dye → collar recolour (no collar render layer — dye falls
        // through to the item hook like any unclaimed item) and the wolf
        // armor equip/repair pair (no wolf armor item).
        if (IsTame()) {
            if (IsFood(held.itemId) && GetHealth() < GetMaxHealth()) {
                Feed(held, 2.0f, 2.0f);
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

        if (m_level && !m_level->IsClientSide() && held.itemId == Items::Bone &&
            !IsAngry()) {
            UsePlayerItem(held);
            TryToTame(player);
            return UseResult::SuccessServer;
        }

        return Animal::MobInteract(player, held);
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

    bool Wolf::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Wolf.hurtServer: a hit wolf stands up before taking the damage.
        if (m_level && !m_level->IsClientSide()) SetOrderedToSit(false);
        return GenericAnimal::Hurt(source, amount, attacker);
    }

    bool Wolf::WantsToAttack(const LivingEntity& target,
                             const LivingEntity& owner) const {
        // MC Wolf.wantsToAttack. Reduced where the systems differ, each
        // named: no ArmorStand entity exists; the owner-vs-player
        // canHarmPlayer test is PvP-rules, and PvP is always on here.
        const EntityTypeId type = target.GetType();
        if (type == EntityTypeId::Creeper || type == EntityTypeId::Ghast) {
            return false;
        }
        if (const auto* wolf = dynamic_cast<const Wolf*>(&target)) {
            return !wolf->IsTame() || !wolf->IsOwnedBy(owner);
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

    std::unique_ptr<Animal> Wolf::CreateBaby() {
        // MC Wolf.getBreedOffspring: a tame parent's pup is born tame with
        // the same owner (the variant pick and collar mix ride their
        // systems).
        std::unique_ptr<Animal> baby = GenericAnimal::CreateBaby();
        if (auto* pup = dynamic_cast<Wolf*>(baby.get())) {
            if (IsTame()) {
                pup->SetOwnerUuid(GetOwnerUuid());
                pup->SetTame(true, /*includeSideEffects=*/true);
            }
        }
        return baby;
    }

    // ── Llama ──────────────────────────────────────────────────────────────

    Llama::Llama(EntityTypeId type, EntityLevel* level)
        : GenericAnimal(type, level) {
        // The GenericAnimal constructor registered the generic animal goal
        // set (def attributes, stroll, panic, look) — MC's llama-specific
        // additions go on top, priority-for-priority:
        //   1 RunAroundLikeCrazyGoal, 2 LlamaFollowCaravanGoal — SKIPPED:
        //     riding/taming and the caravan lead system do not exist.
        m_goalSelector.AddGoal(3, std::make_unique<RangedAttackGoal>(
                                      this, this, 1.25, 40, 20.0f));
        m_targetSelector.AddGoal(1, std::make_unique<LlamaHurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<LlamaAttackWolfGoal>(this));
    }

    void Llama::PerformRangedAttack(LivingEntity& target, float power) {
        (void)power;
        Spit(target);
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
        m_level->AddFreshEntity(std::move(spit));

        m_didSpit = true;
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
        return baby;
    }

    void Fox::RegisterGoals() {
        // MC Fox.registerGoals, priority for priority. The inert entries
        // (village stroll, berries, item search, defend-trusted) say why at
        // their declarations in FoxGoals.hpp.
        m_goalSelector.AddGoal(0, std::make_unique<FoxFloatGoal>(this));
        m_goalSelector.AddGoal(0, std::make_unique<ClimbOnTopOfPowderSnowGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<FaceplantGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<FoxPanicGoal>(this, 2.2));
        m_goalSelector.AddGoal(3, std::make_unique<FoxBreedGoal>(this, 1.0));
        // MC's per-goal avoid gates: !trusts(player) (trust never granted),
        // wolf !isTame (no taming), and !isDefending on all three — the last
        // lives in FoxAvoidEntityGoal.
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

            // MC rolls block-crack particles (levelEvent 2001) while
            // faceplanted — no block-particle path exists to carry it.
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
        if (m_level && !m_level->IsClientSide() && IsAlive() && IsEffectiveAi()) {
            // MC's mouth-item eating loop (ticksSinceEaten, finishUsingItem,
            // event 45 particles) rides the mob item system and is skipped
            // with it.
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
        // MC's 5% FOX_AGGRO bark while defending waits on the sound system.
    }

    std::shared_ptr<SpawnGroupData>
    Fox::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Fox.finalizeSpawn: variant by biome, third-and-later pack
        // members spawn as cubs, then the variant-ordered target goals.
        // (populateDefaultEquipmentSlots — the mouth trinket — rides the mob
        // item system.)
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

    std::unique_ptr<Animal> Turtle::CreateBaby() {
        return std::make_unique<Turtle>(m_level);
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
        // becomes gravid and both parents cool down. (The BRED_ANIMALS
        // advancement rides a system that does not exist.)
        const int32_t feeder = GetLoveCauseId() != -1 ? GetLoveCauseId()
                                                      : partner.GetLoveCauseId();
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
        // MC's constructor wiring: the panda's own move control.
        // (setCanPickUpLoot for adults rides the mob item system.)
        SetMoveControl(MakePandaMoveControl(this));
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
        // (playAttackSound waits on the sound system.)
        if (!IsAggressiveGene()) m_didBite = true;
        return Animal::DoHurtTarget(target);
    }

    UseResult Panda::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Panda.mobInteract, verbatim shape — pandas never take the
        // Animal base path (every non-food click returns PASS).
        if (IsScared()) return UseResult::Pass;

        if (IsOnBack()) {
            SetOnBack(false);
            return UseResult::Success;
        }

        if (!IsFood(held.itemId)) return UseResult::Pass;

        // MC: feeding a panda that has a grudge target sets gotBamboo — the
        // stand-down flag PandaHurtByTargetGoal reads.
        if (GetTarget() != nullptr) m_gotBamboo = true;

        const int age = GetAge();
        const bool clientSide = m_level && m_level->IsClientSide();
        if (IsBaby()) {
            UsePlayerItem(held);
            AgeUp(GetSpeedUpSecondsWhenFeeding(-age), /*forced=*/true);
        } else if (!clientSide && age == 0 && CanFallInLove()) {
            UsePlayerItem(held);
            SetInLove(&player);
        } else {
            if (clientSide) return UseResult::Pass;
            if (IsSitting() || IsInWater()) return UseResult::Pass;
            // MC also eat(true) and moves the fed bamboo into the panda's
            // MAINHAND to chew (dropping whatever it held) — the whole
            // mouth-item/eat layer is skipped with the mob-held-item system
            // (see IsEatingPanda); the sit itself is real.
            TryToSit();
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
            // MC: worried pandas sit out thunderstorms (eat(false) rides the
            // item layer; nothing to clear).
            if (m_level->IsThundering() && !IsInWater()) {
                Sit(true);
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
            // MC plays PANDA_CANT_BREED at counters 29 and 14 — sounds wait
            // on the sound system.
            --m_unhappyCounter;
        }

        // The sneeze clock runs on BOTH sides (the client's flag comes off
        // the anim byte); the sound and particle edges wait on their systems.
        if (IsSneezing()) {
            ++m_sneezeCounter;
            if (m_sneezeCounter > 20) {
                Sneeze(false);
                if (serverSide) {
                    // MC afterSneeze: startle every grounded adult panda
                    // within 10 blocks into a hop. (The sneeze particle and
                    // the slime-ball gift drop wait on their systems.)
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
                }
            }
        }

        if (IsRolling()) {
            HandleRoll();
        } else {
            m_rollCounter = 0;
        }

        if (IsSitting()) xRot = 0.0f;

        UpdateRamps();
        // MC handleEating / addEatingParticles ride the mob item system.
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
        // MC Cat.mobInteract, verbatim shape. The dye → collar branch is
        // skipped with the collar render layer (the cat_collar texture
        // exists; the renderer's texture table is per-type).
        const bool clientSide = m_level && m_level->IsClientSide();

        if (IsTame()) {
            if (IsOwnedBy(player)) {
                if (IsFood(held.itemId) && GetHealth() < GetMaxHealth()) {
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

    std::unique_ptr<Animal> Cat::CreateBaby() {
        // MC getBreedOffspring picks a parent's variant; unreachable while
        // CanMate is tame-gated, but correct the day taming lands.
        auto baby = std::make_unique<Cat>(m_level);
        baby->m_variant = m_variant;
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
        // MC Cat.tick → handleLieDown: the ramps run every tick on both
        // sides. (The purr sounds and the lying-on-sleeping-player scan wait
        // on the sound system and player sleep.)
        Animal::Tick();

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
        }
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    // ── AbstractHorse ──────────────────────────────────────────────────────

    AbstractHorse::AbstractHorse(EntityTypeId type, EntityLevel* level)
        : GenericAnimal(type, level) {
        // The GenericAnimal constructor registered the def-driven animal
        // set; MC's equine table replaces it wholesale (the Bat precedent
        // for a promoted class disowning its base goals).
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        RegisterHorseGoals();
    }

    void AbstractHorse::RegisterHorseGoals() {
        // MC AbstractHorse.registerGoals + addBehaviourGoals, priority for
        // priority. RunAroundLikeCrazyGoal's buck only arms under a player
        // rider (which cannot happen yet) — see its declaration.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<MountPanicGoal>(this, 1.2));
        m_goalSelector.AddGoal(1, std::make_unique<RunAroundLikeCrazyGoal>(this, 1.2));
        // MC BreedGoal(1.0, AbstractHorse.class) — cross-species pairs
        // (horse x donkey) need per-pair offspring; the port's same-species
        // CanMate covers the common case.
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        // MC TemptGoal(1.25, HORSE_TEMPT_ITEMS): golden carrot, golden
        // apple, enchanted golden apple — three goals, one per item, since
        // the shared goal carries one override item (the Pig precedent).
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(
                                      this, 1.25, false, Items::GoldenCarrot));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(
                                      this, 1.25, false, Items::GoldenApple));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(
                                      this, 1.25, false, Items::EnchantedGoldenApple));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                      this, 0.7));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        if (CanPerformRearing()) {
            m_goalSelector.AddGoal(9, std::make_unique<RandomStandGoal>(this));
        }
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

    int AbstractHorse::ModifyTemper(int amount) {
        // MC AbstractHorse.modifyTemper — clamped into [0, maxTemper].
        const int temper = std::clamp(GetTemper() + amount, 0, GetMaxTemper());
        SetTemper(temper);
        return temper;
    }

    UseResult AbstractHorse::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Horse.mobInteract / AbstractChestedHorse.mobInteract share this
        // shape. Riding-gated and skipped at their sites: the isVehicle
        // guard, the tamed secondary-use inventory screen, the body-armor
        // equip, the chest equip (chested family) — and doPlayerRide itself,
        // which is where MC finishes taming (temper vs a random roll while
        // being bucked, tameWithName). Feeding, and the temper it builds,
        // is the live half.
        if (!IsBaby()) {
            if (!held.IsEmpty()) {
                if (IsFood(held.itemId)) return FedFood(player, held);
                if (!IsTamedHorse()) {
                    // MC: a non-food click on an untamed horse makes it mad
                    // (in MC this precedes the mount attempt).
                    MakeMad();
                    return UseResult::Success;
                }
            }
            // MC: this.doPlayerRide(player); return SUCCESS — skipped with
            // player mounting; the click falls through instead.
        }
        return Animal::MobInteract(player, held);
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
        // hay_block and red_mushroom are BLOCK items, resolved by slug (the
        // Panda/Turtle food pattern). Note red_mushroom is in the table but
        // not in ItemTags.HORSE_FOOD — dead through mobInteract in MC too.
        static const ItemID kHayBlock = RecipeManager::ItemFromSlug("hay_block");
        static const ItemID kRedMushroom =
            RecipeManager::ItemFromSlug("red_mushroom");

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
        } else if (id == kHayBlock) {
            heal = 20.0f; ageUpSeconds = 180;
        } else if (id == Items::Apple) {
            heal = 3.0f; ageUpSeconds = 60; temper = 3;
        } else if (id == kRedMushroom) {
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

        if (IsBaby() && ageUpSeconds > 0) {
            // MC's HAPPY_VILLAGER particle waits on particles.
            if (!clientSide) {
                AgeUp(ageUpSeconds);
                itemUsed = true;
            }
        }

        if (temper > 0 && (itemUsed || !IsTamedHorse()) &&
            GetTemper() < GetMaxTemper() && !clientSide) {
            ModifyTemper(temper);
            itemUsed = true;
        }

        // MC closes with eating() — the chew sound + open-mouth flag, both
        // skipped (sound system; the mouth flag's only reader is the render
        // mouth ramp, itself skipped).
        return itemUsed;
    }

    void AbstractHorse::Tick() {
        Animal::Tick();

        // MC tick(): the counters, then the ramps — on BOTH sides (the
        // client's flags come off the anim byte). The mouth counter/ramp is
        // skipped: its only writers are eating-from-hand and rider
        // interactions.
        if (m_standCounter > 0 && --m_standCounter <= 0) ClearStanding();
        if (m_tailCounter > 0 && ++m_tailCounter > 8) m_tailCounter = 0;

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
            // MC's cubic ease-down (allowStandSliding rides riding).
            m_standAnim += (0.8f * m_standAnim * m_standAnim * m_standAnim
                            - m_standAnim) * 0.6f - 0.05f;
            if (m_standAnim < 0.0f) m_standAnim = 0.0f;
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
                if (!IsEating() && m_level->Random().NextInt(300) == 0) {
                    const IBlockAccess* blocks = m_level->Blocks();
                    const glm::ivec3 p = BlockPosition();
                    if (blocks && blocks->GetBlock(p.x, p.y - 1, p.z)
                                      == BlockID::Grass) {
                        SetEating(true);
                    }
                }
                if (IsEating() && ++m_eatingCounter > 50) {
                    m_eatingCounter = 0;
                    SetEating(false);
                }
            }

            // MC followMommy needs the BRED flag, which only taming's
            // breeding path sets — skipped with it.
        }
    }

    float AbstractHorse::GetEatAnim(float partialTick) const {
        return Mth::Lerp(partialTick, m_eatAnimO, m_eatAnim);
    }

    float AbstractHorse::GetStandAnim(float partialTick) const {
        return Mth::Lerp(partialTick, m_standAnimO, m_standAnim);
    }

} // namespace Game
