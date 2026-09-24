// File: src/common/entity/mobs/TwilightMobs.cpp
#include "common/entity/mobs/TwilightMobs.hpp"
#include "common/entity/mobs/TwilightCreatures.hpp"

#include "common/entity/mobs/Animals.hpp"   // Sheep::WoolItemForColor / RandomSpawnColor
#include "common/world/biome/Biomes.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    std::unique_ptr<Mob> MakeTwilightMob(EntityTypeId type, EntityLevel* level) {
        switch (type) {
            case EntityTypeId::Deer:         return std::make_unique<Deer>(level);
            case EntityTypeId::Boar:         return std::make_unique<Boar>(level);
            case EntityTypeId::BighornSheep: return std::make_unique<Bighorn>(level);
            case EntityTypeId::TinyBird:     return std::make_unique<TinyBird>(level);
            case EntityTypeId::Kobold:       return std::make_unique<Kobold>(level);
            case EntityTypeId::Redcap:       return std::make_unique<Redcap>(level);
            case EntityTypeId::Raven:        return std::make_unique<Raven>(level);
            case EntityTypeId::Penguin:      return std::make_unique<Penguin>(level);
            default:                         return MakeTwilightCreature(type, level);
        }
    }

    namespace {

        // A block's data-pack tag membership, by the block's registry slug.
        bool BlockHasTag(BlockID id, const char* tag) {
            if (id == BlockID::Air) return false;
            const Block& def = BlockRegistry::Get(id);
            if (def.registrySlug.empty()) return false;
            const std::vector<std::string>& tags =
                DataTags::TagsFor(DataTags::Registry::Block, def.registrySlug);
            return std::binary_search(tags.begin(), tags.end(), std::string(tag));
        }

    } // namespace

    // ══ GrazingAnimal ══════════════════════════════════════════════════════

    GrazingAnimal::GrazingAnimal(EntityTypeId type, EntityLevel* level)
        : Animal(type, level) {}

    void GrazingAnimal::AddEatBlockGoal(int priority) {
        auto eat = std::make_unique<EatBlockGoal>(this);
        m_eatBlockGoal = eat.get();
        m_goalSelector.AddGoal(priority, std::move(eat));
    }

    void GrazingAnimal::CustomServerAiStep() {
        Animal::CustomServerAiStep();
        // MC Sheep.customServerAiStep: the goal's counter onto the entity.
        m_eatAnimationTick = m_eatBlockGoal ? m_eatBlockGoal->GetEatAnimationTick() : 0;
    }

    void GrazingAnimal::HandleEntityEvent(uint8_t id) {
        // MC Sheep.handleEntityEvent(10): the client's own 40-tick animation.
        if (id == 10) {
            m_eatAnimationTick = EatBlockGoal::kEatAnimationTicks;
            return;
        }
        Animal::HandleEntityEvent(id);
    }

    void GrazingAnimal::AiStep() {
        // MC Sheep.aiStep — the client counts the dip down itself.
        if (m_level && m_level->IsClientSide()) {
            m_eatAnimationTick = std::max(0, m_eatAnimationTick - 1);
        }
        Animal::AiStep();
    }

    void GrazingAnimal::OnEatBlock() {
        // MC Sheep.ate (the wool half waits on shearing).
        if (CanAgeUp()) AgeUp(60);
    }

    float GrazingAnimal::GetHeadEatPositionScale(float partialTick) const {
        // MC Sheep.getHeadEatPositionScale.
        if (m_eatAnimationTick <= 0) return 0.0f;
        if (m_eatAnimationTick >= 4 &&
            m_eatAnimationTick <= EatBlockGoal::kEatAnimationTicks - 4) {
            return 1.0f;
        }
        if (m_eatAnimationTick < 4) {
            return (static_cast<float>(m_eatAnimationTick) - partialTick) / 4.0f;
        }
        return -(static_cast<float>(m_eatAnimationTick - EatBlockGoal::kEatAnimationTicks) -
                 partialTick) / 4.0f;
    }

    float GrazingAnimal::GetHeadEatAngleScale(float partialTick) const {
        // MC Sheep.getHeadEatAngleScale.
        if (m_eatAnimationTick > 4 &&
            m_eatAnimationTick <= EatBlockGoal::kEatAnimationTicks - 4) {
            const float t = (static_cast<float>(m_eatAnimationTick) - 4.0f - partialTick) / 32.0f;
            return Mth::kPi / 5.0f + 0.21991149f * std::sin(t * 28.7f);
        }
        if (m_eatAnimationTick > 0) return Mth::kPi / 5.0f;
        return xRot * Mth::kDegToRad;
    }

    // ══ Deer ═══════════════════════════════════════════════════════════════

    void Deer::CreateAttributes(AttributeMap& out) {
        // TF Deer.registerAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     10.0);
        out.Register(Attribute::MovementSpeed,  0.2);
    }

    Deer::Deer(EntityLevel* level) : Animal(EntityTypeId::Deer, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Deer::IsFood(uint32_t itemId) const {
        return itemId == Items::Wheat || itemId == Items::Apple;
    }

    std::unique_ptr<Animal> Deer::CreateBaby() {
        return std::make_unique<Deer>(m_level);
    }

    void Deer::RegisterGoals() {
        // TF Deer.registerGoals, verbatim priorities and speeds.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 2.0));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.25, false));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.25));
        // AvoidEntityGoal<>(this, Player.class, 16.0F, 1.5D, 1.8D).
        m_goalSelector.AddGoal(4, std::make_unique<AvoidEntityGoal>(this, 16.0f, 1.5, 1.8));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
    }

    // ══ Boar ═══════════════════════════════════════════════════════════════

    void Boar::CreateAttributes(AttributeMap& out) {
        // TF Boar.registerAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     10.0);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Boar::Boar(EntityLevel* level) : Animal(EntityTypeId::Boar, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Boar::IsFood(uint32_t itemId) const {
        return itemId == Items::Carrot || itemId == Items::Potato || itemId == Items::Beetroot;
    }

    std::unique_ptr<Animal> Boar::CreateBaby() {
        return std::make_unique<Boar>(m_level);
    }

    void Boar::RegisterGoals() {
        // TF Boar.registerGoals — the pig's shape, carrot-on-a-stick tempt
        // included.
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

    // ══ Bighorn ═══════════════════════════════════════════════════════════

    namespace {
        // DyeColor ordinals (MC declaration order).
        enum : uint8_t {
            kWhite = 0, kOrange, kMagenta, kLightBlue, kYellow, kLime, kPink,
            kGray, kLightGray, kCyan, kPurple, kBlue, kBrown, kGreen, kRed,
            kBlack,
        };

        // MC DyeColor.getMixedColor: the crafting result of the two parents'
        // dyes when some recipe makes one dye from exactly those two, else a
        // coin flip between them. The two-dye recipes in the vanilla data
        // pack (data/minecraft/recipe/*_dye_from_*): orange = red + yellow,
        // light blue = blue + white, magenta = purple + pink, pink = red +
        // white, gray = black + white, light gray = gray + white, cyan =
        // blue + green, purple = red + blue, lime = green + white. (The
        // three- and four-dye recipes cannot match two ingredients.)
        uint8_t MixedDyeColor(uint8_t a, uint8_t b, JavaRandom& rng) {
            struct Mix { uint8_t x, y, out; };
            static constexpr Mix kMixes[] = {
                { kRed,    kYellow, kOrange },
                { kBlue,   kWhite,  kLightBlue },
                { kPurple, kPink,   kMagenta },
                { kRed,    kWhite,  kPink },
                { kBlack,  kWhite,  kGray },
                { kGray,   kWhite,  kLightGray },
                { kBlue,   kGreen,  kCyan },
                { kRed,    kBlue,   kPurple },
                { kGreen,  kWhite,  kLime },
            };
            for (const Mix& m : kMixes) {
                if ((a == m.x && b == m.y) || (a == m.y && b == m.x)) return m.out;
            }
            return rng.NextBool() ? a : b;
        }
    } // namespace

    void Bighorn::CreateAttributes(AttributeMap& out) {
        // MC Sheep.createAttributes (TF registers Bighorn::createAttributes,
        // i.e. the inherited sheep supplier).
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,      8.0);
        out.Register(Attribute::MovementSpeed, 0.23);
    }

    Bighorn::Bighorn(EntityLevel* level)
        : GrazingAnimal(EntityTypeId::BighornSheep, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Bighorn::IsFood(uint32_t itemId) const {
        return itemId == Items::Wheat;
    }

    std::unique_ptr<Animal> Bighorn::CreateBaby() {
        // Bighorn.getBreedOffspring: DyeColor.getMixedColor(this, partner).
        // With no partner (a spawn egg on an adult) the lamb keeps this one's.
        auto baby = std::make_unique<Bighorn>(m_level);
        if (m_breedPartner && m_level) {
            baby->SetColor(MixedDyeColor(GetColor(), m_breedPartner->GetColor(),
                                         m_level->Random()));
        } else {
            baby->SetColor(GetColor());
        }
        return baby;
    }

    void Bighorn::SpawnChildFromBreeding(Animal& partner) {
        // Canmate is same-type exact, so the partner is a Bighorn.
        m_breedPartner = partner.GetType() == EntityTypeId::BighornSheep
                             ? static_cast<Bighorn*>(&partner) : nullptr;
        GrazingAnimal::SpawnChildFromBreeding(partner);
        m_breedPartner = nullptr;
    }

    std::shared_ptr<SpawnGroupData>
    Bighorn::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        groupData = GrazingAnimal::FinalizeSpawn(reason, std::move(groupData));
        if (!m_level) return groupData;
        JavaRandom& rng = m_level->Random();
        // super.finalizeSpawn is MC Sheep's, which rolls a biome colour
        // (SheepColorSpawnRules) that TF then overwrites — the roll still
        // draws from the level's random, so it is made and discarded to keep
        // the stream MC's.
        std::string_view biome = "plains";
        if (const IBlockAccess* blocks = m_level->Blocks()) {
            const glm::ivec3 p = BlockPosition();
            biome = BiomeRegistry::Get(blocks->GetBiome(p.x, p.y, p.z)).name;
        }
        (void)Sheep::RandomSpawnColor(rng, biome);
        // Bighorn.getRandomFleeceColor.
        SetColor(rng.NextBool() ? kBrown : static_cast<uint8_t>(rng.NextInt(16)));
        return groupData;
    }

    UseResult Bighorn::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC Sheep.mobInteract — the shears branch (see Sheep::MobInteract
        // for the CONSUME cases and the durability note).
        if (held.itemId != Items::Shears) return GrazingAnimal::MobInteract(player, held);
        if (m_level && m_level->IsClientSide()) return UseResult::Consume;
        if (!ReadyForShearing()) return UseResult::Consume;
        Shear();
        return UseResult::Success;
    }

    void Bighorn::Shear() {
        // MC Sheep.shear: the shearing/sheep/<colour> table — uniform(1, 3)
        // rolls of that wool, each popped a metre up as its own stack of one.
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();
        const int rolls = 1 + rng.NextInt(3);
        const uint32_t wool = Sheep::WoolItemForColor(GetColor());
        const glm::dvec3 dropPos = position + glm::dvec3(0.0, 1.0, 0.0);
        for (int i = 0; i < rolls; ++i) m_level->SpawnItemDrop(dropPos, wool, 1);
        SetSheared(true);
    }

    void Bighorn::OnEatBlock() {
        // MC Sheep.ate: the fleece grows back, then the lamb's 60 seconds.
        SetSheared(false);
        GrazingAnimal::OnEatBlock();
    }

    float Bighorn::GetWalkTargetValue(const glm::ivec3& pos) const {
        if (m_level && m_level->Blocks() &&
            m_level->Blocks()->GetBlock(pos.x, pos.y - 1, pos.z) == BlockID::Podzol) {
            return 10.0f;
        }
        // Grass scores 10 there too; everything else the light-level cost.
        return Animal::GetWalkTargetValue(pos);
    }

    void Bighorn::DropCustomDeathLoot(EntityLevel& level) {
        // loot_table/entities/bighorn_sheep/<colour>: one wool of the fleece
        // colour, only while unsheared (MC's sheared condition on the pool).
        // Babies drop nothing (Animal.shouldDropLoot).
        if (IsBaby() || IsSheared()) return;
        level.SpawnItemDrop(position, Sheep::WoolItemForColor(GetColor()), 1);
    }

    void Bighorn::SaveModNbt(ModNbtOut& out) const {
        out.Byte("Color", static_cast<int8_t>(GetColor()));
        out.Bool("Sheared", IsSheared());
    }

    void Bighorn::LoadModNbt(const ModNbtIn& in) {
        if (in.Has("Color")) SetColor(static_cast<uint8_t>(in.Byte("Color", kBrown)));
        SetSheared(in.Bool("Sheared", false));
    }

    void Bighorn::RegisterGoals() {
        // MC Sheep.registerGoals, which Bighorn inherits unchanged.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.25));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.1, false));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.1));
        AddEatBlockGoal(5);
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
    }

    // ══ Birds ══════════════════════════════════════════════════════════════

    namespace {
        // TinyBird's two AvoidEntityGoals — one type each, as TF registers
        // them (Cat.class, Ocelot.class).
        const EntityTypeId kAvoidCat[]    = { EntityTypeId::Cat };
        const EntityTypeId kAvoidOcelot[] = { EntityTypeId::Ocelot };

        // #c:seeds — TFItemTags.TINY_BIRD_TEMPT_ITEMS and RAVEN_TEMPT_ITEMS
        // both resolve to it; the tempt goals and the spook test read it.
        constexpr ItemID kBirdSeeds[] = {
            Items::WheatSeeds, Items::BeetrootSeeds, Items::MelonSeeds,
            Items::PumpkinSeeds, Items::TorchflowerSeeds, Items::PitcherPod,
        };

        bool IsBirdSeed(uint32_t itemId) {
            for (const ItemID seed : kBirdSeeds) {
                if (itemId == seed) return true;
            }
            return false;
        }
    } // namespace

    // ── TFBird ─────────────────────────────────────────────────────────────

    TFBird::TFBird(EntityTypeId type, EntityLevel* level) : Animal(type, level) {}

    void TFBird::AiStep() {
        Animal::AiStep();
        // TF Bird.aiStep, verbatim.
        m_lastFlapLength = m_flapLength;
        m_lastFlapIntensity = m_flapIntensity;
        m_flapIntensity = static_cast<float>(m_flapIntensity + (onGround ? -1 : 4) * 0.3);
        m_flapIntensity = std::clamp(m_flapIntensity, 0.0f, 1.0f);
        if (!onGround && m_flapSpeed < 1.0f) m_flapSpeed = 1.0f;
        m_flapSpeed = static_cast<float>(m_flapSpeed * 0.9);
        // "don't fall as fast"
        if (!onGround && velocity.y < 0.0) velocity.y *= 0.6;
        m_flapLength += m_flapSpeed * 2.0f;
    }

    float TFBird::GetFlap(float partialTick) const {
        return m_lastFlapLength + (m_flapLength - m_lastFlapLength) * partialTick;
    }

    float TFBird::GetFlapIntensity(float partialTick) const {
        return m_lastFlapIntensity + (m_flapIntensity - m_lastFlapIntensity) * partialTick;
    }

    // ── FlyingBird ─────────────────────────────────────────────────────────

    FlyingBird::FlyingBird(EntityTypeId type, EntityLevel* level) : TFBird(type, level) {}

    void FlyingBird::RegisterFlyingBirdGoals() {
        // FlyingBird.registerGoals (Bird / Animal register none).
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(0, std::make_unique<PanicGoal>(this, 1.5));
        // TemptGoal(this, 1.0F, #tempt_items = #c:seeds, true) — one goal per
        // seed, the pig's two-tempt precedent (the engine goal takes one
        // item; IsFood stays false so feeding never courts).
        for (const ItemID seed : kBirdSeeds) {
            m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.0, true, seed));
        }
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
    }

    void FlyingBird::Tick() {
        TFBird::Tick();
        // FlyingBird.tick: "while we are flying, try to level out somewhat".
        if (!m_landed) velocity.y *= 0.6;
    }

    bool FlyingBird::IsLandableBlock(int x, int y, int z) const {
        // FlyingBird.isLandableBlock: not air, and leaves or a sturdy top
        // (a full collision cube is this engine's isFaceSturdy(UP)).
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return false;
        const BlockID block = blocks->GetBlock(x, y, z);
        if (block == BlockID::Air) return false;
        return BlockHasTag(block, "#minecraft:leaves") || blocks->IsBlockSolid(x, y, z);
    }

    void FlyingBird::CustomServerAiStep() {
        TFBird::CustomServerAiStep();
        if (!m_level) return;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return;
        JavaRandom& rnd = m_level->Random();
        const glm::ivec3 pos = BlockPosition();

        if (m_landed) {
            // FlyingBird.customServerAiStep, landed half.
            m_currentFlightTime = 0;
            const bool fluidBelow = blocks->IsBlockFluid(pos.x, pos.y - 1, pos.z);
            if (IsSpooked() || IsInWater() || fluidBelow ||
                (rnd.NextInt(200) == 0 && !IsLandableBlock(pos.x, pos.y - 1, pos.z))) {
                m_landed = false;
                PlaySound("twilightforest:entity.twilightforest.tiny_bird.takeoff", 0.05f, GetVoicePitch());
            }
            return;
        }

        // [VanillaCopy] Bat.customServerAiStep's flight, with TF's edits.
        ++m_currentFlightTime;
        if (m_hasTarget &&
            (blocks->GetBlock(m_targetPosition.x, m_targetPosition.y, m_targetPosition.z) !=
                 BlockID::Air ||
             m_targetPosition.y <= m_level->GetMinY())) {
            m_hasTarget = false;
        }

        // TF: no drowning birds.
        if (IsInWater() || blocks->IsBlockFluid(pos.x, pos.y - 1, pos.z)) {
            m_currentFlightTime = 0;
            velocity.y = 0.1;
        }

        const double tdx = m_targetPosition.x + 0.5 - position.x;
        const double tdy = m_targetPosition.y + 0.5 - position.y;
        const double tdz = m_targetPosition.z + 0.5 - position.z;
        // BlockPos.closerToCenterThan(position, 2.0).
        const bool reached = tdx * tdx + tdy * tdy + tdz * tdz < 4.0;
        if (!m_hasTarget || rnd.NextInt(30) == 0 || reached) {
            // TF: the Y shift climbs from 2 to 4 after 100 ticks aloft.
            const int yTarget = m_currentFlightTime < 100 ? 2 : 4;
            m_targetPosition = glm::ivec3(
                static_cast<int>(std::floor(position.x)) + rnd.NextInt(7) - rnd.NextInt(7),
                static_cast<int>(std::floor(position.y)) + rnd.NextInt(6) - yTarget,
                static_cast<int>(std::floor(position.z)) + rnd.NextInt(7) - rnd.NextInt(7));
            m_hasTarget = true;
        }

        const double dx = m_targetPosition.x + 0.5 - position.x;
        const double dy = m_targetPosition.y + 0.1 - position.y;
        const double dz = m_targetPosition.z + 0.5 - position.z;
        const auto signum = [](double v) { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); };
        velocity.x += (signum(dx) * 0.5 - velocity.x) * 0.1;
        velocity.y += (signum(dy) * 0.7 - velocity.y) * 0.1;
        velocity.z += (signum(dz) * 0.5 - velocity.z) * 0.1;
        needsSync = true;

        const float wanted = static_cast<float>(
            std::atan2(velocity.z, velocity.x) * Mth::kRadToDeg) - 90.0f;
        yRot += Mth::WrapDegrees(wanted - yRot);
        zza = 0.5f;

        // TF: 1-in-10 (Bat: 1-in-100) and a landable block instead of a
        // ceiling — then land, killing the vertical speed.
        if (rnd.NextInt(10) == 0 && IsLandableBlock(pos.x, pos.y - 1, pos.z)) {
            m_landed = true;
            velocity.y = 0.0;
        }
    }

    float FlyingBird::GetWalkTargetValue(const glm::ivec3& pos) const {
        // FlyingBird.getWalkTargetValue: "prefer standing on leaves".
        // BlockTags.SUBSTRATE_OVERWORLD is 26.3's name; this data pack's
        // equivalent is #minecraft:dirt.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return 0.0f;
        const BlockID below = blocks->GetBlock(pos.x, pos.y - 1, pos.z);
        if (BlockHasTag(below, "#minecraft:leaves")) return 200.0f;
        if (BlockHasTag(below, "#minecraft:logs")) return 15.0f;
        if (BlockHasTag(below, "#minecraft:dirt")) return 9.0f;
        return static_cast<float>(m_level->GetMaxLocalRawBrightness(pos.x, pos.y, pos.z)) - 0.5f;
    }

    // ── TinyBird ───────────────────────────────────────────────────────────

    void TinyBird::CreateAttributes(AttributeMap& out) {
        // TF TinyBird.registerAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     4.0);
        out.Register(Attribute::MovementSpeed, 0.2);
        out.Register(Attribute::StepHeight,    1.0);
    }

    TinyBird::TinyBird(EntityLevel* level) : FlyingBird(EntityTypeId::TinyBird, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void TinyBird::RegisterGoals() {
        // FlyingBird.registerGoals, then TinyBird's two avoid goals.
        RegisterFlyingBirdGoals();
        m_goalSelector.AddGoal(4, std::make_unique<AvoidEntityGoal>(this, kAvoidCat, 1,
                                                                    8.0f, 1.0, 1.25));
        m_goalSelector.AddGoal(4, std::make_unique<AvoidEntityGoal>(this, kAvoidOcelot, 1,
                                                                    8.0f, 1.0, 1.25));
    }

    bool TinyBird::IsSpooked() {
        // TF TinyBird.isSpooked: hurt, or the nearest player within 4 blocks
        // (getNearestPlayer(..., 4.0, true): spectators excluded) is holding
        // a tempt item. TF's isHolding also reads the offhand; this port has
        // no offhand slot, so the main hand is the whole test (TemptGoal's
        // own limitation).
        if (GetLastHurtByMob() != nullptr) return true;
        if (!m_level) return false;
        LivingEntity* player =
            m_level->GetNearestPlayer(position.x, position.y, position.z, 4.0);
        if (!player || player->IsSpectator() || !player->IsAlive()) return false;
        return IsBirdSeed(m_level->GetHeldItemId(*player));
    }

    std::shared_ptr<SpawnGroupData>
    TinyBird::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        groupData = FlyingBird::FinalizeSpawn(reason, std::move(groupData));
        // TinyBirdVariant.getVariant: every variant is valid in every biome
        // (no "biomes" key in any of the four), so a uniform pick in
        // registry order.
        if (m_level) m_variant = static_cast<uint8_t>(m_level->Random().NextInt(VariantCount));
        return groupData;
    }

    const char* TinyBird::VariantName(uint8_t v) {
        static constexpr const char* kNames[VariantCount] = { "blue", "brown", "gold", "red" };
        return v < VariantCount ? kNames[v] : kNames[Red];
    }

    uint8_t TinyBird::VariantFromName(std::string_view name) {
        // The mod saves the registry key ("twilightforest:blue").
        if (const size_t colon = name.find(':'); colon != std::string_view::npos) {
            name = name.substr(colon + 1);
        }
        for (uint8_t v = 0; v < VariantCount; ++v) {
            if (name == VariantName(v)) return v;
        }
        return Red;
    }

    void TinyBird::SaveModNbt(ModNbtOut& out) const {
        out.String("variant", std::string("twilightforest:") + VariantName(m_variant));
    }

    void TinyBird::LoadModNbt(const ModNbtIn& in) {
        if (in.Has("variant")) m_variant = VariantFromName(in.String("variant", "red"));
    }

    // ── Raven ──────────────────────────────────────────────────────────────

    void Raven::CreateAttributes(AttributeMap& out) {
        // TF Raven.registerAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     10.0);
        out.Register(Attribute::MovementSpeed,  0.2);
        out.Register(Attribute::StepHeight,     1.0);
    }

    Raven::Raven(EntityLevel* level) : FlyingBird(EntityTypeId::Raven, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Raven::RegisterGoals() {
        // Raven registers nothing of its own: FlyingBird.registerGoals.
        RegisterFlyingBirdGoals();
    }

    bool Raven::IsSpooked() {
        // TF Raven.isSpooked.
        return GetLastHurtByMob() != nullptr;
    }

    // ── Penguin ────────────────────────────────────────────────────────────

    void Penguin::CreateAttributes(AttributeMap& out) {
        // TF Penguin.registerAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     10.0);
        out.Register(Attribute::MovementSpeed,  0.2);
    }

    Penguin::Penguin(EntityLevel* level) : TFBird(EntityTypeId::Penguin, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Penguin::IsFood(uint32_t itemId) const {
        // #minecraft:fishes (data/minecraft/tags/item/fishes.json).
        return itemId == Items::Cod || itemId == Items::CookedCod ||
               itemId == Items::Salmon || itemId == Items::CookedSalmon ||
               itemId == Items::Pufferfish || itemId == Items::TropicalFish;
    }

    std::unique_ptr<Animal> Penguin::CreateBaby() {
        return std::make_unique<Penguin>(m_level);
    }

    void Penguin::RegisterGoals() {
        // TF Penguin.registerGoals, verbatim priorities and speeds.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.75));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 0.75, false));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.15));
        m_goalSelector.AddGoal(5, std::make_unique<RandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        // 7: LookAtPlayerGoal(this, Penguin.class, 5F, 0.02F) — the engine's
        // LookAtPlayerGoal watches players only; the penguin-at-penguin
        // glance is left out.
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
    }

    bool Penguin::CheckPenguinSpawnRules(EntityLevel& level, const glm::ivec3& pos) {
        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return false;
        return BlockHasTag(blocks->GetBlock(pos.x, pos.y - 1, pos.z), "#minecraft:ice");
    }

    // ══ Kobold ═════════════════════════════════════════════════════════════

    void Kobold::CreateAttributes(AttributeMap& out) {
        // TF Kobold.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     13.0);
        out.Register(Attribute::MovementSpeed, 0.28);
        out.Register(Attribute::AttackDamage,   4.0);
    }

    Kobold::Kobold(EntityLevel* level) : Monster(EntityTypeId::Kobold, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Kobold::RegisterGoals() {
        // TF Kobold.registerGoals — see the header for what is left out.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        // PanicOnFlockDeathGoal(this, 2.0F) -> PanicGoal(2.0).
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 2.0));
        m_goalSelector.AddGoal(3, std::make_unique<LeapAtTargetGoal>(this, 0.3f));
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        // KoboldAttackPlayerTarget: NearestAttackableTargetGoal(Player, true)
        // that stands down while the kobold holds bread (no bread here).
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    // ══ Redcap ═════════════════════════════════════════════════════════════

    void Redcap::CreateAttributes(AttributeMap& out) {
        // TF Redcap.registerAttributes, plus the held iron pickaxe's +3
        // (see the header).
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     20.0);
        out.Register(Attribute::MovementSpeed, 0.28);
        out.Register(Attribute::AttackDamage,   5.0);
    }

    Redcap::Redcap(EntityLevel* level) : Monster(EntityTypeId::Redcap, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Redcap::RegisterGoals() {
        // TF Redcap.registerGoals minus the TNT goals (priorities 1-3).
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

} // namespace Game
