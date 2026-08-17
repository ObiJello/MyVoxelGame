// File: src/common/entity/mobs/Animals.cpp
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/biome/Biomes.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
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
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.25));
        m_goalSelector.AddGoal(3, std::make_unique<BreedGoal>(this, 1.0));
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

    void Sheep::FinalizeSpawn() {
        // MC Sheep.finalizeSpawn — which EntityType.create calls for EVERY
        // spawn reason, natural and spawn-egg and /summon alike. That is why a
        // spawn egg in vanilla can produce a grey or brown sheep (and, once in
        // roughly 600, a pink one) rather than always white.
        if (!m_level) return;
        std::string_view biome = "plains";
        if (const IBlockAccess* blocks = m_level->Blocks()) {
            const glm::ivec3 p = BlockPosition();
            biome = BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name;
        }
        SetColor(RandomSpawnColor(m_level->Random(), biome));
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
        if (!IsEffectiveAi() || !IsAlive() || IsBaby()) return;
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

} // namespace Game
