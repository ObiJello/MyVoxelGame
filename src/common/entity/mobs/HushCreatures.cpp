// File: src/common/entity/mobs/HushCreatures.cpp
#include "common/entity/mobs/HushCreatures.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/ModMobNbt.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/RestrictionGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/mobs/TheUnsung.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace Game {

    namespace {

        constexpr float kTwoPi = 6.2831855f;

        // MC Entity.getLookAngle's yaw half: the unit facing for a yRot.
        glm::dvec3 FacingXZ(float yRotDeg) {
            const double r = static_cast<double>(yRotDeg) * Mth::kDegToRad;
            return glm::dvec3(-std::sin(r), 0.0, std::cos(r));
        }

        // The yRot that faces along (dx, dz) — MC's atan2(dz, dx) - 90.
        float YawToward(double dx, double dz) {
            return static_cast<float>(std::atan2(dz, dx) * Mth::kRadToDeg) - 90.0f;
        }

        void FaceVelocity(Mob& mob) {
            const glm::dvec3& v = mob.velocity;
            if (v.x * v.x + v.z * v.z < 1.0e-6) return;
            mob.yRot = YawToward(v.x, v.z);
            mob.yBodyRot = mob.yRot;
            mob.SetYHeadRot(mob.yRot);
        }

        // The first solid block's top at or below `p`, scanning `depth`
        // blocks down; p.y - depth when there is none.
        double FloorUnder(const IBlockAccess* blocks, const glm::dvec3& p, int depth) {
            const int x = static_cast<int>(std::floor(p.x));
            const int z = static_cast<int>(std::floor(p.z));
            const int top = static_cast<int>(std::floor(p.y));
            if (!blocks) return p.y - depth;
            for (int y = top; y > top - depth; --y) {
                if (blocks->IsBlockSolid(x, y, z)) return static_cast<double>(y + 1);
            }
            return p.y - depth;
        }

        // Every player (as the level exposes them) that is alive, not a
        // spectator, and within `range` of `from`.
        void PlayersNear(EntityLevel& level, const glm::dvec3& from, double range,
                         std::vector<LivingEntity*>& out) {
            std::vector<LivingEntity*> all;
            level.GetPlayers(all);
            const double r2 = range * range;
            for (LivingEntity* p : all) {
                if (!p || !p->IsAlive() || p->IsSpectator()) continue;
                const glm::dvec3 d = p->position - from;
                if (glm::dot(d, d) <= r2) out.push_back(p);
            }
        }

    } // namespace

    // ══ Light sources ══════════════════════════════════════════════════════

    bool IsHushLightBlock(BlockID id) {
        if (id == BlockID::Air) return false;
        if (BlockRegistry::Get(id).emissive) return true;
        switch (id) {
            case BlockID::Torch:           case BlockID::WallTorch:
            case BlockID::SoulTorch:       case BlockID::SoulWallTorch:
            case BlockID::CopperTorch:     case BlockID::CopperWallTorch:
            case BlockID::Lantern:         case BlockID::SoulLantern:
            case BlockID::CopperLantern:   case BlockID::ExposedCopperLantern:
            case BlockID::OxidizedCopperLantern:
            case BlockID::Glowstone:       case BlockID::SeaLantern:
            case BlockID::Shroomlight:     case BlockID::JackOLantern:
            case BlockID::EndRod:          case BlockID::Campfire:
            case BlockID::SoulCampfire:
                return true;
            default:
                return false;
        }
    }

    bool IsHushLightItem(uint32_t itemId) {
        if (itemId == 0 || itemId >= PURE_ITEM_BASE) return false;
        return IsHushLightBlock(static_cast<BlockID>(itemId));
    }

    bool FindNearestLightBlock(const IBlockAccess& blocks, const glm::dvec3& from,
                               int radius, int yRadius, glm::ivec3& out) {
        const int cx = static_cast<int>(std::floor(from.x));
        const int cy = static_cast<int>(std::floor(from.y));
        const int cz = static_cast<int>(std::floor(from.z));
        int best = -1;
        for (int dy = -yRadius; dy <= yRadius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    const int d2 = dx * dx + dy * dy + dz * dz;
                    if (best >= 0 && d2 >= best) continue;
                    if (!IsHushLightBlock(blocks.GetBlock(cx + dx, cy + dy, cz + dz))) continue;
                    best = d2;
                    out = glm::ivec3(cx + dx, cy + dy, cz + dz);
                }
            }
        }
        return best >= 0;
    }

    // ══ Lumen Moth ═════════════════════════════════════════════════════════

    void LumenMoth::CreateAttributes(AttributeMap& out) {
        // The bat's 6 HP, a little frailer: a moth.
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth, 4.0);
    }

    LumenMoth::LumenMoth(EntityLevel* level) : Mob(EntityTypeId::LumenMoth, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetNoGravity(true);
        if (level) {
            m_orbitDir = level->Random().NextBool() ? 1 : -1;
            m_orbitAngle = level->Random().NextFloat() * kTwoPi;
            m_orbitRadius = 1.2f + level->Random().NextFloat() * 0.9f;
        }
    }

    void LumenMoth::Travel(const glm::dvec3& input) {
        // Steered, not walked: CustomServerAiStep writes the velocity (the
        // bat's approach); this only moves and damps it.
        (void)input;
        Move(velocity);
        velocity *= 0.9;
    }

    void LumenMoth::ClearReferenceTo(const Entity* entity) {
        Mob::ClearReferenceTo(entity);
        if (m_lightCarrier == entity) m_lightCarrier = nullptr;
    }

    void LumenMoth::RescanLight() {
        m_lightCarrier = nullptr;
        m_hasLight = false;
        if (!m_level) return;
        double bestSq = 1.0e30;

        // A player carrying a light within 16 blocks wins over the blocks
        // when nearer — the moth follows the lantern-bearer.
        std::vector<LivingEntity*> players;
        PlayersNear(*m_level, position, 16.0, players);
        for (LivingEntity* p : players) {
            if (!IsHushLightItem(m_level->GetHeldItemId(*p))) continue;
            const double d = DistanceToSqr(*p);
            if (d < bestSq) {
                bestSq = d;
                m_lightCarrier = p;
                m_light = p->position + glm::dvec3(0.0, 1.3, 0.0);
                m_hasLight = true;
            }
        }
        glm::ivec3 cell;
        if (const IBlockAccess* blocks = m_level->Blocks();
            blocks && FindNearestLightBlock(*blocks, position, 10, 6, cell)) {
            const glm::dvec3 c(cell.x + 0.5, cell.y + 0.5, cell.z + 0.5);
            const glm::dvec3 d = c - position;
            if (glm::dot(d, d) < bestSq) {
                m_lightCarrier = nullptr;
                m_light = c;
                m_hasLight = true;
            }
        }
    }

    void LumenMoth::CustomServerAiStep() {
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();

        if (--m_rescanIn <= 0) {
            RescanLight();
            // Staggered by id so a swarm does not scan on the same tick.
            m_rescanIn = 40 + static_cast<int>(static_cast<uint32_t>(GetId()) % 10u);
        }
        if (m_lightCarrier) {
            if (!m_lightCarrier->IsAlive() ||
                !IsHushLightItem(m_level->GetHeldItemId(*m_lightCarrier))) {
                m_lightCarrier = nullptr;
                m_hasLight = false;
                m_rescanIn = 0;
            } else {
                m_light = m_lightCarrier->position + glm::dvec3(0.0, 1.3, 0.0);
            }
        }

        glm::dvec3 target;
        double speed;
        if (m_hasLight) {
            // Circle the light: an orbit that breathes in and out and bobs,
            // each moth on its own radius and direction.
            m_orbitAngle += 0.11f * static_cast<float>(m_orbitDir);
            if (rng.NextInt(200) == 0) m_orbitDir = -m_orbitDir;
            if (rng.NextInt(80) == 0) m_orbitRadius = 1.0f + rng.NextFloat() * 1.3f;
            const double r = m_orbitRadius + 0.25 * std::sin(tickCount * 0.07);
            target = m_light + glm::dvec3(r * std::cos(m_orbitAngle),
                                          0.2 + 0.4 * std::sin(tickCount * 0.09 + GetId()),
                                          r * std::sin(m_orbitAngle));
            speed = 0.2;
        } else {
            // The bat's wander (Bat.customServerAiStep): a fresh cell within
            // a few blocks now and then, or once reached or blocked.
            const glm::dvec3 w(m_wander.x + 0.5, m_wander.y + 0.1, m_wander.z + 0.5);
            const glm::dvec3 d = w - position;
            if (!m_hasWander || rng.NextInt(30) == 0 || glm::dot(d, d) < 4.0 ||
                horizontalCollision) {
                m_wander = glm::ivec3(
                    static_cast<int>(std::floor(position.x)) + rng.NextInt(7) - rng.NextInt(7),
                    static_cast<int>(std::floor(position.y)) + rng.NextInt(6) - 2,
                    static_cast<int>(std::floor(position.z)) + rng.NextInt(7) - rng.NextInt(7));
                // Keep to the air a few blocks over the ground.
                const double floor = FloorUnder(m_level->Blocks(), glm::dvec3(m_wander) +
                                                glm::dvec3(0.5, 0.0, 0.5), 16);
                m_wander.y = std::clamp(m_wander.y, static_cast<int>(floor) + 1,
                                        static_cast<int>(floor) + 6);
                m_hasWander = true;
            }
            target = glm::dvec3(m_wander.x + 0.5, m_wander.y + 0.1, m_wander.z + 0.5);
            speed = 0.12;
        }

        glm::dvec3 dir = target - position;
        const double len = glm::length(dir);
        if (len > 1.0e-4) dir /= len;
        const glm::dvec3 want = dir * speed * std::min(1.0, len);
        velocity += (want - velocity) * 0.25;
        FaceVelocity(*this);
    }

    void LumenMoth::Tick() {
        Mob::Tick();
        // A faint trail of motes behind a glowing moth (client only).
        if (m_level && m_level->IsClientSide() && m_level->Random().NextInt(m_hasLight ? 5 : 12) == 0) {
            JavaRandom& rng = m_level->Random();
            m_level->AddParticle(ParticleKind::HushMote,
                                 position.x + (rng.NextDouble() - 0.5) * 0.3,
                                 position.y + 0.15,
                                 position.z + (rng.NextDouble() - 0.5) * 0.3,
                                 0.0, -0.01, 0.0);
        }
    }

    // ══ Crystal Golem ══════════════════════════════════════════════════════

    void CrystalGolem::CreateAttributes(AttributeMap& out) {
        // The iron golem's frame, slower and a little lighter-hitting, with
        // a real shove: MAX_HEALTH 80, MOVEMENT_SPEED 0.2, full knockback
        // resistance, ATTACK_DAMAGE 12, ATTACK_KNOCKBACK 1.5, STEP_HEIGHT 1.
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,          80.0);
        out.Register(Attribute::MovementSpeed,       0.2);
        out.Register(Attribute::KnockbackResistance, 1.0);
        out.Register(Attribute::AttackDamage,       12.0);
        out.Register(Attribute::AttackKnockback,     1.5);
        out.Register(Attribute::StepHeight,          1.0);
        out.Register(Attribute::FollowRange,        20.0);
        out.Register(Attribute::Armor,               6.0);
    }

    CrystalGolem::CrystalGolem(EntityLevel* level)
        : PathfinderMob(EntityTypeId::CrystalGolem, level), NeutralMob(this) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void CrystalGolem::RegisterGoals() {
        // IronGolem.registerGoals' shape: melee, a wander (here tethered to
        // the spire field it guards), look goals; targets are the hurt-by
        // grudge and the NeutralMob isAngryAt player hunt.
        m_goalSelector.AddGoal(1, std::make_unique<MeleeAttackGoal>(this, 1.15, true));
        m_goalSelector.AddGoal(3, std::make_unique<MoveTowardsRestrictionGoal>(this, 0.8));
        m_goalSelector.AddGoal(4, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.6));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        auto angryAt = std::make_unique<NearestAttackableTargetGoal>(
            this, /*mustSee=*/false, /*mustReach=*/false, /*randomInterval=*/10);
        angryAt->SetSelector([](Mob& mob, const LivingEntity& target) {
            return static_cast<CrystalGolem&>(mob).IsAngryAt(target);
        });
        m_targetSelector.AddGoal(2, std::move(angryAt));
        m_targetSelector.AddGoal(3, std::make_unique<ResetUniversalAngerTargetGoal>(
                                        this, /*alertOthersOfSameType=*/true));
    }

    std::shared_ptr<SpawnGroupData>
    CrystalGolem::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // Guard the field it was found in.
        SetHomeTo(BlockPosition(), 16);
        return PathfinderMob::FinalizeSpawn(reason, std::move(groupData));
    }

    void CrystalGolem::StartPersistentAngerTimer() {
        // A longer memory than the iron golem's 20-39 s: 30-59 s.
        if (!m_level) return;
        SetTimeToRemainAngry(600 + m_level->Random().NextInt(581));
    }

    void CrystalGolem::ProvokeBy(LivingEntity& breaker) {
        if (!breaker.IsAlive() || breaker.IsSpectator()) return;
        SetPersistentAngerTarget(&breaker);
        StartPersistentAngerTimer();
        SetTarget(&breaker);
        m_angryVisible = true;
    }

    void CrystalGolem::AiStep() {
        // IronGolem.aiStep: super, the swing clock on both sides, then the
        // server's anger bookkeeping.
        PathfinderMob::AiStep();
        if (m_attackAnimationTick > 0) --m_attackAnimationTick;
        if (m_level && !m_level->IsClientSide()) {
            UpdatePersistentAnger(/*stayAngryIfTargetPresent=*/true);
            m_angryVisible = GetTarget() != nullptr || IsAngry();
        }
    }

    bool CrystalGolem::DoHurtTarget(Entity& target) {
        m_attackAnimationTick = 10;
        if (m_level) m_level->BroadcastEntityEvent(*this, 4);
        // Mob.doHurtTarget deals ATTACK_DAMAGE and applies the
        // ATTACK_KNOCKBACK shove; the golem adds IronGolem's 0.4 lift.
        const bool hurt = PathfinderMob::DoHurtTarget(target);
        if (hurt) {
            if (LivingEntity* living = target.AsLiving()) {
                const double kbr = living->GetAttributeValue(Attribute::KnockbackResistance);
                living->velocity.y += 0.4 * std::max(0.0, 1.0 - kbr);
                living->hurtMarked = true;
            }
        }
        // The crystal's crack (obeycraft overlay: amethyst block hit).
        PlaySound("obeycraft:entity.crystal_golem.attack", 1.0f, 0.6f);
        return hurt;
    }

    void CrystalGolem::HandleEntityEvent(uint8_t id) {
        if (id == 4) {
            m_attackAnimationTick = 10;
        } else {
            PathfinderMob::HandleEntityEvent(id);
        }
    }

    void CrystalGolem::DropCustomDeathLoot(EntityLevel& level) {
        // 1-3 resonant crystal. A block item: the loot generator resolves
        // pure items only (the table carries the raw resonite), so the
        // crystal rides the custom-drop hook the way the hushling's sculk
        // does.
        const int n = 1 + level.Random().NextInt(3);
        level.SpawnItemDrop(position, ItemRegistry::FromBlock(BlockID::ResonantCrystal), n);
    }

    void CrystalGolem::SaveModNbt(ModNbtOut& out) const {
        out.Bool("HasHome", HasHome());
        if (HasHome()) {
            const glm::ivec3 h = GetHomePosition();
            out.Int("HomeX", h.x);
            out.Int("HomeY", h.y);
            out.Int("HomeZ", h.z);
        }
    }

    void CrystalGolem::LoadModNbt(const ModNbtIn& in) {
        if (in.Bool("HasHome", false)) {
            SetHomeTo(glm::ivec3(in.Int("HomeX", 0), in.Int("HomeY", 0), in.Int("HomeZ", 0)), 16);
        }
    }

    void OnResonantCrystalBroken(EntityLevel& level, const glm::ivec3& pos,
                                 LivingEntity& breaker) {
        const glm::vec3 c(pos.x + 0.5f, pos.y + 0.5f, pos.z + 0.5f);
        const AABB box = AABB::FromMinMax(c - glm::vec3(16.0f), c + glm::vec3(16.0f));
        std::vector<Entity*> nearby;
        level.GetEntitiesInBox(box, nullptr, nearby);
        for (Entity* e : nearby) {
            if (!e || e->IsRemoved() || e->GetType() != EntityTypeId::CrystalGolem) continue;
            // Spherical 16, like the alarm radius the doc names.
            const glm::dvec3 d = e->position - glm::dvec3(c);
            if (glm::dot(d, d) > 16.0 * 16.0) continue;
            static_cast<CrystalGolem*>(e)->ProvokeBy(breaker);
        }
    }

    // ══ Hush Leviathan ═════════════════════════════════════════════════════

    void HushLeviathan::CreateAttributes(AttributeMap& out) {
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,           60.0);
        out.Register(Attribute::FlyingSpeed,          0.12);
        out.Register(Attribute::KnockbackResistance,  1.0);
    }

    HushLeviathan::HushLeviathan(EntityLevel* level)
        : Mob(EntityTypeId::HushLeviathan, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetNoGravity(true);
    }

    void HushLeviathan::Travel(const glm::dvec3& input) {
        // Aerwhale.travel (FlyingMob's, riderless): move and damp.
        (void)input;
        Move(velocity);
        velocity *= 0.91;
    }

    double HushLeviathan::GroundBelow() const {
        return FloorUnder(m_level ? m_level->Blocks() : nullptr, position, 128);
    }

    void HushLeviathan::PickCourse() {
        // A waypoint 64-128 blocks out within 70 degrees of the current
        // heading; the capped turn rate below turns each leg into a long
        // arc rather than a zig-zag.
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();
        const float yaw = yRot + (rng.NextFloat() * 2.0f - 1.0f) * 70.0f;
        const double dist = 64.0 + rng.NextDouble() * 64.0;
        const glm::dvec3 f = FacingXZ(yaw);
        m_course = position + f * dist;
        m_course.y = m_groundY + 26.0 + rng.NextDouble() * 18.0;
        m_course.y = std::min(m_course.y, static_cast<double>(m_level->GetMaxY() - 8));
        m_hasCourse = true;
    }

    void HushLeviathan::CustomServerAiStep() {
        if (!m_level) return;
        if (--m_altitudeCheckIn <= 0) {
            m_groundY = GroundBelow();
            m_altitudeCheckIn = 20;
        }
        const double dx = m_course.x - position.x;
        const double dz = m_course.z - position.z;
        if (!m_hasCourse || dx * dx + dz * dz < 36.0 || horizontalCollision) {
            if (horizontalCollision) m_groundY = std::max(m_groundY, position.y);
            PickCourse();
        }

        // Yaw toward the course at 0.6 degrees a tick — a full turn takes
        // ten seconds, so every path is a long, slow curve.
        const float wantYaw = YawToward(m_course.x - position.x, m_course.z - position.z);
        yRot = Mth::ApproachDegrees(yRot, wantYaw, 0.6f);
        yBodyRot = yRot;
        SetYHeadRot(yRot);

        // Hold at least 22 blocks over the ground under it; ease toward the
        // course height otherwise.
        const double wantY = std::max(m_course.y, m_groundY + 22.0);
        const double dy = wantY - position.y;
        const double vy = std::clamp(dy * 0.02, -0.05, 0.05);
        const float wantPitch = static_cast<float>(-std::atan2(vy, 0.12) * Mth::kRadToDeg);
        xRot = Mth::ApproachDegrees(xRot, std::clamp(wantPitch, -15.0f, 15.0f), 0.3f);

        const double speed = GetAttributeValue(Attribute::FlyingSpeed);
        const glm::dvec3 f = FacingXZ(yRot);
        velocity = glm::dvec3(f.x * speed, vy, f.z * speed);
        needsSync = true;
    }

    bool HushLeviathan::CheckSpawnObstruction(EntityLevel& level) const {
        // One per large area: no other leviathan within kExclusionRadius.
        const glm::vec3 c(static_cast<float>(position.x), static_cast<float>(position.y),
                          static_cast<float>(position.z));
        const float r = static_cast<float>(kExclusionRadius);
        const AABB area = AABB::FromMinMax(c - glm::vec3(r, 128.0f, r), c + glm::vec3(r, 128.0f, r));
        std::vector<Entity*> nearby;
        level.GetEntitiesInBox(area, this, nearby);
        for (Entity* e : nearby) {
            if (e && !e->IsRemoved() && e->GetType() == EntityTypeId::HushLeviathan) return false;
        }
        // The body goes 30 blocks up (FinalizeSpawn): that air must be clear.
        AABBd box = GetAABBd();
        box.min.y += 30.0;
        box.max.y += 30.0;
        return !CollidesAt(box, level.Physics());
    }

    std::shared_ptr<SpawnGroupData>
    HushLeviathan::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (reason == SpawnReason::Natural || reason == SpawnReason::ChunkGeneration) {
            position.y += 30.0;
        }
        if (m_level) yRot = m_level->Random().NextFloat() * 360.0f;
        m_groundY = GroundBelow();
        return Mob::FinalizeSpawn(reason, std::move(groupData));
    }

    // ══ Echo Mimic ═════════════════════════════════════════════════════════

    void EchoMimic::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,     24.0);
        out.Register(Attribute::MovementSpeed,  0.3);
        out.Register(Attribute::AttackDamage,   5.0);
        out.Register(Attribute::FollowRange,   32.0);
    }

    EchoMimic::EchoMimic(EntityLevel* level) : Monster(EntityTypeId::EchoMimic, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void EchoMimic::RegisterGoals() {
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<EchoMimicStalkGoal>(this));
        m_goalSelector.AddGoal(7, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.8));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        // Needs no line of sight: it follows where you WERE.
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackablePlayerGoal>(this, false));
    }

    void EchoMimic::RecordTrail(const LivingEntity& target) {
        EchoSample s;
        s.pos       = target.position;
        // A player's yRot IS its look yaw (PlayerEntityView mirrors the
        // client's), and its head never leaves it.
        s.yRot      = target.yRot;
        s.xRot      = target.xRot;
        s.onGround  = target.onGround;
        s.crouching = target.IsDiscrete();
        // LivingEntity::Swing parks swingTime at -1 and UpdateSwingTime
        // steps it to 0 on the next tick, so "swinging at 0" is true on
        // exactly one tick per swing (a player's view runs that clock in
        // PlayerEntityView::TickCombatState).
        s.swing     = target.swinging && target.swingTime == 0;
        if (m_trailCount > 0) {
            const EchoSample& prev = m_trail[TrailIndex(0)];
            s.fresh = s.pos != prev.pos || s.yRot != prev.yRot || s.xRot != prev.xRot;
        } else {
            s.fresh = true;
        }
        m_trail[m_trailHead] = s;
        m_trailHead = (m_trailHead + 1) % kTrailSize;
        if (m_trailCount < kTrailSize) ++m_trailCount;
    }

    bool EchoMimic::SampleAt(int delay, EchoSample& out) const {
        if (delay < 0 || delay >= m_trailCount) return false;
        out = m_trail[TrailIndex(delay)];
        if (out.fresh) return true;

        // No move arrived on this tick. Find the last tick that brought one
        // (older) and the next (newer); when both are within the bridge the
        // gap was the network's, and the position and look are eased across
        // it. The flags stay this tick's own: a swing or a crouch is an
        // event, not something to smear.
        int older = -1;
        for (int d = delay + 1; d <= delay + kBridgeTicks && d < m_trailCount; ++d) {
            if (m_trail[TrailIndex(d)].fresh) { older = d; break; }
        }
        int newer = -1;
        for (int d = delay - 1; d >= 0 && d >= delay - kBridgeTicks; --d) {
            if (m_trail[TrailIndex(d)].fresh) { newer = d; break; }
        }
        if (older < 0 || newer < 0 || older - newer > kBridgeTicks + 1) return true;

        const EchoSample& a = m_trail[TrailIndex(older)];
        const EchoSample& b = m_trail[TrailIndex(newer)];
        const float t = static_cast<float>(older - delay) / static_cast<float>(older - newer);
        out.pos  = a.pos + (b.pos - a.pos) * static_cast<double>(t);
        out.yRot = Mth::RotLerp(t, a.yRot, b.yRot);
        out.xRot = Mth::Lerp(t, a.xRot, b.xRot);
        return true;
    }

    void EchoMimic::SetMirroring(bool on) {
        m_mirroring = on;
        if (!on) {
            m_mirrorPending = false;
            SetPose(Pose::Standing);
        }
    }

    void EchoMimic::MirrorThisTick(const EchoSample& s, double maxHorizontal, double maxVertical) {
        m_mirror = s;
        m_mirrorMaxHorizontal = maxHorizontal;
        m_mirrorMaxVertical = maxVertical;
        m_mirrorPending = true;
        m_mirroring = true;
        SetPose(s.crouching ? Pose::Crouching : Pose::Standing);
    }

    void EchoMimic::Travel(const glm::dvec3& input) {
        if (!m_mirrorPending || !m_level || m_level->IsClientSide()) {
            Monster::Travel(input);
            return;
        }
        m_mirrorPending = false;

        // Onto the recorded position — at most one glide step away, so a
        // mimic still settling onto the trail eases on rather than snapping.
        // Moved, not placed: blocks built across the path since still stop
        // it (and the stalk goal then drops it off the trail).
        glm::dvec3 step = m_mirror.pos - position;
        const double horizontal = std::sqrt(step.x * step.x + step.z * step.z);
        if (horizontal > m_mirrorMaxHorizontal && horizontal > 0.0) {
            const double k = m_mirrorMaxHorizontal / horizontal;
            step.x *= k;
            step.z *= k;
        }
        step.y = std::clamp(step.y, -m_mirrorMaxVertical, m_mirrorMaxVertical);
        // A player on the ground is pressed into it by gravity every tick;
        // without the same press a level step touches nothing below, and the
        // echo would read as airborne (no footsteps, and a ground-change
        // resync to every watcher each time it lands).
        if (m_mirror.onGround && step.y <= 0.0) step.y -= 0.0784;
        Move(step);
        // It walks the fall, it does not take it: the player may have
        // landed in the water they placed, or taken the damage themselves.
        ResetFallDistance();

        // The recorded look. yRot is a player's look yaw and their head's;
        // the body follows in TickHeadTurn by a player's own rule.
        yRot     = m_mirror.yRot;
        xRot     = m_mirror.xRot;
        yHeadRot = m_mirror.yRot;
    }

    void EchoMimic::TickHeadTurn(float yBodyRotTarget) {
        if (!m_mirroring) {
            Monster::TickHeadTurn(yBodyRotTarget);
            return;
        }
        // MC LivingEntity.tickHeadTurn with Player's 50-degree neck
        // (getMaxHeadRotationRelativeToBody; the 15-degree shield case never
        // arises). LivingEntity::Tick has already chosen the target — the
        // travel direction, flipped when walking backwards, or the look yaw
        // mid-swing — exactly as it does for a player.
        yBodyRot += Mth::WrapDegrees(yBodyRotTarget - yBodyRot) * 0.3f;
        const float headDiff = Mth::WrapDegrees(yRot - yBodyRot);
        constexpr float kMaxHeadRotation = 50.0f;
        if (std::abs(headDiff) > kMaxHeadRotation) {
            yBodyRot += headDiff - (headDiff > 0.0f ? kMaxHeadRotation : -kMaxHeadRotation);
        }
    }

    void EchoMimic::Knockback(double power, double dx, double dz) {
        Monster::Knockback(power, dx, dz);
        m_pushedTicks = kPushedTicks;
    }

    void EchoMimic::AddDeltaMovement(const glm::dvec3& v) {
        Monster::AddDeltaMovement(v);
        // A current's per-tick nudge is hundredths of a block; a blast or a
        // wind charge is tenths and more.
        if (glm::dot(v, v) > 0.1 * 0.1) m_pushedTicks = kPushedTicks;
    }

    float EchoMimic::BaseBbHeight() const {
        return GetPose() == Pose::Crouching ? kCrouchHeight : Monster::BaseBbHeight();
    }

    float EchoMimic::BaseEyeHeight() const {
        return GetPose() == Pose::Crouching ? kCrouchEyeHeight : Monster::BaseEyeHeight();
    }

    void EchoMimic::AiStep() {
        Monster::AiStep();
        if (!m_level || m_level->IsClientSide()) return;
        if (m_pushedTicks > 0) --m_pushedTicks;

        if (--m_lightCheckIn <= 0) {
            m_lightCheckIn = 10;
            m_nearLight = m_touchingLight = false;
            glm::ivec3 cell;
            if (const IBlockAccess* blocks = m_level->Blocks();
                blocks && FindNearestLightBlock(*blocks, position + glm::dvec3(0.0, 1.0, 0.0),
                                                4, 3, cell)) {
                m_nearLight = true;
                const glm::dvec3 d = glm::dvec3(cell) + glm::dvec3(0.5) - position;
                m_touchingLight = glm::dot(d, d) <= 2.5 * 2.5;
            }
        }
        // The slow burn next to a light: 1 a second, doubled by Hurt below.
        if (m_touchingLight && tickCount % 20 == 0 && IsAlive()) {
            Hurt(MobDamageSource::Magic, 1.0f, nullptr);
        }

        m_revealed = m_level->GetNearestPlayer(position.x, position.y, position.z,
                                               kRevealDistance) != nullptr ||
                     hurtTime > 0;
    }

    bool EchoMimic::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // Light is what it cannot stand: double damage near one.
        if (m_nearLight) amount *= 2.0f;
        return Monster::Hurt(source, amount, attacker);
    }

    EchoMimicStalkGoal::EchoMimicStalkGoal(EchoMimic* mob) : m_mob(mob) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool EchoMimicStalkGoal::CanUse() {
        LivingEntity* t = m_mob->GetTarget();
        return t && t->IsAlive();
    }

    bool EchoMimicStalkGoal::CanContinueToUse() {
        LivingEntity* t = m_mob->GetTarget();
        return t && t->IsAlive() && m_mob->DistanceToSqr(*t) < 48.0 * 48.0;
    }

    namespace {
        // Close enough to the delayed position to start replaying it.
        constexpr double kTrailLockHorizontal = 0.8;
        constexpr double kTrailLockVertical   = 1.25;
        // Farther than this from it while replaying: something held the
        // mimic back (a block placed across the path since, a gap its
        // taller body cannot pass) and it has to walk back on.
        constexpr double kTrailLoseDistance   = 1.5;
        // What one replay step may add to the recorded step, per axis group,
        // while the mimic settles onto the trail.
        constexpr double kTrailGlide          = 0.12;
        // A recorded step longer than this is a teleport (a pearl, a
        // portal, /tp), not a walk: the echo does not follow it.
        constexpr double kTrailTeleportStep   = 4.0;
        // Walking onto the trail — a little quicker than it replays you, so
        // it can catch the trail up.
        constexpr double kTrailCatchUpSpeed   = 1.2;
    }

    void EchoMimicStalkGoal::Start() {
        m_mob->ClearTrail();
        m_attackCooldown = 0;
        m_trailTargetId = -1;
        m_onTrail = false;
        m_repathIn = 0;
    }

    void EchoMimicStalkGoal::Stop() {
        m_mob->ClearTrail();
        m_mob->GetNavigation().Stop();
        m_mob->SetMirroring(false);
        m_mob->SetAggressive(false);
        m_trailTargetId = -1;
        m_onTrail = false;
    }

    void EchoMimicStalkGoal::Tick() {
        LivingEntity* target = m_mob->GetTarget();
        if (!target) return;
        // A new target (a hit turned it on someone else): the trail it holds
        // is another player's.
        if (target->GetId() != m_trailTargetId) {
            m_mob->ClearTrail();
            m_trailTargetId = target->GetId();
            m_onTrail = false;
        }
        m_mob->RecordTrail(*target);
        if (m_attackCooldown > 0) --m_attackCooldown;

        if (m_mob->IsWithinMeleeAttackRange(*target)) {
            // Caught up: it stops echoing, turns on the real you (the one
            // time it looks at you rather than where you looked), and
            // strikes. Arms up while it does — MC's aggressive flag.
            m_onTrail = false;
            m_mob->SetMirroring(false);
            m_mob->SetAggressive(true);
            m_mob->GetNavigation().Stop();
            m_mob->GetLookControl().SetLookAt(target->position.x, target->GetEyeY(),
                                              target->position.z);
            m_mob->GetMoveControl().SetWantedPosition(target->position.x, target->position.y,
                                                      target->position.z, 1.2);
            if (m_attackCooldown <= 0) {
                m_mob->Swing();
                m_mob->DoHurtTarget(*target);
                m_attackCooldown = 20;
            }
            return;
        }
        m_mob->SetAggressive(false);

        // Otherwise it is you, two seconds late: where you walked, where you
        // looked, your crouch and your swings — and it stands watching you
        // until it has two seconds of you to replay.
        EchoMimic::EchoSample s;
        if (!m_mob->SampleAt(EchoMimic::kDelayTicks, s)) {
            m_mob->SetMirroring(false);
            m_mob->GetLookControl().SetLookAt(target->position.x, target->GetEyeY(),
                                              target->position.z);
            return;
        }
        EchoMimic::EchoSample before;
        const glm::dvec3 recordedStep = m_mob->SampleAt(EchoMimic::kDelayTicks + 1, before)
            ? s.pos - before.pos : glm::dvec3(0.0);
        const double recordedH = std::sqrt(recordedStep.x * recordedStep.x +
                                           recordedStep.z * recordedStep.z);
        const bool teleported = glm::dot(recordedStep, recordedStep) >
                                kTrailTeleportStep * kTrailTeleportStep;

        const glm::dvec3 off = s.pos - m_mob->position;
        const double offH = std::sqrt(off.x * off.x + off.z * off.z);
        if (m_onTrail) {
            // Knocked off it (the hit's knockback plays out under ordinary
            // physics), held back, or the trail jumped away.
            if (m_mob->IsBeingPushed() || teleported ||
                offH > kTrailLoseDistance || std::abs(off.y) > kTrailLoseDistance) {
                m_onTrail = false;
            }
        } else if (!m_mob->IsBeingPushed() && !teleported &&
                   offH <= kTrailLockHorizontal && std::abs(off.y) <= kTrailLockVertical) {
            m_onTrail = true;
        }

        if (m_onTrail) {
            // Replay. Travel does the moving and the look after the controls
            // have run, so neither the look control's level-out nor the move
            // control's turn toward its waypoint survives the tick.
            m_mob->GetNavigation().Stop();
            m_mob->MirrorThisTick(s, recordedH + kTrailGlide,
                                  std::abs(recordedStep.y) + kTrailGlide);
            if (s.swing) m_mob->Swing();
            return;
        }
        m_mob->SetMirroring(false);
        WalkOntoTrail(s);
    }

    void EchoMimicStalkGoal::WalkOntoTrail(const EchoMimic::EchoSample& s) {
        // Not on the trail yet, or knocked or held off it: walk to where the
        // target stood two seconds ago — pathing round whatever is in the way
        // while it is more than a couple of blocks off, then straight on.
        PathNavigation& nav = m_mob->GetNavigation();
        if (m_mob->DistanceToSqr(s.pos.x, s.pos.y, s.pos.z) > 2.0 * 2.0) {
            // The trail point moves, so the path is refreshed every half
            // second — sooner once the old one ran out, but never every tick
            // (a point with no path would re-search on every one).
            if (--m_repathIn <= 0 || (nav.IsDone() && m_repathIn <= 7)) {
                m_repathIn = 10;
                nav.MoveTo(s.pos.x, s.pos.y, s.pos.z, kTrailCatchUpSpeed);
            }
        } else {
            nav.Stop();
            m_repathIn = 0;
            m_mob->GetMoveControl().SetWantedPosition(s.pos.x, s.pos.y, s.pos.z,
                                                      kTrailCatchUpSpeed);
        }
        m_mob->GetLookControl().SetLookAt(s.pos.x, s.pos.y + m_mob->GetEyeHeight(), s.pos.z);
    }

    // ══ The Choir Mother ═══════════════════════════════════════════════════

    void ChoirMother::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,           static_cast<double>(kMaxHealth));
        out.Register(Attribute::MovementSpeed,       0.25);
        out.Register(Attribute::FlyingSpeed,         0.3);
        out.Register(Attribute::KnockbackResistance, 1.0);
        out.Register(Attribute::AttackDamage,        8.0);
        out.Register(Attribute::FollowRange,        40.0);
        out.Register(Attribute::Armor,               8.0);
    }

    ChoirMother::ChoirMother(EntityLevel* level) : Monster(EntityTypeId::ChoirMother, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetNoGravity(true);
        SetPersistenceRequired(true);
        RegisterGoals();
    }

    void ChoirMother::RegisterGoals() {
        // Movement and attacks are the phase machine in CustomServerAiStep;
        // the goals only pick the target and turn the head.
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 24.0f));
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackablePlayerGoal>(this, false));
    }

    std::shared_ptr<SpawnGroupData>
    ChoirMother::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        m_home = position;
        m_hasHome = true;
        return Monster::FinalizeSpawn(reason, std::move(groupData));
    }

    int ChoirMother::Phase() const {
        const float f = GetMaxHealth() > 0.0f ? GetHealth() / GetMaxHealth() : 0.0f;
        if (f > 2.0f / 3.0f) return 1;
        if (f > 1.0f / 3.0f) return 2;
        return 3;
    }

    void ChoirMother::Travel(const glm::dvec3& input) {
        (void)input;
        Move(velocity);
        velocity *= 0.9;
    }

    double ChoirMother::FloorBelow(const glm::dvec3& p) const {
        return FloorUnder(m_level ? m_level->Blocks() : nullptr, p, 24);
    }

    void ChoirMother::Hover() {
        // A slow circle of radius 6 round the altar she rose from, four
        // blocks over the floor, breathing up and down.
        m_hoverAngle += 0.01f;
        glm::dvec3 want = m_home + glm::dvec3(6.0 * std::cos(m_hoverAngle), 0.0,
                                              6.0 * std::sin(m_hoverAngle));
        want.y = std::min(FloorBelow(want + glm::dvec3(0.0, 4.0, 0.0)) + 4.0,
                          m_home.y + 6.0) + 0.5 * std::sin(tickCount * 0.05);
        glm::dvec3 d = want - position;
        const double len = glm::length(d);
        if (len > 1.0e-4) {
            const glm::dvec3 push = d / len * std::min(0.3, len * 0.05);
            velocity += (push - velocity) * 0.2;
        }
        if (LivingEntity* t = GetTarget()) {
            yRot = Mth::ApproachDegrees(yRot, YawToward(t->position.x - position.x,
                                                        t->position.z - position.z), 6.0f);
        } else {
            FaceVelocity(*this);
        }
        yBodyRot = yRot;
    }

    void ChoirMother::Conduct() {
        // Summon 2-3 echo wraiths round her, up to six alive near her.
        if (!m_level) return;
        m_conductTicks = 40;
        m_level->BroadcastEntityEvent(*this, kEventConduct);
        PlaySound("obeycraft:entity.choir_mother.conduct", 3.0f, 1.6f);

        const glm::vec3 c(static_cast<float>(position.x), static_cast<float>(position.y),
                          static_cast<float>(position.z));
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(AABB::FromMinMax(c - glm::vec3(24.0f), c + glm::vec3(24.0f)),
                                  this, nearby);
        int wraiths = 0;
        for (Entity* e : nearby) {
            if (e && !e->IsRemoved() && e->GetType() == EntityTypeId::EchoWraith) ++wraiths;
        }
        JavaRandom& rng = m_level->Random();
        const int count = std::min(2 + rng.NextInt(2), 6 - wraiths);
        for (int i = 0; i < count; ++i) {
            std::unique_ptr<Mob> wraith = MakeGenericMob(EntityTypeId::EchoWraith, m_level);
            if (!wraith) return;
            wraith->position = position + glm::dvec3(rng.NextDouble() * 6.0 - 3.0,
                                                     0.5 + rng.NextDouble() * 1.5,
                                                     rng.NextDouble() * 6.0 - 3.0);
            wraith->yRot = rng.NextFloat() * 360.0f;
            wraith->FinalizeSpawn(SpawnReason::MobSummoned, nullptr);
            if (LivingEntity* t = GetTarget()) wraith->SetTarget(t);
            m_level->AddFreshEntity(std::move(wraith));
        }
    }

    void ChoirMother::StartRing() {
        if (!m_level) return;
        Ring ring;
        ring.centre = position;
        ring.baseY = FloorBelow(position);
        ring.radius = 1.0;
        m_rings.push_back(std::move(ring));
        m_level->BroadcastEntityEvent(*this, kEventSonicRing);
        PlaySound("obeycraft:entity.choir_mother.sonic_ring", 3.0f, 0.6f);
    }

    void ChoirMother::TickRings() {
        if (!m_level || m_rings.empty()) return;
        std::vector<LivingEntity*> players;
        PlayersNear(*m_level, position, kRingMaxRadius + 8.0, players);
        for (Ring& ring : m_rings) {
            ring.radius += kRingSpeed;
            for (LivingEntity* p : players) {
                const double dx = p->position.x - ring.centre.x;
                const double dz = p->position.z - ring.centre.z;
                const double d = std::sqrt(dx * dx + dz * dz);
                if (std::abs(d - ring.radius) > kRingHalfWidth) continue;
                // Jumped over it: feet more than 0.9 above the ring's floor.
                // Far below it (a ledge under the hall) is not in its path.
                const double above = p->position.y - ring.baseY;
                if (above > 0.9 || above < -2.5) continue;
                if (std::find(ring.hit.begin(), ring.hit.end(), p->GetId()) != ring.hit.end()) {
                    continue;
                }
                ring.hit.push_back(p->GetId());
                p->Hurt(MobDamageSource::Magic, 6.0f, this);
                // Pushed outward, away from her.
                const glm::dvec3 out = d > 1.0e-4 ? glm::dvec3(dx / d, 0.0, dz / d)
                                                  : glm::dvec3(1.0, 0.0, 0.0);
                p->velocity += glm::dvec3(out.x * 1.2, 0.45, out.z * 1.2);
                p->hurtMarked = true;
            }
        }
        m_rings.erase(std::remove_if(m_rings.begin(), m_rings.end(),
                                     [](const Ring& r) { return r.radius > kRingMaxRadius; }),
                      m_rings.end());
    }

    void ChoirMother::SilenceField() {
        // Every player within 12 blocks: Darkness and Slowness II for 4 s,
        // refreshed every second while they stay in it.
        if (!m_level) return;
        std::vector<LivingEntity*> players;
        PlayersNear(*m_level, position, kSilenceRadius, players);
        for (LivingEntity* p : players) {
            p->AddEffect(MobEffectInstance(MobEffectId::Darkness, 80, 0), this);
            p->AddEffect(MobEffectInstance(MobEffectId::Slowness, 80, 1), this);
        }
    }

    void ChoirMother::CustomServerAiStep() {
        if (!m_level) return;
        if (!m_hasHome) {
            m_home = position;
            m_hasHome = true;
        }
        m_syncedPhase = Phase();
        Hover();
        if (m_conductTicks > 0) --m_conductTicks;
        TickRings();

        // Idle without a player to sing at.
        if (!GetTarget()) return;
        const int phase = m_syncedPhase;

        if (--m_conductIn <= 0) {
            Conduct();
            m_conductIn = phase == 1 ? 200 : 320;
        }
        if (phase >= 2 && --m_ringIn <= 0) {
            StartRing();
            m_ringIn = phase == 2 ? 120 : 90;
        }
        if (phase == 3 && --m_silenceIn <= 0) {
            SilenceField();
            m_silenceIn = 20;
        }
    }

    void ChoirMother::HandleEntityEvent(uint8_t id) {
        if (id == kEventSonicRing) {
            m_clientRings.push_back(ClientRing{ position, FloorBelow(position), 0 });
        } else if (id == kEventConduct) {
            m_conductTicks = 40;
            if (m_level) {
                JavaRandom& rng = m_level->Random();
                for (int i = 0; i < 16; ++i) {
                    m_level->AddParticle(ParticleKind::HushMote,
                                         position.x + (rng.NextDouble() - 0.5) * 3.0,
                                         position.y + 1.5 + rng.NextDouble() * 2.0,
                                         position.z + (rng.NextDouble() - 0.5) * 3.0,
                                         0.0, 0.05, 0.0);
                }
            }
        } else {
            Monster::HandleEntityEvent(id);
        }
    }

    void ChoirMother::Tick() {
        Monster::Tick();
        if (!m_level || !m_level->IsClientSide()) return;
        if (m_conductTicks > 0) --m_conductTicks;
        JavaRandom& rng = m_level->Random();

        // The sonic rings: a circle of motes on the floor, expanding at the
        // server's rate — jump when it reaches you.
        for (ClientRing& ring : m_clientRings) {
            ++ring.age;
            const double r = 1.0 + ring.age * kRingSpeed;
            const int n = std::min(48, static_cast<int>(r * kTwoPi * 1.2));
            for (int i = 0; i < n; ++i) {
                const double a = (i + rng.NextDouble() * 0.5) * kTwoPi / n;
                m_level->AddParticle(ParticleKind::HushMote,
                                     ring.centre.x + r * std::cos(a), ring.baseY + 0.15,
                                     ring.centre.z + r * std::sin(a), 0.0, 0.02, 0.0);
            }
        }
        m_clientRings.erase(std::remove_if(m_clientRings.begin(), m_clientRings.end(),
                                           [](const ClientRing& r) {
                                               return 1.0 + r.age * kRingSpeed > kRingMaxRadius;
                                           }),
                            m_clientRings.end());

        // The silence field (phase 3): a dark haze at its 12-block edge.
        if (m_syncedPhase == 3) {
            for (int i = 0; i < 4; ++i) {
                const double a = rng.NextDouble() * kTwoPi;
                const double r = kSilenceRadius * (0.85 + rng.NextDouble() * 0.15);
                m_level->AddParticle(ParticleKind::LargeSmoke,
                                     position.x + r * std::cos(a),
                                     FloorBelow(position) + rng.NextDouble() * 2.0,
                                     position.z + r * std::sin(a), 0.0, 0.01, 0.0);
            }
        }
        // Motes dripping from her tendrils.
        if (rng.NextInt(3) == 0) {
            m_level->AddParticle(ParticleKind::HushMote,
                                 position.x + (rng.NextDouble() - 0.5) * 1.2,
                                 position.y + rng.NextDouble() * 0.8,
                                 position.z + (rng.NextDouble() - 0.5) * 1.2,
                                 0.0, -0.03, 0.0);
        }
    }

    void ChoirMother::DropCustomDeathLoot(EntityLevel& level) {
        // 2-4 resonant crystal (a block item — see CrystalGolem's note);
        // the heart and the shards are the loot table's.
        const int n = 2 + level.Random().NextInt(3);
        level.SpawnItemDrop(position, ItemRegistry::FromBlock(BlockID::ResonantCrystal), n);
    }

    void ChoirMother::SaveModNbt(ModNbtOut& out) const {
        out.Bool("HasHome", m_hasHome);
        if (m_hasHome) {
            out.Float("HomeX", static_cast<float>(m_home.x));
            out.Float("HomeY", static_cast<float>(m_home.y));
            out.Float("HomeZ", static_cast<float>(m_home.z));
        }
    }

    void ChoirMother::LoadModNbt(const ModNbtIn& in) {
        m_hasHome = in.Bool("HasHome", false);
        if (m_hasHome) {
            m_home = glm::dvec3(in.Float("HomeX", 0.0f), in.Float("HomeY", 0.0f),
                                in.Float("HomeZ", 0.0f));
        }
    }

    // ══ Factory ════════════════════════════════════════════════════════════

    std::unique_ptr<Mob> MakeHushCreature(EntityTypeId type, EntityLevel* level) {
        switch (type) {
            case EntityTypeId::LumenMoth:     return std::make_unique<LumenMoth>(level);
            case EntityTypeId::CrystalGolem:  return std::make_unique<CrystalGolem>(level);
            case EntityTypeId::HushLeviathan: return std::make_unique<HushLeviathan>(level);
            case EntityTypeId::EchoMimic:     return std::make_unique<EchoMimic>(level);
            case EntityTypeId::ChoirMother:   return std::make_unique<ChoirMother>(level);
            // Aurelith's boss (TheUnsung.hpp) rides the same factory.
            case EntityTypeId::TheUnsung:     return std::make_unique<TheUnsung>(level);
            default:                          return nullptr;
        }
    }

} // namespace Game
