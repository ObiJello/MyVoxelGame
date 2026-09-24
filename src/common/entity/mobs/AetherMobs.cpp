// File: src/common/entity/mobs/AetherMobs.cpp
#include "common/entity/mobs/AetherMobs.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ModMobNbt.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/RangedGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/AetherCreatures.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/crafting/RecipeManager.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <memory>

namespace Game {

    std::unique_ptr<Mob> MakeAetherMob(EntityTypeId type, EntityLevel* level) {
        switch (type) {
            case EntityTypeId::Phyg:          return std::make_unique<Phyg>(level);
            case EntityTypeId::FlyingCow:     return std::make_unique<FlyingCow>(level);
            case EntityTypeId::Sheepuff:      return std::make_unique<Sheepuff>(level);
            case EntityTypeId::Cockatrice:    return std::make_unique<Cockatrice>(level);
            case EntityTypeId::Zephyr:        return std::make_unique<Zephyr>(level);
            case EntityTypeId::Moa:           return std::make_unique<Moa>(level);
            case EntityTypeId::Aerbunny:      return std::make_unique<Aerbunny>(level);
            case EntityTypeId::Aerwhale:      return std::make_unique<Aerwhale>(level);
            case EntityTypeId::BlueSwet:      return std::make_unique<Swet>(EntityTypeId::BlueSwet, level);
            case EntityTypeId::GoldenSwet:    return std::make_unique<Swet>(EntityTypeId::GoldenSwet, level);
            case EntityTypeId::Whirlwind:     return std::make_unique<Whirlwind>(EntityTypeId::Whirlwind, level);
            case EntityTypeId::EvilWhirlwind: return std::make_unique<Whirlwind>(EntityTypeId::EvilWhirlwind, level);
            case EntityTypeId::AechorPlant:   return std::make_unique<AechorPlant>(level);
            case EntityTypeId::Mimic:         return std::make_unique<Mimic>(level);
            case EntityTypeId::Sentry:        return std::make_unique<Sentry>(level);
            case EntityTypeId::Valkyrie:      return std::make_unique<Valkyrie>(level);
            case EntityTypeId::FireMinion:    return std::make_unique<FireMinion>(level);
            default:                          return nullptr;
        }
    }

    // ══ Shared ═════════════════════════════════════════════════════════════

    namespace AetherIds {
        ItemID BlueBerry() {
            static const ItemID id = RecipeManager::ItemFromSlug("blue_berry");
            return id;
        }
        BlockID AetherGrassBlock() {
            static const BlockID id =
                BlockStates::FromSlug("aether_grass_block").Block();
            return id;
        }
        BlockID AetherDirt() {
            static const BlockID id = BlockStates::FromSlug("aether_dirt").Block();
            return id;
        }
        ItemID ItemBySlug(const char* slug) {
            return RecipeManager::ItemFromSlug(slug);
        }
    } // namespace AetherIds

    bool ClampWingedFall(LivingEntity& mob, double maxFall) {
        // MC WingedAnimal.tick / Cockatrice.tick / Moa.tick:
        //   fallSpeed = max(GRAVITY * -1.25, maxFall); clamp dy from below.
        const double gravity = mob.GetAttributeValue(Attribute::Gravity);
        const double fallSpeed = std::max(gravity * -1.25, maxFall);
        if (mob.velocity.y < fallSpeed) {
            mob.velocity.y = fallSpeed;
            mob.needsSync = true;   // MC hasImpulse
            return true;
        }
        return false;
    }

    std::unique_ptr<Arrow> MakePoisonNeedle(EntityLevel* level, LivingEntity& shooter) {
        // PoisonNeedle(level, shooter): AbstractArrow's shooter constructor
        // (eye line - 0.1), setBaseDamage(0.25), pickup DISALLOWED (arrows
        // are never picked up here), INEBRIATION 500 -> POISON 500 (see the
        // header).
        auto needle = std::make_unique<Arrow>(level);
        needle->SetOwner(&shooter);
        needle->position = glm::dvec3(shooter.position.x, shooter.GetEyeY() - 0.1,
                                      shooter.position.z);
        needle->SetBaseDamage(0.25);
        needle->AddEffect(MobEffectInstance(MobEffectId::Poison, 500, 0));
        return needle;
    }

