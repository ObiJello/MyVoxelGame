// File: src/common/entity/mobs/AetherCreatures.cpp
#include "common/entity/mobs/AetherCreatures.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ModMobNbt.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/RangedGoals.hpp"
#include "common/entity/ai/goals/RestrictionGoals.hpp"
#include "common/entity/ai/goals/SlimeGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/world/pathfinder/PathType.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/Explosion.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace Game {

    namespace {

        // The mods' (random.nextFloat() - random.nextFloat()) idiom.
        float Spread(JavaRandom& rng) { return rng.NextFloat() - rng.NextFloat(); }

        // An item by slug with a vanilla fallback — for drops of Aether items
        // and blocks this port may not have yet.
        ItemID SlugOr(const char* slug, ItemID fallback) {
            const ItemID id = AetherIds::ItemBySlug(slug);
            return id != Items::Air ? id : fallback;
        }

        // MC `level.getHeight(WORLD_SURFACE, x, z) < y - maxFall` (the
        // whirlwind's and swet's "would this step go over a drop"), read as
        // "nothing solid from y down to y - maxFall - 1" — the engine's
        // entity level has no heightmap query.
        bool IsOverDrop(const EntityLevel& level, const glm::ivec3& p, int maxFall) {
            const IBlockAccess* blocks = level.Blocks();
            if (!blocks) return false;
            for (int y = p.y; y >= p.y - maxFall - 1; --y) {
                if (blocks->GetBlock(p.x, y, p.z) != BlockID::Air) return false;
            }
            return true;
        }

        // A velocity write that reaches a player: players move client-side,
        // so the change is pushed as a delta against the movement the client
        // last reported (MC writes the player's deltaMovement directly).
        void SetEntityMotion(Entity& e, const glm::dvec3& v) {
            if (e.IsPlayer()) {
                e.AddDeltaMovement(v - e.GetKnownMovement());
            } else {
                e.velocity = v;
                e.needsSync = true;
                e.physicsParked = false;
            }
        }

        // EntityUtil.spawnMovementExplosionParticles — POOF around the mob.
        void MovementExplosionParticles(Mob& mob) {
            EntityLevel* level = mob.Level();
            if (!level || !level->IsClientSide()) return;
            JavaRandom& rng = level->Random();
            const double d0 = rng.NextGaussian() * 0.02;
            const double d1 = rng.NextGaussian() * 0.02;
            const double d2 = rng.NextGaussian() * 0.02;
            const double d3 = 10.0;
            level->AddParticle(ParticleKind::Poof,
                mob.position.x + (rng.NextDouble() * 2.0 - 1.0) * mob.GetBbWidth() - d0 * d3,
                mob.position.y + rng.NextDouble() * mob.GetBbHeight() - d1 * d3,
                mob.position.z + (rng.NextDouble() * 2.0 - 1.0) * mob.GetBbWidth() - d2 * d3,
                d0, d1, d2);
        }

        // AetherAnimal.getWalkTargetValue.
        float AetherWalkTarget(const EntityLevel* level, const glm::ivec3& pos) {
            if (!level) return 0.0f;
            const IBlockAccess* blocks = level->Blocks();
            if (!blocks) return 0.0f;
            const BlockID grass = AetherIds::AetherGrassBlock();
            if (grass != BlockID::Air && blocks->GetBlock(pos.x, pos.y - 1, pos.z) == grass) {
                return 10.0f;
            }
            return static_cast<float>(level->GetMaxLocalRawBrightness(pos.x, pos.y, pos.z)) - 0.5f;
        }

        constexpr EntityTypeId kMoaSwetTargets[] = { EntityTypeId::BlueSwet, EntityTypeId::GoldenSwet };
        constexpr EntityTypeId kMoaPlantTargets[] = { EntityTypeId::AechorPlant };

        // MoaType ids / eggs, in the variant-byte order.
        constexpr const char* kMoaTypeIds[Moa::kMoaTypeCount] = {
            "aether:blue", "aether:white", "aether:black" };
        constexpr const char* kMoaEggs[Moa::kMoaTypeCount] = {
            "blue_moa_egg", "white_moa_egg", "black_moa_egg" };
    } // namespace

    // ══ Moa ════════════════════════════════════════════════════════════════

    void Moa::CreateAttributes(AttributeMap& out) {
        // Aether Moa.createMobAttributes; MOVEMENT_SPEED folds in
        // MoaType.speed (see the header).
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     35.0);
        out.Register(Attribute::MovementSpeed, 1.0 * 0.155);
        out.Register(Attribute::FollowRange,   16.0);
        out.Register(Attribute::AttackDamage,   5.0);
    }

    Moa::Moa(EntityLevel* level) : MountableAetherAnimal(EntityTypeId::Moa, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // Moa(): no pathing through fire, powder snow, "other" danger or lava.
        SetPathfindingMalus(PathType::DangerFire, -1.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);
        SetPathfindingMalus(PathType::DangerPowderSnow, -1.0f);
        SetPathfindingMalus(PathType::PowderSnow, -1.0f);
        SetPathfindingMalus(PathType::DangerOther, -1.0f);
        SetPathfindingMalus(PathType::DamageOther, -1.0f);
        SetPathfindingMalus(PathType::Lava, -1.0f);
        if (level) m_eggTime = level->Random().NextInt(6000) + 6000;   // getEggTime
        RegisterGoals();
    }

    uint8_t Moa::RandomMoaType(JavaRandom& rng) {
        // SimpleWeightedRandomList over spawn_chance: blue 100, white 50,
        // black 25 (registry order).
        const int roll = rng.NextInt(175);
        if (roll < 100) return kBlue;
        if (roll < 150) return kWhite;
        return kBlack;
    }

    int Moa::GetMaxJumps() const {
        static constexpr int kMaxJumps[kMoaTypeCount] = { 3, 4, 8 };
        return kMaxJumps[m_moaType];
    }

    void Moa::RegisterGoals() {
        // Aether Moa.registerGoals.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 0.65));
        // 2 MoaFollowGoal(1.0) — follows the Nature Staff holder set by
        // shift-clicking; there is no Nature Staff, so it never runs.
        // ContinuousMeleeAttackGoal(1.0, true) -> MeleeAttackGoal.
        m_goalSelector.AddGoal(3, std::make_unique<MeleeAttackGoal>(this, 1.0, true));
        // FallingRandomStrollGoal(this, 0.35) -> WaterAvoidingRandomStrollGoal.
        m_goalSelector.AddGoal(4, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.35));
        m_goalSelector.AddGoal(5, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<MoaHuntTargetGoal>(this, kMoaSwetTargets, 2));
        m_targetSelector.AddGoal(2, std::make_unique<MoaHuntTargetGoal>(this, kMoaPlantTargets, 1));
    }

    std::shared_ptr<SpawnGroupData>
    Moa::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // Moa.finalizeSpawn: never a baby in a spawn group, and a weighted
        // MoaType.
        if (m_level) SetMoaType(RandomMoaType(m_level->Random()));
        auto data = MountableAetherAnimal::FinalizeSpawn(reason, std::move(groupData));
        SetBaby(false);
        return data;
    }

    void Moa::Tick() {
        MountableAetherAnimal::Tick();
        // Moa.tick: the glide (-0.1 cap; -0.5 when ridden), clearing the
        // on-ground flag.
        if (ClampWingedFall(*this, IsVehicle() ? -0.5 : -0.1)) SetEntityOnGround(false);
        if (onGround) m_remainingJumps = GetMaxJumps();
        if (!m_level) return;

        if (!m_level->IsClientSide() && IsAlive()) {
            JavaRandom& rng = m_level->Random();
            if (rng.NextInt(900) == 0 && deathTime == 0) Heal(1.0f);
            // Egg laying: an adult with no rider, every 6000-11999 ticks.
            if (!IsBaby() && GetPassengers().empty() && --m_eggTime <= 0) {
                const ItemID egg = AetherIds::ItemBySlug(kMoaEggs[m_moaType]);
                if (egg != Items::Air) m_level->SpawnItemDrop(position, egg, 1);
                m_eggTime = rng.NextInt(6000) + 6000;
            }
        }

        // Baby hunger: a 1-in-2000 roll server-side; a hungry baby puffs
        // angry-villager clouds client-side.
        if (IsBaby()) {
            if (!m_hungry) {
                if (!m_level->IsClientSide() && m_level->Random().NextInt(2000) == 0) m_hungry = true;
            } else if (m_level->IsClientSide()) {
                JavaRandom& rng = m_level->Random();
                if (rng.NextInt(10) == 0) {
                    m_level->AddParticle(ParticleKind::AngryVillager,
                        position.x + (rng.NextDouble() - 0.5) * GetBbWidth(), position.y + 1.0,
                        position.z + (rng.NextDouble() - 0.5) * GetBbWidth(), 0.0, 0.0, 0.0);
                }
            }
        } else {
            m_hungry = false;
            m_amountFed = 0;
        }
    }

    void Moa::AiStep() {
        MountableAetherAnimal::AiStep();
        m_wings.Step(IsEntityOnGround());
    }

    int Moa::GetMaxFallDistance() const {
        return onGround ? MountableAetherAnimal::GetMaxFallDistance() : 14;
    }

    float Moa::DimensionFactor() const {
        return (m_sitting ? 0.5f : 1.0f) * (IsBaby() ? 0.5f : 1.0f);
    }

    float Moa::BaseBbWidth() const { return TypeInfo().width; }
    float Moa::BaseBbHeight() const { return TypeInfo().height * DimensionFactor(); }
    float Moa::BaseEyeHeight() const { return TypeInfo().eyeHeight * DimensionFactor(); }

    uint8_t Moa::GetAnimStateByte() const {
        return static_cast<uint8_t>(MountableAetherAnimal::GetAnimStateByte() |
                                    (m_sitting ? 4 : 0) | (m_hungry ? 8 : 0) |
                                    (m_playerGrown ? 16 : 0));
    }

    void Moa::SetAnimStateByte(uint8_t v) {
        MountableAetherAnimal::SetAnimStateByte(v);
        m_sitting = (v & 4) != 0;
        m_hungry = (v & 8) != 0;
        m_playerGrown = (v & 16) != 0;
    }

    void Moa::SaveModNbt(ModNbtOut& out) const {
        MountableAetherAnimal::SaveModNbt(out);
        out.Bool("IsBaby", IsBaby());
        out.String("MoaType", kMoaTypeIds[m_moaType]);
        out.Int("RemainingJumps", m_remainingJumps);
        out.Bool("Hungry", m_hungry);
        out.Int("AmountFed", m_amountFed);
        out.Bool("PlayerGrown", m_playerGrown);
        out.Bool("Sitting", m_sitting);
    }

    void Moa::LoadModNbt(const ModNbtIn& in) {
        MountableAetherAnimal::LoadModNbt(in);
        if (in.Has("IsBaby")) SetBaby(in.Bool("IsBaby", false));
        bool found = false;
        if (in.Has("MoaType")) {
            const std::string id = in.String("MoaType", "");
            for (uint8_t i = 0; i < kMoaTypeCount; ++i) {
                if (id == kMoaTypeIds[i]) { m_moaType = i; found = true; }
            }
        }
        // Moa.readAdditionalSaveData: an unknown type rolls a new one.
        if (!found && m_level) m_moaType = RandomMoaType(m_level->Random());
        if (in.Has("RemainingJumps")) m_remainingJumps = in.Int("RemainingJumps", 0);
        if (in.Has("Hungry")) m_hungry = in.Bool("Hungry", false);
        if (in.Has("AmountFed")) m_amountFed = in.Int("AmountFed", 0);
        if (in.Has("PlayerGrown")) m_playerGrown = in.Bool("PlayerGrown", false);
        if (in.Has("Sitting")) m_sitting = in.Bool("Sitting", false);
    }

    // ══ Aerbunny ═══════════════════════════════════════════════════════════

    void Aerbunny::CreateAttributes(AttributeMap& out) {
        // Aether Aerbunny.createMobAttributes.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MaxHealth,     6.0);
        out.Register(Attribute::MovementSpeed, 0.28);
    }

    Aerbunny::Aerbunny(EntityLevel* level) : Animal(EntityTypeId::Aerbunny, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetMoveControl(std::make_unique<AerbunnyMoveControl>(this));
        RegisterGoals();
    }

    bool Aerbunny::IsFood(uint32_t itemId) const {
        const ItemID berry = AetherIds::BlueBerry();
        return berry != Items::Air && itemId == berry;
    }

    std::unique_ptr<Animal> Aerbunny::CreateBaby() {
        return std::make_unique<Aerbunny>(m_level);
    }

    float Aerbunny::GetWalkTargetValue(const glm::ivec3& pos) const {
        return AetherWalkTarget(m_level, pos);
    }

    void Aerbunny::RegisterGoals() {
        // Aether Aerbunny.registerGoals.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<AerbunnyRunWhenAfraidGoal>(this, 1.3));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.2, false));
        m_goalSelector.AddGoal(4, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        // FallingRandomStrollGoal(this, 1.0, 80) -> RandomStrollGoal(80).
        m_goalSelector.AddGoal(5, std::make_unique<RandomStrollGoal>(this, 1.0, 80));
    }

    void Aerbunny::Tick() {
        Animal::Tick();
        // Aerbunny.tick: the slow fall unless thrown; a thrown one falls
        // freely until it lands.
        if (!m_fastFalling) {
            ClampWingedFall(*this);
        } else if (onGround) {
            m_fastFalling = false;
        }
        m_puffiness -= m_puffSubtract;
        if (m_puffiness > 0) {
            m_puffSubtract = 1;
        } else {
            m_puffSubtract = 0;
            m_puffiness = 0;
        }
    }

    void Aerbunny::AiStep() {
        Animal::AiStep();
        if (m_afraidTime > 0) --m_afraidTime;
    }

    bool Aerbunny::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool hurt = Animal::Hurt(source, amount, attacker);
        // Aerbunny.hurt: a player hit frightens it for 100-149 ticks.
        if (hurt && attacker && attacker->IsPlayer() && m_level) {
            m_afraidTime = 100 + m_level->Random().NextInt(50);
        }
        return hurt;
    }

    void Aerbunny::MidairJump() {
        // Aerbunny.midairJump: a puff on the way down, then 0.25 up.
        if (velocity.y < 0.0) {
            if (m_level && !m_level->IsClientSide()) {
                m_puffiness = 11;   // MAXIMUM_PUFFS
                m_level->BroadcastEntityEvent(*this, 70);
            }
        }
        velocity.y = 0.25;
        needsSync = true;
    }

    void Aerbunny::HandleEntityEvent(uint8_t id) {
        if (id == 70) {
            for (int i = 0; i < 5; ++i) MovementExplosionParticles(*this);
            return;
        }
        Animal::HandleEntityEvent(id);
    }

    void Aerbunny::SaveModNbt(ModNbtOut& out) const {
        out.Int("AfraidTime", m_afraidTime);
    }

    void Aerbunny::LoadModNbt(const ModNbtIn& in) {
        if (in.Has("AfraidTime")) m_afraidTime = in.Int("AfraidTime", 0);
    }

    void AerbunnyMoveControl::Tick() {
        MoveControl::Tick();
        // Aerbunny.AerbunnyMoveControl.tick: moving on the ground hops;
        // moving in the air over a ledge double-jumps.
        if (m_bunny->zza == 0.0f) return;
        if (m_bunny->onGround) {
            m_bunny->GetJumpControl().Jump();
            return;
        }
        const int x = static_cast<int>(std::floor(m_bunny->position.x));
        const int y = static_cast<int>(std::floor(m_bunny->position.y));
        const int z = static_cast<int>(std::floor(m_bunny->position.z));
        if (CheckForSurfaces(x, y, z) && !m_bunny->horizontalCollision) m_bunny->MidairJump();
    }

    bool AerbunnyMoveControl::CheckForSurfaces(int x, int y, int z) const {
        const EntityLevel* level = m_bunny->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return false;
        if (blocks->GetBlock(x, y - 1, z) == BlockID::Air) return false;
        return blocks->GetBlock(x, y + 2, z) == BlockID::Air &&
               blocks->GetBlock(x, y + 1, z) == BlockID::Air;
    }

    AerbunnyRunWhenAfraidGoal::AerbunnyRunWhenAfraidGoal(Aerbunny* bunny, double speed)
        : m_bunny(bunny), m_speed(speed) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool AerbunnyRunWhenAfraidGoal::CanContinueToUse() {
        EntityLevel* level = m_bunny->Level();
        return !m_bunny->GetNavigation().IsDone() && level && level->Random().NextInt(20) != 0;
    }

    void AerbunnyRunWhenAfraidGoal::Start() {
        // RunWhenAfraid.start: run 8 blocks away from the nearest player
        // (within 12), up to 0.75 rad off the straight line.
        EntityLevel* level = m_bunny->Level();
        if (!level) return;
        LivingEntity* attacker = level->GetNearestPlayer(m_bunny->position.x, m_bunny->position.y,
                                                         m_bunny->position.z, 12.0);
        if (!attacker) return;
        double angle = std::atan2(m_bunny->position.x - attacker->position.x,
                                  m_bunny->position.z - attacker->position.z);
        angle += (level->Random().NextFloat() * 2.0f - 1.0f) * 0.75;
        const double x = m_bunny->position.x + std::sin(angle) * 8.0;
        const double z = m_bunny->position.z + std::cos(angle) * 8.0;
        if (!m_bunny->GetNavigation().MoveTo(x, m_bunny->position.y, z, m_speed)) {
            m_bunny->GetLookControl().SetLookAt(attacker->position.x, attacker->GetEyeY(),
                                                attacker->position.z, 30.0f, 30.0f);
        }
        // tick(): SPLASH particles — no splash particle kind here.
    }

    // ══ Aerwhale ═══════════════════════════════════════════════════════════

    namespace {
        // Aerwhale.BlankLookControl.
        class BlankLookControl : public LookControl {
        public:
            explicit BlankLookControl(Mob* mob) : LookControl(mob) {}
            void Tick() override {}
        };
    } // namespace

    void Aerwhale::CreateAttributes(AttributeMap& out) {
        // Aether Aerwhale.createMobAttributes (FlyingMob's = Mob's).
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,   20.0);
        out.Register(Attribute::FlyingSpeed,  0.2);
        out.Register(Attribute::StepHeight,   0.4);
    }

    Aerwhale::Aerwhale(EntityLevel* level) : Mob(EntityTypeId::Aerwhale, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetLookControl(std::make_unique<BlankLookControl>(this));
        SetMoveControl(std::make_unique<AerwhaleMoveControl>(this));
        RegisterGoals();
    }

    void Aerwhale::RegisterGoals() {
        m_goalSelector.AddGoal(1, std::make_unique<AerwhaleTravelCourseGoal>(this));
    }

    void Aerwhale::Travel(const glm::dvec3& input) {
        // FlyingMob.travel (no rider here).
        const double drag = IsInWater() ? 0.8 : (IsInLava() ? 0.5 : 0.91);
        MoveRelative(0.02f, input);
        Move(velocity);
        velocity *= drag;
    }

    int Aerwhale::GetXpReward() const {
        // Aerwhale.getBaseExperienceReward: 1 + rand(3).
        return m_level ? 1 + m_level->Random().NextInt(3) : 1;
    }

    bool AerwhaleMoveControl::IsColliding(const glm::dvec3& dir) const {
        // AerwhaleMoveControl.isColliding: the box stepped 1..6 times along
        // the direction must stay clear of terrain.
        const EntityLevel* level = m_mob->Level();
        if (!level) return false;
        AABBd box = m_mob->GetAABBd();
        for (int i = 1; i < 7; ++i) {
            box.min += dir;
            box.max += dir;
            if (CollidesAt(box, level->Physics())) return true;
        }
        return false;
    }

    void AerwhaleMoveControl::Tick() {
        // Aerwhale.AerwhaleMoveControl.tick (the leash half has no leads).
        if (m_mob->IsVehicle()) return;
        double x = m_wantedX - m_mob->position.x;
        double y = m_wantedY - m_mob->position.y;
        double z = m_wantedZ - m_mob->position.z;
        const double distance = std::sqrt(x * x + z * z);
        const double len = std::sqrt(x * x + y * y + z * z);
        if (len > 1.0e-4 && IsColliding(glm::dvec3(x, y, z) / len)) m_operation = Operation::Wait;

        const float xRotTarget = static_cast<float>(std::atan2(y, distance) * Mth::kRadToDeg);
        float xRot = Mth::WrapDegrees(m_mob->xRot);
        xRot = Mth::ApproachDegrees(xRot, xRotTarget, 0.2f);
        m_mob->xRot = xRot;

        const float yRotTarget = Mth::WrapDegrees(static_cast<float>(std::atan2(z, x) * Mth::kRadToDeg));
        float yRot = Mth::WrapDegrees(m_mob->yRot + 90.0f);
        yRot = Mth::ApproachDegrees(yRot, yRotTarget, 0.5f);
        m_mob->yRot = yRot - 90.0f;
        m_mob->yBodyRot = yRot;
        m_mob->SetYHeadRot(yRot);

        const double speed = m_mob->GetAttributeValue(Attribute::FlyingSpeed);
        m_mob->velocity = glm::dvec3(speed * std::cos(yRot * Mth::kDegToRad),
                                     speed * std::sin(xRot * Mth::kDegToRad),
                                     speed * std::sin(yRot * Mth::kDegToRad));
        m_mob->needsSync = true;
    }

    AerwhaleTravelCourseGoal::AerwhaleTravelCourseGoal(Mob* mob) : m_mob(mob) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool AerwhaleTravelCourseGoal::CanUse() {
        // SetTravelCourseGoal.canUse: no course, or within a block of it.
        MoveControl& mc = m_mob->GetMoveControl();
        if (!mc.HasWanted()) return true;
        const double dx = mc.GetWantedX() - m_mob->position.x;
        const double dy = mc.GetWantedY() - m_mob->position.y;
        const double dz = mc.GetWantedZ() - m_mob->position.z;
        return dx * dx + dy * dy + dz * dz < 1.0;
    }

    void AerwhaleTravelCourseGoal::Start() {
        // SetTravelCourseGoal.start: 32-48 blocks out on each horizontal
        // axis, +-16 vertically, clamped to the build height.
        EntityLevel* level = m_mob->Level();
        if (!level) return;
        JavaRandom& rng = level->Random();
        double x = (rng.NextFloat() * 2.0f - 1.0f) * 16.0;
        double z = (rng.NextFloat() * 2.0f - 1.0f) * 16.0;
        double y = m_mob->position.y + (rng.NextFloat() * 2.0f - 1.0f) * 16.0;
        x = x >= 0.0 ? x + 32.0 : x - 32.0;
        z = z >= 0.0 ? z + 32.0 : z - 32.0;
        x += m_mob->position.x;
        z += m_mob->position.z;
        y = std::clamp(y, static_cast<double>(level->GetMinY()), static_cast<double>(level->GetMaxY() + 1));
        m_mob->GetMoveControl().SetWantedPosition(x, y, z, 1.0);
    }

    // ══ Swet ═══════════════════════════════════════════════════════════════

    void Swet::CreateAttributes(AttributeMap& out) {
        // Aether Swet.createMobAttributes.
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,           12.0);
        out.Register(Attribute::MovementSpeed,        0.4);
        out.Register(Attribute::FollowRange,         14.0);
        out.Register(Attribute::KnockbackResistance,  0.5);
    }

    Swet::Swet(EntityTypeId type, EntityLevel* level) : Mob(type, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetMoveControl(std::make_unique<SwetMoveControl>(this));
        RegisterGoals();
    }

    void Swet::RegisterGoals() {
        // Aether Swet.registerGoals.
        m_goalSelector.AddGoal(0, std::make_unique<SwetConsumeGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<SwetHuntGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<SwetRandomDirectionGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<SwetKeepOnJumpingGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    float Swet::BaseBbWidth() const { return TypeInfo().width * (1.0f - m_waterDamageScale); }
    float Swet::BaseBbHeight() const { return TypeInfo().height * (1.0f - m_waterDamageScale); }
    float Swet::BaseEyeHeight() const { return TypeInfo().eyeHeight * (1.0f - m_waterDamageScale); }

    int Swet::GetJumpDelay() {
        return m_level ? m_level->Random().NextInt(20) + 10 : 20;
    }

    void Swet::JumpFromGround() {
        // Slime.jumpFromGround: the vertical launch only.
        velocity.y = GetJumpPower();
        needsSync = true;
    }

    void Swet::Consume(LivingEntity& prey) {
        // Swet.consumePassenger (see the header on holding the prey).
        m_prey = &prey;
        yRot = prey.yRot;
    }

    void Swet::ReleasePrey() { m_prey = nullptr; }

    void Swet::ClearReferenceTo(const Entity* entity) {
        if (entity == m_prey) m_prey = nullptr;
        Mob::ClearReferenceTo(entity);
    }

    void Swet::Tick() {
        // Swet.tick: water dissolves it.
        if (m_level && !m_level->IsClientSide()) {
            if (IsInWater()) {
                m_level->BroadcastEntityEvent(*this, 70);
                if (m_waterDamageScale < 0.9f) m_waterDamageScale += 0.02f;
            }
            if (m_waterDamageScale >= 0.9f) {
                m_level->BroadcastEntityEvent(*this, 60);
                ReleasePrey();
                Remove(RemovalReason::Killed);
                return;
            }
        }
        Mob::Tick();
        if (!onGround && velocity.y < 0.05 && m_ascendTimer > 0) {
            velocity = glm::dvec3(velocity.x * 1.2, 0.07, velocity.z * 1.2);
            --m_ascendTimer;
            needsSync = true;
        }
        if (onGround) m_ascendTimer = 10;

        // (The prey-less SPLASH drip from the top has no particle kind here.)
        if (!IsNoAi()) {
            if (m_level && !m_level->IsClientSide()) m_midJump = !onGround;
            if (m_level && m_level->IsClientSide()) {
                m_swetHeightO = m_swetHeight;
                m_swetWidthO = m_swetWidth;
                m_jumpTimer = m_midJump ? m_jumpTimer + 1 : 0;
                if (m_jumpTimer > 1) {
                    m_swetHeight = 1.425f;
                    m_swetWidth = 0.875f;
                    const float scale = static_cast<float>(std::min(m_jumpTimer, 10));
                    if (m_jumpTimer > 2) {
                        m_swetHeight -= 0.04f * scale;
                        m_swetWidth += 0.04f * scale;
                    }
                    if (m_jumpTimer > 3) {
                        m_swetHeight -= 0.02f * scale;
                        m_swetWidth += 0.02f * scale;
                    }
                } else {
                    m_swetHeight = m_swetHeight < 1.0f ? m_swetHeight + 0.25f : 1.0f;
                    m_swetWidth = m_swetWidth > 1.0f ? m_swetWidth - 0.25f : 1.0f;
                }
            } else {
                m_jumpTimer = m_midJump ? m_jumpTimer + 1 : 0;
            }
            m_wasOnGround = onGround;
        }
    }

    void Swet::AiStep() {
        Mob::AiStep();
        // Swet.aiStep: no hunting while holding prey.
        if (GetTarget() && HasPrey()) SetTarget(nullptr);
        if (m_prey && (!m_prey->IsAlive() || m_prey->IsRemoved())) m_prey = nullptr;
    }

    uint8_t Swet::GetAnimStateByte() const {
        const int steps = std::clamp(static_cast<int>(std::lround(m_waterDamageScale / 0.02f)), 0, 63);
        return static_cast<uint8_t>((m_midJump ? 1 : 0) | (steps << 1));
    }

    void Swet::SetAnimStateByte(uint8_t v) {
        m_midJump = (v & 1) != 0;
        m_waterDamageScale = static_cast<float>(v >> 1) * 0.02f;
    }

    void Swet::DropCustomDeathLoot(EntityLevel& level) {
        if (GetType() == EntityTypeId::BlueSwet) {
            // entities/blue_swet, pool 2: one aether:blue_aercloud.
            const ItemID cloud = AetherIds::ItemBySlug("blue_aercloud");
            if (cloud != Items::Air) level.SpawnItemDrop(position, cloud, 1);
        } else {
            // entities/golden_swet: one glowstone (a block item).
            const ItemID glowstone = AetherIds::ItemBySlug("glowstone");
            if (glowstone != Items::Air) level.SpawnItemDrop(position, glowstone, 1);
        }
    }

    void Swet::SaveModNbt(ModNbtOut& out) const {
        out.Float("WaterDamageScale", m_waterDamageScale);
    }

    void Swet::LoadModNbt(const ModNbtIn& in) {
        if (in.Has("WaterDamageScale")) m_waterDamageScale = in.Float("WaterDamageScale", 0.0f);
    }

    SwetMoveControl::SwetMoveControl(Swet* swet) : MoveControl(swet), m_swet(swet) {
        m_yRot = 180.0f * swet->yRot / Mth::kPi;   // the mod's constructor, verbatim
    }

    void SwetMoveControl::Tick() {
        // Swet.SwetMoveControl.tick.
        m_mob->yRot = RotLerp(m_mob->yRot, m_yRot, 90.0f);
        m_mob->SetYHeadRot(m_mob->yRot);
        m_mob->yBodyRot = m_mob->yRot;
        if (m_operation != Operation::MoveTo) {
            m_mob->SetZza(0.0f);
            return;
        }
        m_operation = Operation::Wait;
        const float speed = static_cast<float>(
            m_speedModifier * m_mob->GetAttributeValue(Attribute::MovementSpeed));
        if (m_mob->onGround) {
            m_mob->SetSpeed(speed);
            if (m_jumpDelay-- <= 0) {
                m_jumpDelay = m_swet->GetJumpDelay();
                if (m_aggressive) m_jumpDelay /= 6;
                m_swet->GetJumpControl().Jump();
            } else {
                m_mob->SetXxa(0.0f);
                m_mob->SetZza(0.0f);
                m_mob->SetSpeed(0.0f);
            }
        } else {
            m_mob->SetSpeed(speed);
        }
    }

    SwetConsumeGoal::SwetConsumeGoal(Swet* swet) : m_swet(swet) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    void SwetConsumeGoal::MoveHorizontal(float forward, float rotation) {
        // ConsumeGoal.moveHorizontal with strafe 0.
        float f = std::sqrt(forward * forward);
        if (f < 1.0f) f = 1.0f;
        forward *= f;
        const float f1 = std::sin(rotation * 0.017453292f);
        const float f2 = std::cos(rotation * 0.017453292f);
        m_swet->velocity.x = -forward * f1;
        m_swet->velocity.z = forward * f2;
        m_swet->needsSync = true;
        static_cast<SwetMoveControl&>(m_swet->GetMoveControl())
            .SetDirection(std::fmod(rotation, 360.0f), false);
    }

    void SwetConsumeGoal::Tick() {
        // Swet.ConsumeGoal.tick.
        EntityLevel* level = m_swet->Level();
        LivingEntity* prey = m_swet->GetPrey();
        if (!level || !prey || m_jumps > 3) return;
        if (m_swet->onGround) {
            m_chosenDegrees = static_cast<float>(level->Random().NextInt(360));
            static constexpr double kLift[3] = { 0.65, 0.75, 1.55 };
            if (m_jumps <= 2) {
                const double lift = kLift[m_jumps];
                m_swet->velocity.y += lift;
                m_swet->needsSync = true;
                // The prey rides along: the same lift, pulled onto the swet.
                const glm::dvec3 pull = (m_swet->position - prey->position) * 0.5;
                prey->AddDeltaMovement(glm::dvec3(pull.x, lift, pull.z));
            } else {
                // The third landing: drop the prey, dissolve.
                m_swet->ReleasePrey();
                level->BroadcastEntityEvent(*m_swet, 70);
                m_swet->Discard();
                return;
            }
            if (!m_swet->GetMidJump()) ++m_jumps;
        }
        if (!m_swet->WasOnGround() && m_swet->GetJumpTimer() < 6) {
            if (m_jumps == 1)      MoveHorizontal(0.1f, m_chosenDegrees);
            else if (m_jumps == 2) MoveHorizontal(0.15f, m_chosenDegrees);
            else if (m_jumps == 3) MoveHorizontal(0.3f, m_chosenDegrees);
        }
    }

    SwetHuntGoal::SwetHuntGoal(Swet* swet) : m_swet(swet) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
    }

    bool SwetHuntGoal::CanUse() {
        LivingEntity* target = m_swet->GetTarget();
        if (m_swet->HasPrey() || !target || !target->IsAlive()) return false;
        return !target->IsCreative() && !target->IsSpectator();
    }

    bool SwetHuntGoal::CanContinueToUse() { return CanUse(); }

    void SwetHuntGoal::Tick() {
        LivingEntity* target = m_swet->GetTarget();
        if (!target) return;
        // lookAt(target, 10, 10): the yaw toward the target.
        const double dx = target->position.x - m_swet->position.x;
        const double dz = target->position.z - m_swet->position.z;
        const float wanted = static_cast<float>(std::atan2(dz, dx) * Mth::kRadToDeg) - 90.0f;
        m_swet->yRot += std::clamp(Mth::WrapDegrees(wanted - m_swet->yRot), -10.0f, 10.0f);
        auto& mc = static_cast<SwetMoveControl&>(m_swet->GetMoveControl());
        mc.SetDirection(m_swet->yRot, true);
        mc.SetWantedMovement(1.0);
        if (m_swet->GetAABB().Intersects(target->GetAABB())) m_swet->Consume(*target);
    }

    SwetRandomDirectionGoal::SwetRandomDirectionGoal(Swet* swet) : m_swet(swet) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
    }

    bool SwetRandomDirectionGoal::CanUse() {
        return !m_swet->GetTarget() &&
               (m_swet->onGround || m_swet->IsInWater() || m_swet->IsInLava() ||
                m_swet->HasEffect(MobEffectId::Levitation));
    }

    void SwetRandomDirectionGoal::Tick() {
        // SwetRandomDirectionGoal.tick: turn back from a drop, else a new
        // heading every 40-99 ticks.
        EntityLevel* level = m_swet->Level();
        if (!level) return;
        auto& mc = static_cast<SwetMoveControl&>(m_swet->GetMoveControl());
        const float rot = mc.GetYRot();
        const glm::dvec3 ahead = m_swet->position +
            glm::dvec3(-std::sin(rot * Mth::kDegToRad) * 2.0, 0.0, std::cos(rot * Mth::kDegToRad) * 2.0);
        const glm::ivec3 aheadPos(static_cast<int>(std::floor(ahead.x)),
                                  static_cast<int>(std::floor(ahead.y)),
                                  static_cast<int>(std::floor(ahead.z)));
        if (IsOverDrop(*level, aheadPos, m_swet->GetMaxFallDistance())) {
            m_nextRandomizeTime = AdjustedTickDelay(40 + level->Random().NextInt(60));
            m_chosenDegrees += 180.0f;
            mc.SetCanJump(false);
        } else {
            if (--m_nextRandomizeTime <= 0) {
                m_nextRandomizeTime = AdjustedTickDelay(40 + level->Random().NextInt(60));
                m_chosenDegrees = static_cast<float>(level->Random().NextInt(360));
            }
            mc.SetCanJump(true);
        }
        mc.SetDirection(m_chosenDegrees, false);
    }

    SwetKeepOnJumpingGoal::SwetKeepOnJumpingGoal(Swet* swet) : m_swet(swet) {
        SetFlags(GoalFlag::Jump | GoalFlag::Move);
    }

    bool SwetKeepOnJumpingGoal::CanUse() {
        return !m_swet->IsPassenger() &&
               static_cast<SwetMoveControl&>(m_swet->GetMoveControl()).CanJump();
    }

    void SwetKeepOnJumpingGoal::Tick() {
        static_cast<SwetMoveControl&>(m_swet->GetMoveControl()).SetWantedMovement(1.0);
    }

    // ══ Whirlwinds ═════════════════════════════════════════════════════════

    void Whirlwind::CreateAttributes(AttributeMap& out) {
        // Aether AbstractWhirlwind.createMobAttributes.
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,     10.0);
        out.Register(Attribute::MovementSpeed, 0.025);
        out.Register(Attribute::FollowRange,   16.0);
    }

    Whirlwind::Whirlwind(EntityTypeId type, EntityLevel* level) : Mob(type, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // AbstractWhirlwind(): the heading is rolled on the CLIENT only.
        if (level && level->IsClientSide()) {
            JavaRandom& rng = level->Random();
            m_movementAngle = rng.NextFloat() * 360.0f;
            m_movementCurve = Spread(rng) * 0.1f;
        }
        // getDefaultColor: white for the passive one, black for the evil.
        m_color = type == EntityTypeId::EvilWhirlwind ? 0 : 16777215;
        RegisterGoals();
    }

    void Whirlwind::RegisterGoals() {
        m_goalSelector.AddGoal(2, std::make_unique<WhirlwindMoveGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, false));
    }

    std::shared_ptr<SpawnGroupData>
    Whirlwind::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // Passive: 512-1023 ticks of life; evil: (512-1023) / 2.
        if (m_level) {
            const int roll = m_level->Random().NextInt(512) + 512;
            m_lifeLeft = IsEvil() ? roll / 2 : roll;
        }
        return Mob::FinalizeSpawn(reason, std::move(groupData));
    }

    void Whirlwind::Tick() {
        Mob::Tick();
        --m_lifeLeft;
        if (m_level && !m_level->IsClientSide() && (m_lifeLeft <= 0 || IsInWater() || IsInLava())) {
            Discard();
        }
    }

    void Whirlwind::AiStep() {
        if (!m_level) return;
        if (!m_level->IsClientSide()) {
            // Stuck against a ceiling counts up four at a time.
            if (verticalCollision && !onGround) {
                m_stuckTick += 4;
            } else if (m_stuckTick > 0) {
                --m_stuckTick;
            }
            if (GetTarget()) ++m_dropsTimer;
            if (m_dropsTimer >= 128) {
                SpawnDrops();
                m_dropsTimer = 0;
            }
        } else {
            SpawnParticles();
        }
        Mob::AiStep();

        // Lift and spin everything in the box stretched +2.5 on each axis.
        if (!m_level->IsClientSide()) {
            AABB box = GetAABB();
            box.max += glm::vec3(2.5f);
            std::vector<Entity*> nearby;
            m_level->GetEntitiesInBox(box, this, nearby);
            for (Entity* entity : nearby) {
                if (!entity || entity->IsRemoved()) continue;
                // WHIRLWIND_UNAFFECTED: bosses, the aechor plant, whirlwinds.
                const EntityTypeId t = entity->GetType();
                if (!entity->IsPlayer() &&
                    (t == EntityTypeId::AechorPlant || t == EntityTypeId::Whirlwind ||
                     t == EntityTypeId::EvilWhirlwind || t == EntityTypeId::EnderDragon ||
                     t == EntityTypeId::Wither)) {
                    continue;
                }
                const double ex = entity->position.x;
                // getPassengerRidingPosition(this).y * 0.6 — the whirlwind's
                // seat is at its height.
                const double ey = entity->position.y - (position.y + GetBbHeight()) * 0.6;
                const double ez = entity->position.z;
                double distance = DistanceTo(*entity);
                const double d1 = ey - position.y;
                glm::dvec3 motion = entity->IsPlayer() ? entity->GetKnownMovement() : entity->velocity;
                if (distance <= 1.5 + d1) {
                    motion.y = 0.15;
                    entity->ResetFallDistance();
                    if (d1 > 1.5) {
                        motion.y = -0.45 + d1 * 0.35;
                        distance += d1 * 1.5;
                    } else {
                        motion.y = 0.125;
                    }
                    double d2 = std::atan2(position.x - ex, position.z - ez) / 0.0175;
                    d2 += 160.0;
                    motion.x = -std::cos(0.0175 * d2) * (distance + 0.25) * 0.1;
                    motion.z = std::sin(0.0175 * d2) * (distance + 0.25) * 0.1;
                } else {
                    const double d3 = std::atan2(position.x - ex, position.z - ez) / 0.0175;
                    // MC's quirk, kept: y is ADDED to itself.
                    motion += glm::dvec3(std::sin(0.0175 * d3) * 0.01, motion.y,
                                         std::cos(0.0175 * d3) * 0.01);
                }
                SetEntityMotion(*entity, motion);
                const glm::ivec3 bp = BlockPosition();
                if (const IBlockAccess* blocks = m_level->Blocks()) {
                    if (blocks->GetBlock(bp.x, bp.y, bp.z) != BlockID::Air) m_lifeLeft -= 50;
                }
            }
        }
        if (m_stuckTick > 40) m_lifeLeft = 0;
    }

    void Whirlwind::SpawnDrops() {
        // AbstractWhirlwind.spawnDrops: 1 in 4, one roll of
        // selectors/(evil_)whirlwind_junk.
        JavaRandom& rng = m_level->Random();
        if (rng.NextInt(4) != 0) return;
        struct Junk { const char* slug; int weight; };
        static constexpr Junk kJunk[] = {
            {"diamond", 1}, {"iron_ingot", 4}, {"gold_ingot", 5}, {"coal", 9},
            {"pumpkin", 2}, {"gravel", 5}, {"clay", 11}, {"stick", 12},
            {"flint", 14}, {"oak_log", 17}, {"sand", 20},
        };
        int total = 0;
        for (const Junk& j : kJunk) total += j.weight;
        // The evil table's extra 60-weight entry: an empty stack whose
        // whirlwind_spawn_entity function spawns one creeper.
        const int creeperWeight = IsEvil() ? 60 : 0;
        int roll = rng.NextInt(total + creeperWeight);
        const glm::dvec3 origin = position;
        for (const Junk& j : kJunk) {
            roll -= j.weight;
            if (roll < 0) {
                const ItemID item = AetherIds::ItemBySlug(j.slug);
                // spawnAtLocation(stack, 1): one block up.
                if (item != Items::Air) {
                    m_level->SpawnItemDrop(origin + glm::dvec3(0.0, 1.0, 0.0), item, 1);
                }
                return;
            }
        }
        std::unique_ptr<Mob> creeper = MakeGenericMob(EntityTypeId::Creeper, m_level);
        if (!creeper) return;
        creeper->position = origin + glm::dvec3(0.0, 0.5, 0.0);
        creeper->yRot = static_cast<float>(rng.NextDouble()) * 360.0f;
        creeper->velocity = glm::dvec3((rng.NextDouble() - rng.NextDouble()) * 0.125, 0.0,
                                       (rng.NextDouble() - rng.NextDouble()) * 0.125);
        m_level->AddFreshEntity(std::move(creeper));
    }

    void Whirlwind::SpawnParticles() {
        // Passive/EvilWhirlwind.spawnParticles: 2 (3) motes rising from the
        // top and flung outward.
        JavaRandom& rng = m_level->Random();
        const int count = IsEvil() ? 3 : 2;
        const ParticleKind kind = IsEvil() ? ParticleKind::LargeSmoke : ParticleKind::Poof;
        for (int i = 0; i < count; ++i) {
            const double x = position.x + rng.NextDouble() * 0.25;
            const double y = position.y + GetBbHeight() + 0.125;
            const double z = position.z + rng.NextDouble() * 0.25;
            const float f = rng.NextFloat() * 360.0f;
            m_level->AddParticle(kind, x, y - 0.25, z,
                                 -std::sin(0.0175f * f) * 0.75, 0.125, std::cos(0.0175f * f) * 0.75);
        }
    }

    void Whirlwind::SaveModNbt(ModNbtOut& out) const {
        out.Float("Movement Angle", m_movementAngle);
        out.Float("Movement Curve", m_movementCurve);
        out.Int("Life Left", m_lifeLeft);
        out.Int("Color", m_color);
    }

    void Whirlwind::LoadModNbt(const ModNbtIn& in) {
        if (in.Has("Movement Angle")) m_movementAngle = in.Float("Movement Angle", 0.0f);
        if (in.Has("Movement Curve")) m_movementCurve = in.Float("Movement Curve", 0.0f);
        if (in.Has("Life Left")) m_lifeLeft = in.Int("Life Left", 0);
        if (in.Has("Color")) m_color = in.Int("Color", m_color);
    }

    WhirlwindMoveGoal::WhirlwindMoveGoal(Whirlwind* whirlwind) : m_whirlwind(whirlwind) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    void WhirlwindMoveGoal::Tick() {
        // AbstractWhirlwind.MoveGoal.tick.
        if (m_movementAngle == 0.0f) {
            m_movementAngle = m_whirlwind->GetMovementAngle();
            m_movementCurve = m_whirlwind->GetMovementCurve();
        }
        EntityLevel* level = m_whirlwind->Level();
        if (!level) return;
        if (!m_whirlwind->IsEvil() || !m_whirlwind->GetTarget()) {
            const glm::dvec3 next = m_whirlwind->position + m_whirlwind->velocity;
            const glm::ivec3 p(static_cast<int>(std::floor(next.x)), static_cast<int>(std::floor(next.y)),
                               static_cast<int>(std::floor(next.z)));
            if (IsOverDrop(*level, p, m_whirlwind->GetMaxFallDistance())) {
                m_movementAngle += 180.0f;
            } else {
                m_movementAngle += m_movementCurve;
            }
            const double modifier = m_whirlwind->GetAttributeValue(Attribute::MovementSpeed);
            m_whirlwind->velocity.x = std::cos(m_movementAngle * Mth::kDegToRad) * modifier;
            m_whirlwind->velocity.z = std::sin(m_movementAngle * Mth::kDegToRad) * modifier;
        } else {
            m_whirlwind->velocity = glm::dvec3(0.0);
        }
        m_whirlwind->needsSync = true;
    }

    // ══ Aechor plant ═══════════════════════════════════════════════════════

    void AechorPlant::CreateAttributes(AttributeMap& out) {
        // Aether AechorPlant.createMobAttributes.
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,           15.0);
        out.Register(Attribute::MovementSpeed,        0.0);
        out.Register(Attribute::KnockbackResistance,  1.0);
    }

    AechorPlant::AechorPlant(EntityLevel* level)
        : PathfinderMob(EntityTypeId::AechorPlant, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        if (level && level->IsClientSide()) m_sinage = level->Random().NextFloat() * 6.0f;
        RegisterGoals();
    }

    void AechorPlant::RegisterGoals() {
        m_goalSelector.AddGoal(0, std::make_unique<RangedAttackGoal>(this, this, 1.0, 60, 10.0f));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    std::shared_ptr<SpawnGroupData>
    AechorPlant::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // AechorPlant.finalizeSpawn: size 1-4, snapped to the block centre.
        if (m_level) m_size = m_level->Random().NextInt(4) + 1;
        const glm::ivec3 bp = BlockPosition();
        position = glm::dvec3(bp.x + 0.5, bp.y, bp.z + 0.5);
        return PathfinderMob::FinalizeSpawn(reason, std::move(groupData));
    }

    void AechorPlant::Tick() {
        PathfinderMob::Tick();
        if (!m_level) return;
        if (!m_level->IsClientSide()) {
            // Dies off its soil (AECHOR_PLANT_SPAWNABLE_ON: aether grass).
            const glm::ivec3 bp = BlockPosition();
            const BlockID grass = AetherIds::AetherGrassBlock();
            const IBlockAccess* blocks = m_level->Blocks();
            if (blocks && grass != BlockID::Air && !IsPassenger() &&
                blocks->GetBlock(bp.x, bp.y - 1, bp.z) != grass) {
                // Mob.kill(): a genericKill hurt of Float.MAX_VALUE — a death
                // with its drops.
                Hurt(MobDamageSource::Generic, 3.4028235e38f, nullptr);
                return;
            }
            m_targetingEntity = GetTarget() != nullptr;
        }
    }

    void AechorPlant::AiStep() {
        PathfinderMob::AiStep();
        // AechorPlant.aiStep, client half: the sway clock.
        if (m_level && m_level->IsClientSide()) {
            m_sinage += m_sinageAdd;
            if (hurtTime > 0)            m_sinageAdd = 0.45f;
            else if (m_targetingEntity)  m_sinageAdd = 0.3f;
            else                         m_sinageAdd = 0.15f;
            if (m_sinage >= 2.0f * Mth::kPi) m_sinage -= 2.0f * Mth::kPi;
        }
    }

    void AechorPlant::PerformRangedAttack(LivingEntity& target, float power) {
        // AechorPlant.performRangedAttack — a lob.
        (void)power;
        if (!m_level) return;
        auto needle = MakePoisonNeedle(m_level, *this);
        double x = target.position.x - position.x;
        double z = target.position.z - position.z;
        const double sqrt = std::sqrt(x * x + z * z + 0.1);
        const double y = 0.1 + sqrt * 0.5 + (position.y - target.position.y) * 0.25;
        const double distance = 1.5 / sqrt;
        x *= distance;
        z *= distance;
        needle->Shoot(x, y + 0.5, z, 0.285f + static_cast<float>(y) * 0.08f, 1.0f);
        m_level->AddFreshEntity(std::move(needle));
    }

    void AechorPlant::SaveModNbt(ModNbtOut& out) const {
        out.Int("Size", m_size);
        out.Int("Poison Remaining", m_poisonRemaining);
    }

    void AechorPlant::LoadModNbt(const ModNbtIn& in) {
        if (in.Has("Size")) m_size = in.Int("Size", 0);
        if (in.Has("Poison Remaining")) m_poisonRemaining = in.Int("Poison Remaining", 2);
    }

    // ══ Mimic ══════════════════════════════════════════════════════════════

    void Mimic::CreateAttributes(AttributeMap& out) {
        // Aether Mimic.createMobAttributes (Monster's supplier).
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     40.0);
        out.Register(Attribute::AttackDamage,   3.0);
        out.Register(Attribute::MovementSpeed, 0.28);
        out.Register(Attribute::FollowRange,    8.0);
    }

    Mimic::Mimic(EntityLevel* level) : Monster(EntityTypeId::Mimic, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Mimic::RegisterGoals() {
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        // ContinuousMeleeAttackGoal(1.0, false) -> MeleeAttackGoal.
        m_goalSelector.AddGoal(2, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(5, std::make_unique<MoveTowardsRestrictionGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        // HurtByTargetGoal(this, Mimic.class): mimic damage is refused
        // outright in hurt(), so the ignore list is moot.
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    bool Mimic::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // Mimic.hurt: mimics cannot hurt mimics; a living attacker becomes
        // the target (creative players excepted). The chest-block burst is
        // the client's hurt particles.
        if (attacker && attacker->GetType() == EntityTypeId::Mimic) return false;
        if (auto* living = dynamic_cast<LivingEntity*>(attacker)) {
            if (hurtTime == 0 && !(living->IsPlayer() && living->IsCreative())) {
                SetTarget(living);
            }
        }
        return Monster::Hurt(source, amount, attacker);
    }

    void Mimic::DropCustomDeathLoot(EntityLevel& level) {
        // entities/mimic pool 1: the chest. Pool 2: uniform(1, 3) rolls of an
        // eleven-way even pick. Aether items resolve by slug; the ones this
        // port lacks fall back to the nearest vanilla item.
        if (const ItemID chest = AetherIds::ItemBySlug("chest"); chest != Items::Air) {
            level.SpawnItemDrop(position, chest, 1);
        }
        struct Junk { const char* slug; ItemID fallback; int count; };
        const Junk kJunk[] = {
            {"golden_dart",      Items::Arrow,         4},
            {"enchanted_dart",   Items::Arrow,         4},
            {"poison_dart",      Items::Arrow,         4},
            {"skyroot_pickaxe",  Items::WoodenPickaxe, 1},
            {"iron_ring",        Items::IronIngot,     1},
            {"golden_amber",     Items::GoldNugget,    1},
            {"zanite_gemstone",  Items::Emerald,       1},
            {"holystone_pickaxe", Items::StonePickaxe, 1},
            {"icestone",         Items::Snowball,      1},
            {"ambrosium_shard",  Items::GlowstoneDust, 1},
            {"ambrosium_torch",  Items::GlowstoneDust, 1},
        };
        JavaRandom& rng = level.Random();
        const int rolls = 1 + rng.NextInt(3);
        for (int i = 0; i < rolls; ++i) {
            const Junk& j = kJunk[rng.NextInt(static_cast<int>(std::size(kJunk)))];
            level.SpawnItemDrop(position, SlugOr(j.slug, j.fallback), j.count);
        }
    }

    // ══ Sentry ═════════════════════════════════════════════════════════════

    namespace {
        // Sentry's slime goals, each gated on the sentry being awake.
        template <typename Base>
        class AwakeGated : public Base {
        public:
            explicit AwakeGated(Sentry* sentry) : Base(sentry), m_sentry(sentry) {}
            bool CanUse() override { return m_sentry->IsAwake() && Base::CanUse(); }
            bool CanContinueToUse() override {
                return m_sentry->IsAwake() && Base::CanContinueToUse();
            }
        private:
            Sentry* m_sentry;
        };

        // The sentry's target: players within 4 blocks vertically.
        class SentryTargetGoal : public NearestAttackableTargetGoal {
        public:
            explicit SentryTargetGoal(Sentry* sentry)
                : NearestAttackableTargetGoal(sentry, true, false, 10), m_sentry(sentry) {}
            void Start() override {
                NearestAttackableTargetGoal::Start();
                LivingEntity* t = m_sentry->GetTarget();
                if (t && std::abs(t->position.y - m_sentry->position.y) > 4.0) {
                    m_sentry->SetTarget(nullptr);
                }
            }
        private:
            Sentry* m_sentry;
        };
    } // namespace

    Sentry::Sentry(EntityLevel* level) : Slime(EntityTypeId::Sentry, level) {
        // Sentry.createMobAttributes over Slime's size-1 defaults.
        m_attributes.SetBaseValue(Attribute::MaxHealth,     10.0);
        m_attributes.SetBaseValue(Attribute::MovementSpeed, 0.6);
        m_attributes.SetBaseValue(Attribute::AttackDamage,  2.0);
        m_health = GetMaxHealth();
        // Sentry.registerGoals replaces Slime's (already registered by the
        // base constructor).
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        m_goalSelector.AddGoal(1, std::make_unique<AwakeGated<SlimeFloatGoal>>(this));
        m_goalSelector.AddGoal(2, std::make_unique<AwakeGated<SlimeAttackGoal>>(this));
        m_goalSelector.AddGoal(3, std::make_unique<AwakeGated<SlimeRandomDirectionGoal>>(this));
        m_goalSelector.AddGoal(5, std::make_unique<AwakeGated<SlimeKeepOnJumpingGoal>>(this));
        m_targetSelector.AddGoal(1, std::make_unique<SentryTargetGoal>(this));
    }

    void Sentry::Tick() {
        // Sentry.tick: wake after 24 ticks near a player; sleep without one.
        if (m_level && !m_level->IsClientSide()) {
            LivingEntity* near = m_level->GetNearestPlayer(position.x, position.y, position.z, 8.0);
            if (near && !near->IsSpectator()) {
                if (!m_awake) {
                    if (m_timeSpotted >= 24.0f) m_awake = true;
                    m_timeSpotted += 1.0f;
                }
            } else {
                m_awake = false;
            }
        }
        Slime::Tick();
        if (!m_level || m_level->IsClientSide() || IsRemoved() || !m_awake) return;
        // Sentry.push(Entity) / playerTouch: anything living it touches.
        AABB box = GetAABB();
        box.min -= glm::vec3(0.1f);
        box.max += glm::vec3(0.1f);
        std::vector<Entity*> touching;
        m_level->GetEntitiesInBox(box, this, touching);
        for (Entity* e : touching) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || e->GetType() == EntityTypeId::Sentry) continue;
            if (living->IsPlayer() && (living->IsCreative() || living->IsSpectator())) continue;
            ExplodeAt(*living);
            if (IsRemoved()) return;
        }
    }

    void Sentry::JumpFromGround() {
        if (m_awake) Slime::JumpFromGround();
    }

    void Sentry::ExplodeAt(LivingEntity& entity) {
        // Sentry.explodeAt.
        if (DistanceToSqr(entity) >= 1.5 || !m_awake || tickCount <= 20 || !IsAlive()) return;
        if (!GetSensing().HasLineOfSight(entity)) return;
        if (!entity.Hurt(MobDamageSource::MobAttack, 1.0f, this)) return;
        entity.AddDeltaMovement(glm::dvec3(0.3, 0.4, 0.3));
        ExplosionParams params;
        params.center = position;
        params.radius = 1.0f;
        params.source = this;
        params.attributedTo = this;
        params.interaction = ExplosionInteraction::Mob;
        Game::Explode(*m_level, params);
        m_level->BroadcastEntityEvent(*this, 60);
        Discard();
    }

    void Sentry::DropCustomDeathLoot(EntityLevel& level) {
        // entities/sentry: one of carved stone / sentry stone (even pick).
        const bool sentryStone = level.Random().NextInt(2) == 1;
        const ItemID item = sentryStone
            ? SlugOr("sentry_stone", AetherIds::ItemBySlug("chiseled_stone_bricks"))
            : SlugOr("carved_stone", AetherIds::ItemBySlug("stone_bricks"));
        if (item != Items::Air) level.SpawnItemDrop(position, item, 1);
    }

    // ══ Valkyrie ═══════════════════════════════════════════════════════════

    void Valkyrie::CreateAttributes(AttributeMap& out) {
        // Aether Valkyrie.createMobAttributes (over AbstractValkyrie's).
        CreateMonsterAttributes(out);
        out.Register(Attribute::MovementSpeed,        0.5);
        out.Register(Attribute::KnockbackResistance, 0.25);
        out.Register(Attribute::FollowRange,         16.0);
        out.Register(Attribute::AttackDamage,        10.0);
        out.Register(Attribute::MaxHealth,           50.0);
    }

    Valkyrie::Valkyrie(EntityLevel* level) : Monster(EntityTypeId::Valkyrie, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetMoveControl(std::make_unique<ValkyrieMoveControl>(this));
        RegisterGoals();
    }

    void Valkyrie::RegisterGoals() {
        // AbstractValkyrie.registerGoals + Valkyrie's.
        m_goalSelector.AddGoal(1, std::make_unique<ValkyrieTeleportGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<ValkyrieLungeGoal>(this, 0.65, 30));
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 0.65, true));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.5));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        // MostDamageTargetGoal -> HurtByTargetGoal (see the header); the
        // NeutralMob anger goals collapse into the same retaliation.
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
    }

    void Valkyrie::Tick() {
        Monster::Tick();
        if (onGround) m_entityOnGround = true;
        if (m_level && !m_level->IsClientSide() && m_lungeCooldown > 0) --m_lungeCooldown;
        // Valkyrie.tick: a settling fall gets a small lift — the hover.
        const double motionY = velocity.y;
        const double change = std::abs(motionY - m_lastMotionY);
        if (!onGround && change > 0.07 && change < 0.09) {
            velocity.y += 0.0225;
            needsSync = true;
        }
    }

    void Valkyrie::Travel(const glm::dvec3& input) {
        m_lastMotionY = velocity.y;
        Monster::Travel(input);
    }

    void Valkyrie::JumpFromGround() {
        Monster::JumpFromGround();
        m_entityOnGround = false;
    }

    void Valkyrie::HandleEntityEvent(uint8_t id) {
        if (id == 70) {
            for (int i = 0; i < 5; ++i) MovementExplosionParticles(*this);
            return;
        }
        Monster::HandleEntityEvent(id);
    }

    bool Valkyrie::TeleportAroundTarget(Entity& target) {
        // AbstractValkyrie.teleportAroundTarget: a spot 3 blocks from the
        // target on a random heading, dropped up to 4 blocks to a floor that
        // is VALKYRIE_TELEPORTABLE_ON (locked angelic stone), then
        // randomTeleport (no liquid, no collision).
        if (!m_level) return false;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;
        JavaRandom& rng = m_level->Random();
        glm::dvec2 dir(rng.NextFloat() - 0.5f, rng.NextFloat() - 0.5f);
        const double len = glm::length(dir);
        if (len < 1.0e-6) return false;
        dir /= len;
        const double x = target.position.x + dir.x * 3.0;
        const double y = target.position.y;
        const double z = target.position.z + dir.y * 3.0;
        glm::ivec3 p(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
                     static_cast<int>(std::floor(z)));
        int i = 0;
        while (p.y > m_level->GetMinY() && !blocks->IsBlockSolid(p.x, p.y, p.z) && i <= 4) {
            --p.y;
            ++i;
        }
        static const BlockID kLocked = BlockStates::FromSlug("locked_angelic_stone").Block();
        static const BlockID kLockedLight = BlockStates::FromSlug("locked_light_angelic_stone").Block();
        const BlockID floor = blocks->GetBlock(p.x, p.y, p.z);
        if (floor == BlockID::Air || (floor != kLocked && floor != kLockedLight)) return false;
        // LivingEntity.randomTeleport onto the floor under (x, y, z).
        const glm::dvec3 dest(x, p.y + 1.0, z);
        const glm::dvec3 old = position;
        position = dest;
        if (CollidesAt(GetAABBd(), m_level->Physics()) ||
            blocks->IsBlockFluid(p.x, p.y + 1, p.z)) {
            position = old;
            return false;
        }
        GetNavigation().Stop();
        m_level->BroadcastEntityEvent(*this, 70);
        return true;
    }

    void Valkyrie::DropCustomDeathLoot(EntityLevel& level) {
        level.SpawnItemDrop(position, SlugOr("victory_medal", Items::GoldNugget), 1);
    }

    void ValkyrieMoveControl::Tick() {
        if (m_operation == Operation::Jumping) m_operation = Operation::MoveTo;
        MoveControl::Tick();
    }

    ValkyrieLungeGoal::ValkyrieLungeGoal(Valkyrie* valkyrie, double speed, int cooldownMax)
        : m_valkyrie(valkyrie), m_speed(speed), m_cooldownMax(cooldownMax) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool ValkyrieLungeGoal::CanUse() {
        return !m_valkyrie->onGround && m_valkyrie->GetLungeCooldown() <= 0;
    }

    void ValkyrieLungeGoal::Tick() {
        // AbstractValkyrie.LungeGoal.tick.
        LivingEntity* target = m_valkyrie->GetTarget();
        if (!target) return;
        double motionY = m_valkyrie->velocity.y;
        if (motionY < 0.2 && m_valkyrie->GetLastMotionY() >= 0.2 &&
            m_valkyrie->DistanceTo(*target) <= 16.0) {
            const double x = target->position.x - m_valkyrie->position.x;
            const double z = target->position.z - m_valkyrie->position.z;
            motionY -= 0.1;
            const double angle = std::atan2(x, z);
            m_valkyrie->velocity = glm::dvec3(std::sin(angle) * 0.3, motionY, std::cos(angle) * 0.3);
            m_valkyrie->yRot = static_cast<float>(angle) * Mth::kRadToDeg;
            m_valkyrie->needsSync = true;
            m_flyingTicks = 8;
        }
        if (m_flyingTicks > 0) {
            --m_flyingTicks;
            const double gravity = m_valkyrie->GetAttributeValue(Attribute::Gravity);
            const double fallSpeed = std::max(gravity * -0.625, -0.275);
            if (motionY < fallSpeed) {
                m_valkyrie->velocity.y = fallSpeed;
                m_valkyrie->needsSync = true;
                m_valkyrie->SetEntityOnGround(false);
            }
        }
        m_valkyrie->GetMoveControl().SetWantedPosition(target->position.x, target->position.y,
                                                       target->position.z, m_speed);
    }

    ValkyrieTeleportGoal::ValkyrieTeleportGoal(Valkyrie* valkyrie) : m_valkyrie(valkyrie) {}

    void ValkyrieTeleportGoal::Tick() {
        // ValkyrieTeleportGoal: 450 ticks between tries (the start offset a
        // random 0-199), 20 back on a failure.
        EntityLevel* level = m_valkyrie->Level();
        if (!level) return;
        if (m_teleportTimer < 0) m_teleportTimer = level->Random().NextInt(200);
        if (m_teleportTimer++ < 450) return;
        LivingEntity* target = m_valkyrie->GetTarget();
        if (target && m_valkyrie->TeleportAroundTarget(*target)) {
            m_teleportTimer = level->Random().NextInt(40);
        } else {
            m_teleportTimer -= 20;
        }
    }

    // ══ Fire minion ════════════════════════════════════════════════════════

    void FireMinion::CreateAttributes(AttributeMap& out) {
        // Aether FireMinion.createMobAttributes.
        CreateMonsterAttributes(out);
        out.Register(Attribute::FollowRange,   40.0);
        out.Register(Attribute::MovementSpeed, 0.25);
        out.Register(Attribute::AttackDamage,  15.0);
        out.Register(Attribute::MaxHealth,     40.0);
    }

    FireMinion::FireMinion(EntityLevel* level) : Monster(EntityTypeId::FireMinion, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void FireMinion::RegisterGoals() {
        // ContinuousMeleeAttackGoal(1.5, true) -> MeleeAttackGoal.
        m_goalSelector.AddGoal(3, std::make_unique<MeleeAttackGoal>(this, 1.5, true));
        m_goalSelector.AddGoal(4, std::make_unique<MoveTowardsRestrictionGoal>(this, 1.0));
        // FallingRandomStrollGoal(this, 1.0) -> WaterAvoidingRandomStrollGoal.
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, true));
    }

    void FireMinion::Tick() {
        Monster::Tick();
        // FireMinion.tick, client: one falling flame a tick (SMOKE here).
        if (m_level && m_level->IsClientSide()) {
            JavaRandom& rng = m_level->Random();
            const double d = rng.NextFloat() - 0.5f;
            const double d1 = rng.NextFloat();
            const double d2 = rng.NextFloat() - 0.5f;
            m_level->AddParticle(ParticleKind::Smoke, position.x + d * d1, position.y + d1 + 0.5,
                                 position.z + d2 * d1, 0.0, -0.075, 0.0);
        }
    }

    bool FireMinion::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // FireMinion.hurt: a snowball's direct hit adds 3.
        if (attacker && attacker->GetType() == EntityTypeId::Snowball) amount += 3.0f;
        return Monster::Hurt(source, amount, attacker);
    }

} // namespace Game
