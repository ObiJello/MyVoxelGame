// File: src/common/entity/mobs/TwilightCreatures.cpp
#include "common/entity/mobs/TwilightCreatures.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/RangedGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game {

    std::unique_ptr<Mob> MakeTwilightCreature(EntityTypeId type, EntityLevel* level) {
        switch (type) {
            case EntityTypeId::Squirrel:      return std::make_unique<Squirrel>(level);
            case EntityTypeId::DwarfRabbit:   return std::make_unique<DwarfRabbit>(level);
            case EntityTypeId::HostileWolf:   return std::make_unique<HostileWolf>(level);
            case EntityTypeId::MistWolf:      return std::make_unique<MistWolf>(level);
            case EntityTypeId::WinterWolf:    return std::make_unique<WinterWolf>(level);
            case EntityTypeId::KingSpider:    return std::make_unique<KingSpider>(level);
            case EntityTypeId::MosquitoSwarm: return std::make_unique<MosquitoSwarm>(level);
            case EntityTypeId::SkeletonDruid: return std::make_unique<SkeletonDruid>(level);
            case EntityTypeId::Yeti:          return std::make_unique<Yeti>(level);
            default:                          return nullptr;
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

        // SpawnRuleContext::biomeAt, "" when the context carries none.
        std::string_view BiomeAt(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
            if (!ctx.biomeAt || !*ctx.biomeAt) return {};
            return (*ctx.biomeAt)(pos.x, pos.y, pos.z);
        }

        constexpr std::string_view kSnowyForest = "twilightforest:snowy_forest";

        // #c:seeds — TFItemTags.SQUIRREL_TEMPT_ITEMS.
        constexpr ItemID kSeeds[] = {
            Items::WheatSeeds, Items::BeetrootSeeds, Items::MelonSeeds,
            Items::PumpkinSeeds, Items::TorchflowerSeeds, Items::PitcherPod,
        };

        // AvoidEntityGoal classes, one type per goal as the mods register
        // them.
        const EntityTypeId kAvoidWolf[]   = { EntityTypeId::Wolf };
        const EntityTypeId kAvoidCat[]    = { EntityTypeId::Cat };
        const EntityTypeId kAvoidOcelot[] = { EntityTypeId::Ocelot };
        const EntityTypeId kAvoidFox[]    = { EntityTypeId::Fox };

        // MC Wolf.PREY_SELECTOR: sheep, rabbit, fox.
        const EntityTypeId kWolfPrey[] = {
            EntityTypeId::Sheep, EntityTypeId::Rabbit, EntityTypeId::Fox,
        };
        // AbstractSkeleton.class, flattened to its members.
        const EntityTypeId kSkeletons[] = {
            EntityTypeId::Skeleton, EntityTypeId::Stray, EntityTypeId::WitherSkeleton,
            EntityTypeId::Bogged, EntityTypeId::Parched, EntityTypeId::SkeletonDruid,
        };

        // EntitySelector.NO_CREATIVE_OR_SPECTATOR + LIVING_ENTITY_STILL_ALIVE.
        bool NotCreativeOrSpectatorAndAlive(const Entity& e) {
            return !e.IsCreative() && !e.IsSpectator() && e.IsAlive();
        }

        // MC AABB.clip over a segment: the entry fraction, or <0 on a miss.
        double ClipSegment(const AABBd& box, const glm::dvec3& from, const glm::dvec3& to) {
            double tMin = 0.0, tMax = 1.0;
            const glm::dvec3 d = to - from;
            for (int a = 0; a < 3; ++a) {
                if (std::abs(d[a]) < 1.0e-12) {
                    if (from[a] < box.min[a] || from[a] > box.max[a]) return -1.0;
                    continue;
                }
                double t0 = (box.min[a] - from[a]) / d[a];
                double t1 = (box.max[a] - from[a]) / d[a];
                if (t0 > t1) std::swap(t0, t1);
                tMin = std::max(tMin, t0);
                tMax = std::min(tMax, t1);
                if (tMin > tMax) return -1.0;
            }
            return tMin;
        }

        AABBd BoxOf(const Entity& e) {
            const double hw = e.GetBbWidth() * 0.5;
            return AABBd::FromMinMax(
                glm::dvec3(e.position.x - hw, e.position.y, e.position.z - hw),
                glm::dvec3(e.position.x + hw, e.position.y + e.GetBbHeight(), e.position.z + hw));
        }

    } // namespace

    // ══ Squirrel ═══════════════════════════════════════════════════════════

    void Squirrel::CreateAttributes(AttributeMap& out) {
        // TF Squirrel.registerAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     6.0);
        out.Register(Attribute::MovementSpeed, 0.3);
        out.Register(Attribute::StepHeight,    1.0);
    }

    Squirrel::Squirrel(EntityLevel* level) : Animal(EntityTypeId::Squirrel, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Squirrel::RegisterGoals() {
        // TF Squirrel.registerGoals, verbatim priorities and speeds.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.38));
        // TemptGoal(this, 1.0F, #c:seeds, true) — one goal per seed.
        for (const ItemID seed : kSeeds) {
            m_goalSelector.AddGoal(2, std::make_unique<TemptGoal>(this, 1.0, true, seed));
        }
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, 2.0f, 0.8, 1.4));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kAvoidWolf, 1, 8.0f, 0.8, 1.4));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kAvoidCat, 1, 8.0f, 0.8, 1.4));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kAvoidOcelot, 1, 8.0f, 0.8, 1.4));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kAvoidFox, 1, 8.0f, 0.8, 1.4));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.25));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
    }

    float Squirrel::GetWalkTargetValue(const glm::ivec3& pos) const {
        // TF Squirrel.getWalkTargetValue ("prefer standing on leaves");
        // SUBSTRATE_OVERWORLD is #minecraft:dirt in this data pack.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return 0.0f;
        const BlockID below = blocks->GetBlock(pos.x, pos.y - 1, pos.z);
        if (BlockHasTag(below, "#minecraft:leaves")) return 12.0f;
        if (BlockHasTag(below, "#minecraft:logs")) return 15.0f;
        if (BlockHasTag(below, "#minecraft:dirt")) return 10.0f;
        return static_cast<float>(m_level->GetMaxLocalRawBrightness(pos.x, pos.y, pos.z)) - 0.5f;
    }

    // ══ Dwarf rabbit ═══════════════════════════════════════════════════════

    namespace {
        ItemID Dandelion() {
            static const ItemID id = RecipeManager::ItemFromSlug("dandelion");
            return id;
        }

        const char* DwarfRabbitVariantName(uint8_t v) {
            static constexpr const char* kNames[DwarfRabbit::VariantCount] = {
                "brown", "dutch", "white" };
            return v < DwarfRabbit::VariantCount ? kNames[v] : kNames[DwarfRabbit::Brown];
        }
    } // namespace

    void DwarfRabbit::CreateAttributes(AttributeMap& out) {
        // TF DwarfRabbit.registerAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     3.0);
        out.Register(Attribute::MovementSpeed, 0.3);
        out.Register(Attribute::StepHeight,    1.0);
    }

    DwarfRabbit::DwarfRabbit(EntityLevel* level) : Animal(EntityTypeId::DwarfRabbit, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool DwarfRabbit::IsFood(uint32_t itemId) const {
        const ItemID dandelion = Dandelion();
        return itemId == Items::Carrot || itemId == Items::GoldenCarrot ||
               (dandelion != Items::Air && itemId == dandelion);
    }

    void DwarfRabbit::RegisterGoals() {
        // TF DwarfRabbit.registerGoals, verbatim priorities and speeds.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 2.0));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 0.8));
        m_goalSelector.AddGoal(2, std::make_unique<TemptGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, 2.0f, 0.8, 1.33));
        m_goalSelector.AddGoal(4, std::make_unique<AvoidEntityGoal>(this, kAvoidOcelot, 1, 8.0f, 0.8, 1.1));
        m_goalSelector.AddGoal(4, std::make_unique<AvoidEntityGoal>(this, kAvoidCat, 1, 8.0f, 0.8, 1.1));
        m_goalSelector.AddGoal(4, std::make_unique<AvoidEntityGoal>(this, kAvoidWolf, 1, 8.0f, 0.8, 1.1));
        m_goalSelector.AddGoal(4, std::make_unique<AvoidEntityGoal>(this, kAvoidFox, 1, 8.0f, 0.8, 1.1));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.8));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
    }

    std::unique_ptr<Animal> DwarfRabbit::CreateBaby() {
        // DwarfRabbit.getBreedOffspring: a random common variant (every
        // variant is common), unless the 19-in-20 roll inherits one parent's
        // (coin flip between the two).
        auto baby = std::make_unique<DwarfRabbit>(m_level);
        if (!m_level) return baby;
        JavaRandom& rng = m_level->Random();
        uint8_t variant = static_cast<uint8_t>(rng.NextInt(VariantCount));
        if (m_breedPartner && rng.NextInt(20) != 0) {
            variant = rng.NextBool() ? m_variant : m_breedPartner->m_variant;
        }
        baby->m_variant = variant;
        return baby;
    }

    void DwarfRabbit::SpawnChildFromBreeding(Animal& partner) {
        m_breedPartner = partner.GetType() == EntityTypeId::DwarfRabbit
                             ? static_cast<DwarfRabbit*>(&partner) : nullptr;
        Animal::SpawnChildFromBreeding(partner);
        m_breedPartner = nullptr;
    }

    float DwarfRabbit::GetWalkTargetValue(const glm::ivec3& pos) const {
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        if (!blocks) return 0.0f;
        const BlockID below = blocks->GetBlock(pos.x, pos.y - 1, pos.z);
        if (BlockHasTag(below, "#minecraft:leaves") || BlockHasTag(below, "#minecraft:logs")) {
            return -1.0f;
        }
        if (BlockHasTag(below, "#minecraft:dirt")) return 10.0f;
        return static_cast<float>(m_level->GetMaxLocalRawBrightness(pos.x, pos.y, pos.z)) - 0.5f;
    }

    std::shared_ptr<SpawnGroupData>
    DwarfRabbit::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Animal::FinalizeSpawn(reason, std::move(groupData));
        // DwarfRabbitVariant.getVariant: none of the three names biomes, so a
        // uniform pick in registry order.
        if (m_level) m_variant = static_cast<uint8_t>(m_level->Random().NextInt(VariantCount));
        return groupData;
    }

    void DwarfRabbit::SaveModNbt(ModNbtOut& out) const {
        out.String("variant", std::string("twilightforest:") + DwarfRabbitVariantName(m_variant));
    }

    void DwarfRabbit::LoadModNbt(const ModNbtIn& in) {
        if (!in.Has("variant")) return;
        std::string name = in.String("variant", "brown");
        if (const size_t colon = name.find(':'); colon != std::string::npos) name.erase(0, colon + 1);
        for (uint8_t v = 0; v < VariantCount; ++v) {
            if (name == DwarfRabbitVariantName(v)) { m_variant = v; return; }
        }
    }

    // ══ Hostile wolves ═════════════════════════════════════════════════════

    namespace {
        // HostileWolf.LeapGoal: LeapAtTargetGoal that holds the aggressive
        // flag while it runs, "so its face doesn't turn passive when it jumps".
        class HostileWolfLeapGoal : public LeapAtTargetGoal {
        public:
            HostileWolfLeapGoal(Mob* mob, float yd) : LeapAtTargetGoal(mob, yd), m_wolf(mob) {}
            void Start() override { LeapAtTargetGoal::Start(); m_wolf->SetAggressive(true); }
            void Stop() override { LeapAtTargetGoal::Stop(); m_wolf->SetAggressive(false); }
        private:
            Mob* m_wolf;
        };
    } // namespace

    void HostileWolf::CreateAttributes(AttributeMap& out) {
        // TF HostileWolf.registerAttributes: Mob.createMobAttributes + ...
        CreateMobAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.3);
        out.Register(Attribute::MaxHealth,    20.0);
        out.Register(Attribute::AttackDamage,  2.0);
    }

    HostileWolf::HostileWolf(EntityLevel* level) : HostileWolf(EntityTypeId::HostileWolf, level) {
        RegisterGoals();
    }

    HostileWolf::HostileWolf(EntityTypeId type, EntityLevel* level) : Monster(type, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // RegisterGoals is the concrete constructor's job.
    }

    void HostileWolf::RegisterGoals() {
        // TF HostileWolf.registerGoals, verbatim priorities.
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<HostileWolfLeapGoal>(this, 0.4f));
        m_goalSelector.AddGoal(3, std::make_unique<MeleeAttackGoal>(this, 1.0, true));
        m_goalSelector.AddGoal(4, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(5, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(5, std::make_unique<RandomLookAroundGoal>(this));

        // HurtByTargetGoal(this, HostileWolf.class): the class is an
        // exclusion list for alertOthers, which is never switched on.
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kWolfPrey, static_cast<int>(std::size(kWolfPrey)), false));
        // 4: Turtle.BABY_ON_LAND_SELECTOR — the engine's wolf skips it too
        // (no baby-on-land selector yet).
        m_targetSelector.AddGoal(5, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kSkeletons, static_cast<int>(std::size(kSkeletons)), false));
    }

    uint8_t HostileWolf::GetAnimStateByte() const {
        return GetTarget() != nullptr ? 1 : 0;
    }

    float HostileWolf::GetTailAngle() const {
        return HasTargetClient() ? 1.5393804f : Mth::kPi / 5.0f;
    }

    bool HostileWolf::CheckWolfSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
        // HostileWolf.checkWolfSpawnRules. The hedge-maze exemption
        // (LegacyLandmarkPlacements) has no landmark grid here yet.
        return ctx.level.GetDifficulty() != Difficulty::Peaceful &&
               Monster::IsDarkEnoughToSpawn(ctx.level, pos, ctx.rng) &&
               Monster::CheckAnyLightMonsterSpawnRules(ctx.level, ctx.reason, pos, ctx.rng);
    }

    // ── MistWolf ───────────────────────────────────────────────────────────

    void MistWolf::CreateAttributes(AttributeMap& out) {
        HostileWolf::CreateAttributes(out);
        out.Register(Attribute::MaxHealth,   30.0);
        out.Register(Attribute::AttackDamage, 6.0);
    }

    MistWolf::MistWolf(EntityLevel* level) : HostileWolf(EntityTypeId::MistWolf, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();   // HostileWolf's — MistWolf adds none
    }

    bool MistWolf::DoHurtTarget(Entity& target) {
        // TF MistWolf.doHurtTarget.
        if (!HostileWolf::DoHurtTarget(target)) return false;
        if (!m_level) return true;
        const glm::ivec3 p = BlockPosition();
        const float myBrightness =
            static_cast<float>(m_level->GetMaxLocalRawBrightness(p.x, p.y, p.z));
        auto* living = dynamic_cast<LivingEntity*>(&target);
        if (living && myBrightness < 0.10f) {
            int seconds = 7;
            if (m_level->GetDifficulty() == Difficulty::Easy) seconds = 0;
            else if (m_level->GetDifficulty() == Difficulty::Hard) seconds = 15;
            const IBlockAccess* blocks = m_level->Blocks();
            const bool inSolid = blocks && blocks->IsBlockSolid(p.x, p.y, p.z);
            if (seconds > 0 && !inSolid) {
                living->AddEffect(MobEffectInstance(MobEffectId::Blindness, seconds * 20, 0), this);
            }
        }
        return true;
    }

    // ── WinterWolf ─────────────────────────────────────────────────────────

    void WinterWolf::CreateAttributes(AttributeMap& out) {
        HostileWolf::CreateAttributes(out);
        out.Register(Attribute::MaxHealth,   30.0);
        out.Register(Attribute::AttackDamage, 6.0);
    }

    WinterWolf::WinterWolf(EntityLevel* level) : HostileWolf(EntityTypeId::WinterWolf, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void WinterWolf::RegisterGoals() {
        // TF WinterWolf.registerGoals (replaces HostileWolf's).
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<TFBreathAttackGoal>(this, 5.0f, 30, 0.1f));
        m_goalSelector.AddGoal(3, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    bool WinterWolf::IsBreathing() const {
        // Server: the goal's flag; client: the synced bit.
        if (m_level && m_level->IsClientSide()) return (m_animBits & 2) != 0;
        return m_breathing;
    }

    void WinterWolf::DoBreathAttack(Entity& target) {
        if (auto* living = dynamic_cast<LivingEntity*>(&target)) {
            living->Hurt(MobDamageSource::MobAttack, 2.0f, this);
        }
    }

    void WinterWolf::AiStep() {
        HostileWolf::AiStep();
        if (!IsBreathing() || !m_level) return;
        // TF playBreathSound, every breathing tick on both sides (the
        // client's copy is the null-except no-op).
        {
            JavaRandom& rng = m_level->Random();
            const float volume = rng.NextFloat() * 0.5f;
            PlaySound("twilightforest:entity.twilightforest.winter_wolf.shoot", volume, rng.NextFloat() * 0.5f);
        }
        if (!m_level->IsClientSide()) return;
        // WinterWolf.spawnBreathParticles: ten flakes from half a block ahead
        // of the muzzle along the look vector, spread by gaussian * 0.0075 *
        // (5..7.5) and thrown at 3..3.15x. TF's SNOW particle keeps 0.4 of
        // that velocity (SnowParticle's ctor); POOF — the nearest kind this
        // engine has — is thrown with the same 0.4x.
        JavaRandom& rng = m_level->Random();
        const glm::vec3 look = Mth::ViewVector(xRot, yRot);
        constexpr double kDist = 0.5;
        const double px = position.x + look.x * kDist;
        const double py = position.y + 1.25 + look.y * kDist;
        const double pz = position.z + look.z * kDist;
        for (int i = 0; i < 10; ++i) {
            double dx = look.x, dy = look.y, dz = look.z;
            const double spread = 5.0 + rng.NextDouble() * 2.5;
            const double velocity = 3.0 + rng.NextDouble() * 0.15;
            dx += rng.NextGaussian() * 0.0075 * spread;
            dy += rng.NextGaussian() * 0.0075 * spread;
            dz += rng.NextGaussian() * 0.0075 * spread;
            dx *= velocity; dy *= velocity; dz *= velocity;
            m_level->AddParticle(ParticleKind::Poof, px, py, pz, dx * 0.4, dy * 0.4, dz * 0.4);
        }
    }

    bool WinterWolf::CheckWinterWolfSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
        // TF WinterWolf.canSpawnHere, `a && b || c` as written.
        return (ctx.level.GetDifficulty() != Difficulty::Peaceful &&
                BiomeAt(ctx, pos) == kSnowyForest) ||
               Monster::IsDarkEnoughToSpawn(ctx.level, pos, ctx.rng);
    }

    // ── TFBreathAttackGoal ─────────────────────────────────────────────────

    TFBreathAttackGoal::TFBreathAttackGoal(WinterWolf* host, float range, int duration,
                                           float chance)
        : m_host(host), m_maxDuration(duration), m_attackChance(chance), m_breathRange(range) {
        SetFlags(GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump);
    }

    bool TFBreathAttackGoal::IsValidTarget(const LivingEntity& e) {
        return NotCreativeOrSpectatorAndAlive(e);
    }

    bool TFBreathAttackGoal::CanUse() {
        m_target = dynamic_cast<LivingEntity*>(m_host->GetLastHurtByMob());
        if (!m_target || m_host->DistanceTo(*m_target) > m_breathRange ||
            !m_host->GetSensing().HasLineOfSight(*m_target) || !IsValidTarget(*m_target)) {
            return false;
        }
        m_breathPos = m_target->GetEyePosition();
        EntityLevel* level = m_host->Level();
        return level && level->Random().NextFloat() < m_attackChance;
    }

    void TFBreathAttackGoal::Start() {
        m_durationLeft = m_maxDuration;
        m_host->SetBreathing(true);
    }

    bool TFBreathAttackGoal::CanContinueToUse() {
        return m_durationLeft > 0 && m_host->IsAlive() && m_target && m_target->IsAlive() &&
               m_host->DistanceTo(*m_target) <= m_breathRange &&
               m_host->GetSensing().HasLineOfSight(*m_target) && IsValidTarget(*m_target);
    }

    void TFBreathAttackGoal::Tick() {
        --m_durationLeft;
        m_host->GetLookControl().SetLookAt(m_breathPos);
        FaceVec(m_breathPos, 100.0f, 100.0f);
        if (m_maxDuration - m_durationLeft > 5) {
            if (Entity* target = GetHeadLookTarget()) {
                m_host->DoBreathAttack(*target);
                // gameEvent(PROJECTILE_SHOOT) — no game-event system.
            }
        }
    }

    void TFBreathAttackGoal::Stop() {
        m_durationLeft = 0;
        m_target = nullptr;
        m_host->SetBreathing(false);
    }

    void TFBreathAttackGoal::ClearReferenceTo(const Entity* entity) {
        if (m_target && static_cast<const Entity*>(m_target) == entity) m_target = nullptr;
    }

    Entity* TFBreathAttackGoal::GetHeadLookTarget() const {
        // BreathAttackGoal.getHeadLookTarget: the nearest pickable living
        // thing on a 30-block ray from 0.25 above the feet, searched in the
        // host's box pushed 3 blocks along the look and grown by 0.5.
        EntityLevel* level = m_host->Level();
        if (!level) return nullptr;
        constexpr double kRange = 30.0;
        constexpr double kOffset = 3.0;
        const glm::dvec3 src(m_host->position.x, m_host->position.y + 0.25, m_host->position.z);
        const glm::dvec3 look(Mth::ViewVector(m_host->xRot, m_host->yRot));
        const glm::dvec3 dest = src + look * kRange;

        const AABBd hostBox = BoxOf(*m_host);
        const glm::vec3 shift(look * kOffset);
        const AABB searchBox = AABB::FromMinMax(glm::vec3(hostBox.min) + shift - glm::vec3(0.5f),
                                                glm::vec3(hostBox.max) + shift + glm::vec3(0.5f));
        std::vector<Entity*> candidates;
        level->GetEntitiesInBox(searchBox, m_host, candidates);

        Entity* pointed = nullptr;
        double hitDist = 0.0;
        for (Entity* e : candidates) {
            if (!e || e == m_host || !e->IsPickable() || !NotCreativeOrSpectatorAndAlive(*e)) continue;
            if (!dynamic_cast<LivingEntity*>(e)) continue;
            const AABBd box = BoxOf(*e);   // getPickRadius is 0 for every mob
            const bool containsSrc = src.x >= box.min.x && src.x <= box.max.x &&
                                     src.y >= box.min.y && src.y <= box.max.y &&
                                     src.z >= box.min.z && src.z <= box.max.z;
            if (containsSrc) {
                if (0.0 < hitDist || hitDist == 0.0) {
                    pointed = e;
                    hitDist = 0.0;
                }
                continue;
            }
            const double t = ClipSegment(box, src, dest);
            if (t >= 0.0) {
                const double d = t * kRange;
                if (d < hitDist || hitDist == 0.0) {
                    pointed = e;
                    hitDist = d;
                }
            }
        }
        return pointed;
    }

    void TFBreathAttackGoal::FaceVec(const glm::dvec3& pos, float yawConstraint,
                                     float pitchConstraint) {
        // BreathAttackGoal.faceVec, verbatim (the pitch sign flip included).
        const auto updateRotation = [](float current, float target, float maxDelta) {
            float delta = Mth::WrapDegrees(target - current);
            delta = std::clamp(delta, -maxDelta, maxDelta);
            return current + delta;
        };
        const double xOffset = pos.x - m_host->position.x;
        const double zOffset = pos.z - m_host->position.z;
        const double yOffset = (m_host->position.y + 0.25) - pos.y;
        const double distance = std::sqrt(static_cast<float>(xOffset * xOffset + zOffset * zOffset));
        const float xyAngle = static_cast<float>((std::atan2(zOffset, xOffset) * 180.0) / Mth::kPi) - 90.0f;
        const float zdAngle = static_cast<float>(-((std::atan2(yOffset, distance) * 180.0) / Mth::kPi));
        m_host->xRot = -updateRotation(m_host->xRot, zdAngle, pitchConstraint);
        m_host->yRot = updateRotation(m_host->yRot, xyAngle, yawConstraint);
    }

    // ══ King spider ════════════════════════════════════════════════════════

    void KingSpider::CreateAttributes(AttributeMap& out) {
        // TF KingSpider.registerAttributes: Spider.createAttributes + ...
        Spider::CreateAttributes(out);
        out.Register(Attribute::MaxHealth,     30.0);
        out.Register(Attribute::MovementSpeed, 0.35);
        out.Register(Attribute::AttackDamage,   6.0);
    }

    KingSpider::KingSpider(EntityLevel* level) : Spider(EntityTypeId::KingSpider, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();   // Spider's — KingSpider adds none
    }

    void KingSpider::Tick() {
        // Spider.tick minus the climbing flag (onClimbable is false).
        Monster::Tick();
    }

    void KingSpider::Travel(const glm::dvec3& input) {
        Monster::Travel(input);
    }

    std::shared_ptr<SpawnGroupData>
    KingSpider::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Spider::FinalizeSpawn(reason, std::move(groupData));
        if (!m_level || m_level->IsClientSide()) return groupData;
        // KingSpider.finalizeSpawn: a JOCKEY druid on top of whatever already
        // rides (Spider's own 1-in-100 skeleton included).
        auto druid = std::make_unique<SkeletonDruid>(m_level);
        druid->position = position;
        druid->yRot = yRot;
        druid->yHeadRot = yRot;
        druid->yBodyRot = yRot;
        druid->FinalizeSpawn(SpawnReason::Jockey, nullptr);
        Entity* lastRider = this;
        while (!lastRider->GetPassengers().empty()) lastRider = lastRider->GetPassengers().front();
        SkeletonDruid* placed = druid.get();
        m_level->AddFreshEntity(std::move(druid));
        placed->StartRiding(*lastRider, /*force=*/true);
        return groupData;
    }

    glm::dvec3 KingSpider::GetPassengerAttachmentPoint(const Entity& passenger) const {
        return Spider::GetPassengerAttachmentPoint(passenger) +
               glm::dvec3(0.0, -GetBbHeight() * 0.15, 0.0);
    }

    // ══ Mosquito swarm ═════════════════════════════════════════════════════

    void MosquitoSwarm::CreateAttributes(AttributeMap& out) {
        // TF MosquitoSwarm.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,    12.0);
        out.Register(Attribute::MovementSpeed, 0.23);
        out.Register(Attribute::AttackDamage,  3.0);
        out.Register(Attribute::StepHeight,    2.1);
    }

    MosquitoSwarm::MosquitoSwarm(EntityLevel* level) : Monster(EntityTypeId::MosquitoSwarm, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void MosquitoSwarm::RegisterGoals() {
        // TF MosquitoSwarm.registerGoals.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(6, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    bool MosquitoSwarm::DoHurtTarget(Entity& target) {
        // TF MosquitoSwarm.doHurtTarget: Hunger 7 / 15 / 30 s.
        if (!Monster::DoHurtTarget(target)) return false;
        if (auto* living = dynamic_cast<LivingEntity*>(&target)) {
            int seconds = 15;
            if (m_level && m_level->GetDifficulty() == Difficulty::Easy) seconds = 7;
            else if (m_level && m_level->GetDifficulty() == Difficulty::Hard) seconds = 30;
            living->AddEffect(MobEffectInstance(MobEffectId::Hunger, seconds * 20, 0), this);
        }
        return true;
    }

    // ══ Skeleton druid ═════════════════════════════════════════════════════

    namespace {
        constexpr EntityTypeId kDruidWolfAvoid[]    = { EntityTypeId::Wolf };
        constexpr EntityTypeId kDruidGolemTargets[] = { EntityTypeId::IronGolem };
    } // namespace

    SkeletonDruid::SkeletonDruid(EntityLevel* level)
        : Skeleton(EntityTypeId::SkeletonDruid, level) {
        RegisterGoals();
    }

    void SkeletonDruid::RegisterGoals() {
        // AbstractSkeleton.registerGoals, with the weapon slot filled by
        // reassessWeaponGoal (TF's: the hoe's ranged goal).
        m_goalSelector.AddGoal(2, std::make_unique<RestrictSunGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<FleeSunGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kDruidWolfAvoid, 1,
                                                                    6.0f, 1.0, 1.2));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kDruidGolemTargets, 1, true));
        ReassessWeaponGoal();
    }

    void SkeletonDruid::ReassessWeaponGoal() {
        if (m_level && m_level->IsClientSide()) return;
        if (m_rangedGoal) { m_goalSelector.RemoveGoal(m_rangedGoal); m_rangedGoal = nullptr; }
        if (m_meleeGoal)  { m_goalSelector.RemoveGoal(m_meleeGoal);  m_meleeGoal = nullptr; }
        if (HoldsHoe()) {
            // TF: RangedAttackGoal(this, 1.25D, 60, 5.0F).
            auto ranged = std::make_unique<RangedAttackGoal>(this, this, 1.25, 60, 5.0f);
            m_rangedGoal = ranged.get();
            m_goalSelector.AddGoal(4, std::move(ranged));
        } else {
            // AbstractSkeleton.reassessWeaponGoal's non-bow branch: the melee
            // goal (MeleeAttackGoal(this, 1.2, false)).
            auto melee = std::make_unique<MeleeAttackGoal>(this, 1.2, false);
            m_meleeGoal = melee.get();
            m_goalSelector.AddGoal(4, std::move(melee));
        }
    }

    void SkeletonDruid::SetBaby(bool baby) {
        if (m_baby == baby) return;
        m_baby = baby;
        // TF's Zombie copy: SPEED_MODIFIER_BABY, +50% of base.
        if (baby) {
            m_attributes.AddModifier(Attribute::MovementSpeed,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::BabySpeedBoost), 0.5,
                                   AttributeOperation::AddMultipliedBase });
        } else {
            m_attributes.RemoveModifier(Attribute::MovementSpeed, ModifierId::BabySpeedBoost);
        }
        // populateDefaultEquipmentSlots gave a baby the stick, an adult the
        // hoe; the weapon goal follows (reassessWeaponGoal on setItemSlot).
        ReassessWeaponGoal();
    }

    int SkeletonDruid::GetXpReward() const {
        // SkeletonDruid.getBaseExperienceReward: x2.5 for a baby.
        const int base = Skeleton::GetXpReward();
        return m_baby ? static_cast<int>(static_cast<float>(base) * 2.5f) : base;
    }

    void SkeletonDruid::PerformRangedAttack(LivingEntity& target, float power) {
        (void)power;
        if (!m_level) return;
        if (!HoldsHoe()) return;   // the stick: nothing (TF's third branch)
        // SkeletonDruid.performRangedAttack: a NatureBolt from the eyes
        // (ThrowableItemProjectile(owner)), aimed at the target's eyes minus
        // 2.7 with a 0.2-per-block loft, velocity 0.6, inaccuracy 6, then
        // SKELETON_DRUID_SHOOT.
        PlaySound("twilightforest:entity.twilightforest.skeleton_druid.shoot", 1.0f,
                  1.0f / (m_level->Random().NextFloat() * 0.4f + 0.8f));
        auto bolt = std::make_unique<NatureBolt>(m_level);
        bolt->SetOwnerAndPosition(*this);
        const double tx = target.position.x - position.x;
        const double ty = target.position.y + target.GetEyeHeight() - 2.7 - position.y;
        const double tz = target.position.z - position.z;
        const float heightOffset = std::sqrt(static_cast<float>(tx * tx + tz * tz)) * 0.2f;
        bolt->Shoot(tx, ty + heightOffset, tz, 0.6f, 6.0f);
        m_level->AddFreshEntity(std::move(bolt));
    }

    void SkeletonDruid::SaveModNbt(ModNbtOut& out) const {
        out.Bool("IsBaby", m_baby);
    }

    void SkeletonDruid::LoadModNbt(const ModNbtIn& in) {
        SetBaby(in.Bool("IsBaby", false));
    }

    bool SkeletonDruid::CheckDruidSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
        // TF SkeletonDruid.checkDruidSpawnRules / isValidLightLevel.
        if (ctx.level.GetDifficulty() == Difficulty::Peaceful) return false;
        if (ctx.level.GetSkyBrightness(pos.x, pos.y, pos.z) > ctx.rng.NextInt(32)) return false;
        if (ctx.level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z) > ctx.rng.NextInt(12)) {
            return false;
        }
        return Monster::CheckAnyLightMonsterSpawnRules(ctx.level, ctx.reason, pos, ctx.rng);
    }

    // ── NatureBolt ─────────────────────────────────────────────────────────

    void NatureBolt::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        if (!m_level || m_level->IsClientSide()) return;
        // NatureBolt.onHitEntity: not the owner, not the owner's mount.
        Entity* owner = GetOwner();
        if (owner && (&target == owner || &target == owner->GetVehicle())) return;
        if (DealHitDamage(target, hit, MobDamageSource::Projectile, 2.0f, owner ? owner : this) &&
            m_level->GetDifficulty() != Difficulty::Peaceful) {
            const int seconds = m_level->GetDifficulty() == Difficulty::Hard ? 7 : 3;
            target.AddEffect(MobEffectInstance(MobEffectId::Poison, seconds * 20, 0),
                             owner ? owner : this);
        }
    }

    void NatureBolt::OnHitBlock(const HitResult& hit) {
        if (!m_level || m_level->IsClientSide() || !m_level->MobGriefing()) return;
        ILevelWrite* world = m_level->MutableBlocks();
        if (!world) return;
        // NatureBolt.onHitBlock: bone-meal a bonemealable block.
        const glm::ivec3 p = hit.blockPos;
        const BlockState state = world->GetBlockState(p.x, p.y, p.z);
        const Block& def = BlockRegistry::Get(state.Block());
        if (def.isValidBonemealTarget && def.performBonemeal &&
            def.isValidBonemealTarget(*world, p, state)) {
            def.performBonemeal(*world, p, state, m_level->Random());
        }
    }

    void NatureBolt::OnHit(const HitResult& hit) {
        ThrowableProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) {
            m_level->BroadcastEntityEvent(*this, 3);
            Discard();
        }
    }

    // ══ Yeti ═══════════════════════════════════════════════════════════════

    namespace {
        // Yeti.ANGRY_MODIFIER ("anger_follow_boost", +8 FOLLOW_RANGE). The
        // engine's ModifierId list is MC's; this id sits well clear of it.
        constexpr uint32_t kYetiAngerFollowBoost = 0x54460001u;   // "TF" 1

        // YetiThrowAttachment.THROW_COOLDOWN, kept per player (the mod keeps
        // it as a data attachment on the player, shared by every yeti).
        constexpr int kThrowCooldownTicks = 200;
        std::unordered_map<int32_t, int64_t>& ThrowCooldowns() {
            static std::unordered_map<int32_t, int64_t> map;
            return map;
        }

        bool IsBoss(const Entity& e) {
            // Tags.EntityTypes.BOSSES (c:bosses): the dragon and the wither.
            return e.GetType() == EntityTypeId::EnderDragon || e.GetType() == EntityTypeId::Wither;
        }

        bool IsPlayer(const LivingEntity& e, EntityLevel& level) {
            std::vector<LivingEntity*> players;
            level.GetPlayers(players);
            return std::find(players.begin(), players.end(), &e) != players.end();
        }
    } // namespace

    void Yeti::CreateAttributes(AttributeMap& out) {
        // TF Yeti.registerAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     20.0);
        out.Register(Attribute::MovementSpeed, 0.38);
        out.Register(Attribute::AttackDamage,   0.0);
        out.Register(Attribute::FollowRange,    4.0);
    }

    Yeti::Yeti(EntityLevel* level) : Monster(EntityTypeId::Yeti, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Yeti::RegisterGoals() {
        // TF Yeti.registerGoals.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<YetiThrowRiderGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(2, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(3, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    void Yeti::SetAngry(bool angry) {
        m_angry = angry;
        if (m_level && m_level->IsClientSide()) return;
        const auto id = static_cast<ModifierId>(kYetiAngerFollowBoost);
        if (angry) {
            if (!m_attributes.HasModifier(Attribute::FollowRange, id)) {
                m_attributes.AddModifier(Attribute::FollowRange,
                    AttributeModifier{ kYetiAngerFollowBoost, 8.0, AttributeOperation::AddValue });
            }
        } else {
            m_attributes.RemoveModifier(Attribute::FollowRange, id);
        }
    }

    bool Yeti::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // Yeti.hurtServer: any attacker but a creative player angers it.
        if (attacker && !attacker->IsCreative() && m_level && !m_level->IsClientSide()) {
            SetAngry(true);
        }
        return Monster::Hurt(source, amount, attacker);
    }

    void Yeti::AiStep() {
        Monster::AiStep();
        // "look at things in our jaws"
        if (Entity* held = GetFirstPassenger()) {
            GetLookControl().SetLookAt(held->position.x, held->GetEyeY(), held->position.z,
                                       100.0f, 100.0f);
        }
    }

    uint8_t Yeti::GetAnimStateByte() const {
        return static_cast<uint8_t>((m_angry ? 1 : 0) | (IsVehicle() ? 2 : 0));
    }

    void Yeti::SetAnimStateByte(uint8_t v) {
        m_angry = (v & 1) != 0;
        m_holdingClient = (v & 2) != 0;
    }

    glm::dvec3 Yeti::GetPassengerAttachmentPoint(const Entity& passenger) const {
        // super + (0, 0, 0.4): forward in the yeti's frame, rotated by its
        // yaw like the attachment itself.
        const glm::dvec3 base = Monster::GetPassengerAttachmentPoint(passenger);
        const double yawRad = static_cast<double>(yBodyRot) * Mth::kDegToRad;
        return base + glm::dvec3(-std::sin(yawRad) * 0.4, 0.0, std::cos(yawRad) * 0.4);
    }

    void Yeti::SaveModNbt(ModNbtOut& out) const {
        out.Bool("Angry", m_angry);
    }

    void Yeti::LoadModNbt(const ModNbtIn& in) {
        SetAngry(in.Bool("Angry", false));
    }

    bool Yeti::CheckYetiSpawnRules(const SpawnRuleContext& ctx, const glm::ivec3& pos) {
        // TF Yeti.normalYetiSpawnHandler = isValidLightLevel && checkMobSpawnRules.
        const bool snowy = BiomeAt(ctx, pos) == kSnowyForest;
        bool lightOk;
        if (ctx.level.GetSkyBrightness(pos.x, pos.y, pos.z) > ctx.rng.NextInt(32)) {
            lightOk = snowy;
        } else {
            const int i = ctx.level.IsThundering()
                ? ctx.level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z, 10)
                : ctx.level.GetMaxLocalRawBrightness(pos.x, pos.y, pos.z);
            lightOk = i <= ctx.rng.NextInt(8) || snowy;
        }
        // checkMobSpawnRules: the engine's any-light monster rule (its extra
        // peaceful test never fires — the yeti is notInPeaceful).
        return lightOk && Monster::CheckAnyLightMonsterSpawnRules(ctx.level, ctx.reason, pos, ctx.rng);
    }

    // ── YetiThrowRiderGoal ─────────────────────────────────────────────────

    YetiThrowRiderGoal::YetiThrowRiderGoal(Yeti* yeti, double speedModifier, bool useLongMemory)
        : MeleeAttackGoal(yeti, speedModifier, useLongMemory), m_yeti(yeti) {}

    bool YetiThrowRiderGoal::CanUse() {
        LivingEntity* target = m_yeti->GetTarget();
        if (!m_yeti->GetPassengers().empty() || !target || IsBoss(*target)) return false;
        EntityLevel* level = m_yeti->Level();
        if (level) {
            auto& cooldowns = ThrowCooldowns();
            const auto it = cooldowns.find(target->GetId());
            if (it != cooldowns.end()) {
                if (level->GetGameTime() < it->second) return false;
                cooldowns.erase(it);
            }
        }
        return MeleeAttackGoal::CanUse();
    }

    void YetiThrowRiderGoal::Start() {
        JavaRandom& rng = m_yeti->Level()->Random();
        m_throwTimer = 10 + rng.NextInt(30);   // 0.5 to 2 seconds before the throw
        m_timeout = 80 + rng.NextInt(40);      // chase for around 4-6 seconds
        MeleeAttackGoal::Start();
    }

    void YetiThrowRiderGoal::Tick() {
        --m_timeout;
        if (!m_yeti->GetPassengers().empty()) --m_throwTimer;
        else MeleeAttackGoal::Tick();
    }

    void YetiThrowRiderGoal::CheckAndPerformAttack(LivingEntity& victim) {
        // ThrowRiderGoal.checkAndPerformAttack, the cooldown's post-decrement
        // kept (it counts down only on otherwise-eligible checks).
        if (!(CanPerformAttack(victim) && IsTimeToAttack() &&
              m_yeti->GetPassengers().empty() && m_cooldown-- == 0)) {
            return;
        }
        m_cooldown = 3;
        ResetAttackCooldown();
        m_yeti->Swing();
        EntityLevel* level = m_yeti->Level();
        if (!level) return;
        m_yeti->PlaySound("twilightforest:entity.twilightforest.yeti.grab", 1.0f,
                          1.25f + level->Random().NextFloat() * 0.5f);
        if (IsPlayer(victim, *level)) {
            // Players cannot ride here: the catch is the throw.
            Throw(victim);
            m_timeout = 0;
            return;
        }
        // "Pluck them from the boat, minecart, donkey, or whatever"
        // (#rides_obstruct_snatching is empty of anything this engine has).
        victim.StopRiding();
        victim.StartRiding(*m_yeti, /*force=*/true);
    }

    void YetiThrowRiderGoal::Throw(Entity& rider) {
        const glm::vec3 look = Mth::ViewVector(m_yeti->xRot, m_yeti->yRot);
        const glm::dvec3 throwVec(look.x * 2.0, 0.9, look.z * 2.0);
        if (rider.GetVehicle() == m_yeti) rider.StopRiding();
        // Entity.push — AddDeltaMovement is the impulse that reaches a
        // player's client (TF's MovePlayerPacket).
        rider.AddDeltaMovement(throwVec);
        EntityLevel* level = m_yeti->Level();
        if (auto* living = dynamic_cast<LivingEntity*>(&rider); living && level &&
                                                               IsPlayer(*living, *level)) {
            ThrowCooldowns()[rider.GetId()] = level->GetGameTime() + kThrowCooldownTicks;
        }
    }

    void YetiThrowRiderGoal::Stop() {
        if (EntityLevel* level = m_yeti->Level()) {
            m_yeti->PlaySound("twilightforest:entity.twilightforest.yeti.throw", 1.0f,
                              1.25f + level->Random().NextFloat() * 0.5f);
        }
        if (Entity* rider = m_yeti->GetFirstPassenger()) Throw(*rider);
        MeleeAttackGoal::Stop();
    }

    bool YetiThrowRiderGoal::CanContinueToUse() {
        const bool holding = !m_yeti->GetPassengers().empty();
        return (m_throwTimer > 0 && holding) ||
               (m_timeout > 0 && MeleeAttackGoal::CanContinueToUse() && !holding);
    }

} // namespace Game