    void WingedBirdAnim::Step(bool entityOnGround) {
        // WingedBird.animateWings.
        prevWingRotation = wingRotation;
        prevDestPos = destPos;
        if (!entityOnGround) {
            destPos = std::clamp(destPos + 0.45f, 0.01f, 1.0f);
        } else {
            destPos = 0.0f;
            wingRotation = 0.0f;
        }
        wingRotation += 3.0f;
    }

    float WingedBirdAnim::Get(float partialTick) const {
        // BipedBirdModel.setupWingsAnimation.
        const float rot = prevWingRotation + (wingRotation - prevWingRotation) * partialTick;
        const float dest = prevDestPos + (destPos - prevDestPos) * partialTick;
        return (std::sin(rot * 0.225f) + 1.0f) * dest;
    }

    namespace {
        // AetherAnimal.getWalkTargetValue.
        float AetherWalkTargetValue(const EntityLevel* level, const glm::ivec3& pos) {
            if (!level) return 0.0f;
            const IBlockAccess* blocks = level->Blocks();
            if (!blocks) return 0.0f;
            const BlockID grass = AetherIds::AetherGrassBlock();
            if (grass != BlockID::Air &&
                blocks->GetBlock(pos.x, pos.y - 1, pos.z) == grass) {
                return 10.0f;
            }
            return static_cast<float>(level->GetMaxLocalRawBrightness(pos.x, pos.y, pos.z)) - 0.5f;
        }

        // DyeItem.getDyeColor over the sixteen dye items (DyeColor order;
        // the generated item ids run white..black contiguously). -1 = not a
        // dye.
        int DyeColorOf(uint32_t itemId) {
            if (itemId >= Items::WhiteDye && itemId <= Items::BlackDye) {
                return static_cast<int>(itemId - Items::WhiteDye);
            }
            return -1;
        }
    } // namespace

    // ══ Mountable animals ══════════════════════════════════════════════════

    MountableAetherAnimal::MountableAetherAnimal(EntityTypeId type, EntityLevel* level)
        : Animal(type, level) {}

    void MountableAetherAnimal::Tick() {
        Animal::Tick();
        // MountableAnimal.tick: the synced flag follows onGround; a jump
        // clears it (jumpFromGround).
        if (onGround) m_entityOnGround = true;
    }

    void MountableAetherAnimal::JumpFromGround() {
        Animal::JumpFromGround();
        m_entityOnGround = false;
    }

    UseResult MountableAetherAnimal::MobInteract(LivingEntity& player, ItemStack& held) {
        // MountableAnimal.mobInteract: super first; when it did not consume
        // the click, a saddle equips (SaddleItem.interactLivingEntity:
        // isSaddleable && !isSaddled, then shrink).
        const UseResult result = Animal::MobInteract(player, held);
        if (ConsumesAction(result)) return result;
        if (held.itemId != Items::Saddle) return UseResult::Pass;
        if (!IsSaddleable() || m_saddled) return UseResult::Pass;
        if (m_level && !m_level->IsClientSide()) {
            m_saddled = true;   // equipSaddle — the saddle sound waits on audio
            held.count -= 1;
            if (held.count <= 0) held.Clear();
        }
        return UseResult::Success;
    }

    void MountableAetherAnimal::DropCustomDeathLoot(EntityLevel& level) {
        if (m_saddled) level.SpawnItemDrop(position, Items::Saddle, 1);
    }

    void MountableAetherAnimal::SaveModNbt(ModNbtOut& out) const {
        out.Bool("Saddled", m_saddled);
    }

    void MountableAetherAnimal::LoadModNbt(const ModNbtIn& in) {
        if (in.Has("Saddled")) m_saddled = in.Bool("Saddled", false);
    }

    float MountableAetherAnimal::GetWalkTargetValue(const glm::ivec3& pos) const {
        return AetherWalkTargetValue(m_level, pos);
    }

    // ══ Winged animals ═════════════════════════════════════════════════════

    WingedAetherAnimal::WingedAetherAnimal(EntityTypeId type, EntityLevel* level)
        : MountableAetherAnimal(type, level) {}

    bool WingedAetherAnimal::IsFood(uint32_t itemId) const {
        const ItemID berry = AetherIds::BlueBerry();
        return berry != Items::Air && itemId == berry;
    }

    void WingedAetherAnimal::Tick() {
        MountableAetherAnimal::Tick();
        // WingedAnimal.tick — the glide (MC skips it while a rider crouches;
        // nothing rides here).
        if (ClampWingedFall(*this)) SetEntityOnGround(false);
    }

    int WingedAetherAnimal::GetMaxFallDistance() const {
        return onGround ? MountableAetherAnimal::GetMaxFallDistance() : 14;
    }

    // ── Phyg ───────────────────────────────────────────────────────────────

    void Phyg::CreateAttributes(AttributeMap& out) {
        // Aether Phyg.createMobAttributes (see the header on TEMPT_RANGE).
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     10.0);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Phyg::Phyg(EntityLevel* level) : WingedAetherAnimal(EntityTypeId::Phyg, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    std::unique_ptr<Animal> Phyg::CreateBaby() {
        return std::make_unique<Phyg>(m_level);
    }

    void Phyg::RegisterGoals() {
        // Aether Phyg.registerGoals.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.25));
        m_goalSelector.AddGoal(3, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(4, std::make_unique<TemptGoal>(this, 1.2, false));
        m_goalSelector.AddGoal(5, std::make_unique<FollowParentGoal>(this, 1.1));
        // FallingRandomStrollGoal(this, 1.0) -> WaterAvoidingRandomStrollGoal.
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
    }

    // ── Flying cow ─────────────────────────────────────────────────────────

    void FlyingCow::CreateAttributes(AttributeMap& out) {
        // Aether FlyingCow.createMobAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     10.0);
        out.Register(Attribute::MovementSpeed,  0.2);
    }

    FlyingCow::FlyingCow(EntityLevel* level)
        : WingedAetherAnimal(EntityTypeId::FlyingCow, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    std::unique_ptr<Animal> FlyingCow::CreateBaby() {
        return std::make_unique<FlyingCow>(m_level);
    }

    UseResult FlyingCow::MobInteract(LivingEntity& player, ItemStack& held) {
        // FlyingCow.mobInteract: bucket + grown -> milk bucket
        // (ItemUtils.createFilledResult), else MountableAnimal's.
        if (held.itemId == Items::Bucket && !IsBaby()) {
            if (m_level && !m_level->IsClientSide()) {
                m_level->CreateFilledResult(player, held, ItemStack(Items::MilkBucket, 1));
            }
            return UseResult::Success;
        }
        return WingedAetherAnimal::MobInteract(player, held);
    }

    void FlyingCow::RegisterGoals() {
        // Aether FlyingCow.registerGoals.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 2.0));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.25, false));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.25));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
    }

    // ══ Sheepuff ═══════════════════════════════════════════════════════════

    void Sheepuff::CreateAttributes(AttributeMap& out) {
        // Aether Sheepuff.createMobAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,      8.0);
        out.Register(Attribute::MovementSpeed, 0.23);
    }

    Sheepuff::Sheepuff(EntityLevel* level) : Animal(EntityTypeId::Sheepuff, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool Sheepuff::IsFood(uint32_t itemId) const {
        const ItemID berry = AetherIds::BlueBerry();
        return berry != Items::Air && itemId == berry;
    }

    std::unique_ptr<Animal> Sheepuff::CreateBaby() {
        auto baby = std::make_unique<Sheepuff>(m_level);
        baby->SetColor(GetColor());   // see the header on the colour mix
        return baby;
    }

    float Sheepuff::GetWalkTargetValue(const glm::ivec3& pos) const {
        return AetherWalkTargetValue(m_level, pos);
    }

    uint8_t Sheepuff::RandomSheepuffColor(JavaRandom& rng) {
        // Sheepuff.getRandomSheepuffColor (DyeColor ordinals).
        const int i = rng.NextInt(100);
        if (i < 5)  return 3;    // LIGHT_BLUE
        if (i < 10) return 9;    // CYAN
        if (i < 15) return 5;    // LIME
        if (i < 18) return 6;    // PINK
        return rng.NextInt(500) == 0 ? 10 : 0;   // PURPLE : WHITE
    }

    std::shared_ptr<SpawnGroupData>
    Sheepuff::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (m_level) SetColor(RandomSheepuffColor(m_level->Random()));
        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    void Sheepuff::RegisterGoals() {
        // Aether Sheepuff.registerGoals.
        auto eat = std::make_unique<EatAetherGrassGoal>(this);
        m_eatGoal = eat.get();
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.25));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.1, false));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.1));
        m_goalSelector.AddGoal(5, std::move(eat));
        // FallingRandomStrollGoal(this, 1.0) -> WaterAvoidingRandomStrollGoal.
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
    }

    void Sheepuff::CustomServerAiStep() {
        // Sheepuff.customServerAiStep: the goal's counter onto the entity.
        m_eatAnimationTick = m_eatGoal ? m_eatGoal->GetEatAnimationTick() : 0;
        Animal::CustomServerAiStep();
    }

    void Sheepuff::AiStep() {
        if (m_level && m_level->IsClientSide()) {
            m_eatAnimationTick = std::max(0, m_eatAnimationTick - 1);
        }
        Animal::AiStep();
    }

    void Sheepuff::HandleEntityEvent(uint8_t id) {
        if (id == 10) {
            m_eatAnimationTick = 40;
            return;
        }
        Animal::HandleEntityEvent(id);
    }

    void Sheepuff::Tick() {
        Animal::Tick();
        // Sheepuff.tick: a puffed sheepuff floats down — fall capped at
        // max(gravity * -0.625, -0.05), and checkSlowFallDistance clears the
        // fall distance.
        if (m_puffed) {
            const double gravity = GetAttributeValue(Attribute::Gravity);
            const double fallSpeed = std::max(gravity * -0.625, -0.05);
            if (velocity.y < fallSpeed) {
                velocity.y = fallSpeed;
                needsSync = true;
            }
            ResetFallDistance();
        }
    }

    void Sheepuff::JumpFromGround() {
        Animal::JumpFromGround();
        // Sheepuff.jumpFromGround: push(0, 1.8, 0) while puffed.
        if (m_puffed) {
            velocity.y += 1.8;
            needsSync = true;
        }
    }

    bool Sheepuff::CauseFallDamage(double fallDist, float damageMultiplier) {
        // Sheepuff.calculateFallDamage: 0 while puffed.
        if (m_puffed) return false;
        return Animal::CauseFallDamage(fallDist, damageMultiplier);
    }

    int Sheepuff::GetMaxFallDistance() const {
        return (!onGround && m_puffed) ? 20 : Animal::GetMaxFallDistance();
    }

    void Sheepuff::OnEatBlock() {
        // Sheepuff.ate.
        ++m_amountEaten;
        if (!IsSheared()) {
            if (m_amountEaten >= 2) {   // "only puff up after eating twice"
                m_puffed = true;
                m_amountEaten = 0;
            }
        } else if (m_amountEaten == 1) {
            SetSheared(false);
            m_amountEaten = 0;
        }
        if (CanAgeUp()) AgeUp(60);
    }

    void Sheepuff::Shear() {
        // Sheepuff.shear: puffed -> unpuff, 2 + rand(3) wool; else shear,
        // 1 + rand(3). Each as its own stack of one, lifted out of the
        // fleece (spawnAtLocation(item, 1): one block up).
        if (!m_level) return;
        m_amountEaten = 0;
        int count;
        if (m_puffed) {
            m_puffed = false;
            count = 2;
        } else {
            SetSheared(true);
            count = 1;
        }
        JavaRandom& rng = m_level->Random();
        count += rng.NextInt(3);
        const uint32_t wool = Sheep::WoolItemForColor(GetColor());
        const glm::dvec3 dropPos = position + glm::dvec3(0.0, 1.0, 0.0);
        for (int i = 0; i < count; ++i) m_level->SpawnItemDrop(dropPos, wool, 1);
    }

    UseResult Sheepuff::MobInteract(LivingEntity& player, ItemStack& held) {
        // Sheepuff.mobInteract: dye an unshorn sheepuff (two dyes while
        // puffed).
        const int dye = DyeColorOf(held.itemId);
        if (dye >= 0 && !IsSheared() && GetColor() != dye) {
            const int cost = m_puffed ? 2 : 1;
            if (held.count >= cost) {
                if (m_level && !m_level->IsClientSide()) {
                    SetColor(static_cast<uint8_t>(dye));
                    held.count -= cost;
                    if (held.count <= 0) held.Clear();
                }
                return UseResult::Success;
            }
        }
        // IShearable (NeoForge's shears hook): ready -> shear.
        if (held.itemId == Items::Shears) {
            if (m_level && m_level->IsClientSide()) return UseResult::Consume;
            if (!ReadyForShearing()) return UseResult::Consume;
            Shear();
            return UseResult::Success;
        }
        return Animal::MobInteract(player, held);
    }

    void Sheepuff::DropCustomDeathLoot(EntityLevel& level) {
        // loot_table/entities/sheepuff/<colour>: one wool of the colour,
        // unsheared only, adults only (babies drop nothing at all).
        if (IsSheared() || IsBaby()) return;
        level.SpawnItemDrop(position, Sheep::WoolItemForColor(GetColor()), 1);
    }

    void Sheepuff::SaveModNbt(ModNbtOut& out) const {
        out.Bool("Sheared", IsSheared());
        out.Bool("Puffed", m_puffed);
        out.Byte("Color", static_cast<int8_t>(GetColor()));
    }

    void Sheepuff::LoadModNbt(const ModNbtIn& in) {
        if (in.Has("Sheared")) SetSheared(in.Bool("Sheared", false));
        if (in.Has("Puffed")) m_puffed = in.Bool("Puffed", false);
        if (in.Has("Color")) SetColor(static_cast<uint8_t>(in.Byte("Color", 0)));
    }

    float Sheepuff::GetHeadEatPositionScale(float partialTick) const {
        if (m_eatAnimationTick <= 0) return 0.0f;
        if (m_eatAnimationTick >= 4 && m_eatAnimationTick <= 36) return 1.0f;
        if (m_eatAnimationTick < 4) {
            return (static_cast<float>(m_eatAnimationTick) - partialTick) / 4.0f;
        }
        return -(static_cast<float>(m_eatAnimationTick - 40) - partialTick) / 4.0f;
    }

    float Sheepuff::GetHeadEatAngleScale(float partialTick) const {
        if (m_eatAnimationTick > 4 && m_eatAnimationTick <= 36) {
            const float f = (static_cast<float>(m_eatAnimationTick - 4) - partialTick) / 32.0f;
            return Mth::kPi / 5.0f + 0.21991149f * std::sin(f * 28.7f);
        }
        if (m_eatAnimationTick > 0) return Mth::kPi / 5.0f;
        return xRot * Mth::kDegToRad;
    }

    // ── EatAetherGrassGoal ─────────────────────────────────────────────────

    EatAetherGrassGoal::EatAetherGrassGoal(Sheepuff* mob) : m_mob(mob) {
        SetFlags(GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump);
    }

    bool EatAetherGrassGoal::CanUse() {
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        if (level->Random().NextInt(m_mob->IsBaby() ? 50 : 1000) != 0) return false;
        const IBlockAccess* blocks = level->Blocks();
        if (!blocks) return false;
        const glm::ivec3 p = m_mob->BlockPosition();
        if (blocks->GetBlock(p.x, p.y, p.z) == BlockID::ShortGrass) return true;
        const BlockID grass = AetherIds::AetherGrassBlock();
        return grass != BlockID::Air && blocks->GetBlock(p.x, p.y - 1, p.z) == grass;
    }

    void EatAetherGrassGoal::Start() {
        m_eatAnimationTick = AdjustedTickDelay(40);
        if (EntityLevel* level = m_mob->Level()) level->BroadcastEntityEvent(*m_mob, 10);
        m_mob->GetNavigation().Stop();
    }

    void EatAetherGrassGoal::Tick() {
        m_eatAnimationTick = std::max(0, m_eatAnimationTick - 1);
        if (m_eatAnimationTick != AdjustedTickDelay(4)) return;
        EntityLevel* level = m_mob->Level();
        if (!level) return;
        const IBlockAccess* blocks = level->Blocks();
        if (!blocks) return;
        const glm::ivec3 p = m_mob->BlockPosition();
        if (blocks->GetBlock(p.x, p.y, p.z) == BlockID::ShortGrass) {
            if (level->MobGriefing()) level->DestroyBlock(p, false);
            m_mob->OnEatBlock();
            return;
        }
        const BlockID grass = AetherIds::AetherGrassBlock();
        const glm::ivec3 below(p.x, p.y - 1, p.z);
        if (grass != BlockID::Air && blocks->GetBlock(below.x, below.y, below.z) == grass) {
            // levelEvent 2001 (the grass break particles) has no event here.
            if (level->MobGriefing() && AetherIds::AetherDirt() != BlockID::Air) {
                level->SetBlock(below, AetherIds::AetherDirt());
            }
            m_mob->OnEatBlock();
        }
    }

    // ══ Cockatrice ═════════════════════════════════════════════════════════

    void Cockatrice::CreateAttributes(AttributeMap& out) {
        // Aether Cockatrice.createMobAttributes (the monster supplier's extra
        // ATTACK_DAMAGE is never read by a ranged mob).
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     20.0);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Cockatrice::Cockatrice(EntityLevel* level)
        : Monster(EntityTypeId::Cockatrice, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Cockatrice::RegisterGoals() {
        // Aether Cockatrice.registerGoals.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<RangedAttackGoal>(this, this, 1.0, 60, 10.0f));
        // FallingRandomStrollGoal(this, 1.0) -> WaterAvoidingRandomStrollGoal.
        m_goalSelector.AddGoal(3, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 5.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    void Cockatrice::PerformRangedAttack(LivingEntity& target, float power) {
        // Cockatrice.performRangedAttack — AbstractSkeleton's aim with the
        // needle aimed at 0.75 of the target's height, velocity 1.0.
        (void)power;
        if (!m_level) return;
        auto needle = MakePoisonNeedle(m_level, *this);
        const double d0 = target.position.x - position.x;
        const double d1 = (target.position.y + target.GetBbHeight() * 0.75) - needle->position.y;
        const double d2 = target.position.z - position.z;
        const double d3 = std::sqrt(d0 * d0 + d2 * d2);
        const int difficultyId = static_cast<int>(m_level->GetDifficulty());
        needle->Shoot(d0, d1 + d3 * 0.2, d2, 1.0f, static_cast<float>(14 - difficultyId * 4));
        PlaySound("aether:entity.cockatrice.shoot", 2.0f, 1.0f / (m_level->Random().NextFloat() * 0.4f + 0.8f));
        m_level->AddFreshEntity(std::move(needle));
    }

    void Cockatrice::Tick() {
        Monster::Tick();
        if (!m_level || m_level->IsClientSide()) return;
        // Cockatrice.tick: the synced on-ground flag, then the glide (which
        // clears it while falling). The flap sound cooldown waits on audio.
        if (onGround) m_entityOnGround = true;
        if (ClampWingedFall(*this)) m_entityOnGround = false;
    }

    void Cockatrice::AiStep() {
        Monster::AiStep();
        m_wings.Step(m_entityOnGround);
    }

    int Cockatrice::GetMaxFallDistance() const {
        return onGround ? Monster::GetMaxFallDistance() : 14;
    }

    // ══ Zephyr ═════════════════════════════════════════════════════════════

    void Zephyr::CreateAttributes(AttributeMap& out) {
        // Aether Zephyr.createMobAttributes (FlyingMob.createMobAttributes
        // is Mob's) + the ghast's FLYING_SPEED, which GhastMoveControl
        // scales by 5/3 into the Zephyr's 0.1 impulse.
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,    5.0);
        out.Register(Attribute::FollowRange, 50.0);
        out.Register(Attribute::FlyingSpeed, 0.06);
    }

    Zephyr::Zephyr(EntityLevel* level) : Mob(EntityTypeId::Zephyr, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetMoveControl(std::make_unique<GhastMoveControl>(this));
        RegisterGoals();
    }

    void Zephyr::RegisterGoals() {
        // Aether Zephyr.registerGoals.
        m_goalSelector.AddGoal(5, std::make_unique<RandomFloatAroundGoal>(this));
        m_goalSelector.AddGoal(7, std::make_unique<GhastLookGoal>(this));
        m_goalSelector.AddGoal(7, std::make_unique<ZephyrShootSnowballGoal>(this));
        // NearestAttackableTargetGoal<>(this, Player.class, true, false).
        m_targetSelector.AddGoal(1, std::make_unique<NearestAttackableTargetGoal>(
                                        this, /*mustSee=*/true, /*mustReach=*/false));
    }

    void Zephyr::Travel(const glm::dvec3& input) {
        // FlyingMob.travel — the same travelFlying as Ghast::Travel.
        if (IsInWater()) {
            MoveRelative(0.02f, input);
            Move(velocity);
            velocity *= 0.8;
        } else if (IsInLava()) {
            MoveRelative(0.02f, input);
            Move(velocity);
            velocity *= 0.5;
        } else {
            MoveRelative(0.02f, input);
            Move(velocity);
            velocity *= 0.91;
        }
    }

    void Zephyr::AiStep() {
        Mob::AiStep();
        if (!m_level) return;
        if (!m_level->IsClientSide()) {
            // Zephyr.aiStep: gone below the floor (by 2) or over the top.
            if (position.y < m_level->GetMinY() - 2 ||
                position.y > m_level->GetMaxY() + 1) {
                Discard();
            }
            return;
        }
        // Zephyr.aiStep, client half: the charge puff and the tail sweep.
        m_cloudScale += m_cloudScaleAdd;
        m_tailRot += m_tailRotAdd;
        if (m_chargingClient) {
            m_cloudScaleAdd = 1;
        } else {
            m_cloudScaleAdd = 0;
            m_cloudScale = 0;
        }
        m_tailRotAdd = 0.015f;
        if (m_tailRot >= 2.0f * Mth::kPi) m_tailRot -= 2.0f * Mth::kPi;
    }

    void Zephyr::DropCustomDeathLoot(EntityLevel& level) {
        // loot_table/entities/zephyr: uniform(0, 2) cold aercloud.
        static const ItemID kColdAercloud = RecipeManager::ItemFromSlug("cold_aercloud");
        if (kColdAercloud == Items::Air) return;
        const int count = level.Random().NextInt(3);
        if (count > 0) level.SpawnItemDrop(position, kColdAercloud, count);
    }

    // ── ZephyrShootSnowballGoal ────────────────────────────────────────────

    bool ZephyrShootSnowballGoal::CanUse() {
        return m_zephyr->GetTarget() != nullptr;
    }

    void ZephyrShootSnowballGoal::Tick() {
        LivingEntity* target = m_zephyr->GetTarget();
        if (!target) return;
        EntityLevel* level = m_zephyr->Level();
        if (!level) return;

        if (target->DistanceToSqr(*m_zephyr) < 1600.0 &&
            m_zephyr->GetSensing().HasLineOfSight(*target)) {
            m_zephyr->SetChargeTime(m_zephyr->GetChargeTime() + 1);
            JavaRandom& rng = level->Random();
            if (m_zephyr->GetChargeTime() == 10) {
                // The warble: the ambient voice.
                const char* ambient = m_zephyr->GetAmbientSound();
                m_zephyr->PlaySound(ambient ? ambient : "", m_zephyr->GetSoundVolume(),
                                    (rng.NextFloat() - rng.NextFloat()) * 0.2f + 1.0f);
            } else if (m_zephyr->GetChargeTime() == 20) {
                m_zephyr->PlaySound("aether:entity.zephyr.shoot", m_zephyr->GetSoundVolume(),
                                    (rng.NextFloat() - rng.NextFloat()) * 0.2f + 1.0f);
                const glm::vec3 look = Mth::ViewVector(m_zephyr->xRot, m_zephyr->yRot);
                const double halfHeight = m_zephyr->GetBbHeight() * 0.5;
                const double accelX = target->position.x -
                    (m_zephyr->position.x + static_cast<double>(look.x) * 4.0);
                const double accelY = (target->position.y + target->GetBbHeight() * 0.5) -
                    (0.5 + m_zephyr->position.y + halfHeight);
                const double accelZ = target->position.z -
                    (m_zephyr->position.z + static_cast<double>(look.z) * 4.0);

                auto snowball = std::make_unique<ZephyrSnowball>(level);
                snowball->SetOwnerAndDirection(*m_zephyr, glm::dvec3(accelX, accelY, accelZ));
                snowball->position = glm::dvec3(
                    m_zephyr->position.x + static_cast<double>(look.x) * 4.0,
                    m_zephyr->position.y + halfHeight + 0.5,
                    m_zephyr->position.z + static_cast<double>(look.z) * 4.0);
                level->AddFreshEntity(std::move(snowball));
                m_zephyr->SetChargeTime(-40);
            }
        } else if (m_zephyr->GetChargeTime() > 0) {
            m_zephyr->SetChargeTime(m_zephyr->GetChargeTime() - 1);
        }
    }

    // ── ZephyrSnowball ─────────────────────────────────────────────────────

    void ZephyrSnowball::Tick() {
        // ZephyrSnowball.tick: 400 ticks airborne, then gone.
        if (!onGround) ++m_ticksInAir;
        if (m_ticksInAir > 400 && m_level && !m_level->IsClientSide()) {
            Discard();
            return;
        }
        HurtingProjectile::Tick();
    }

    void ZephyrSnowball::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        (void)hit;
        if (!m_level || m_level->IsClientSide()) return;
        // ZephyrSnowball.onHitEntity: no damage — +0.5 up, 1.5x the
        // snowball's horizontal speed. AddDeltaMovement is MC's push that
        // reaches a player's client (the Aether's ZephyrSnowballHitPacket).
        target.AddDeltaMovement(glm::dvec3(velocity.x * 1.5, 0.5, velocity.z * 1.5));
    }

    void ZephyrSnowball::OnHit(const HitResult& hit) {
        HurtingProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) Discard();
    }

} // namespace Game
