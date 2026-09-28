// File: src/common/world/block/entity/BellBlockEntity.cpp
#include "BellBlockEntity.hpp"

#include "common/core/Mth.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/physics/Physics.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/LevelSound.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/tags/DataTags.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        constexpr int kEventRing = 1;

        // BlockPos.closerToCenterThan(pos, distance).
        bool CloserToCenterThan(const glm::ivec3& block, const glm::dvec3& pos, double distance) {
            const glm::dvec3 d = pos - (glm::dvec3(block) + glm::dvec3(0.5));
            return glm::dot(d, d) < distance * distance;
        }

        bool IsRaider(const Entity& entity) {
            return DataTags::HasTag(DataTags::Registry::EntityType, entity.TypeInfo().slug, "minecraft:raiders");
        }
    } // namespace

    void BellBlockEntity::OnHit(ILevelWrite& level, int face) {
        m_clickDirection = face;
        if (m_shaking) m_ticks = 0;
        else m_shaking = true;
        level.BlockEvent(GetWorldPos(), GetBlockId(), kEventRing, face);
    }

    bool BellBlockEntity::TriggerEvent(int b0, int b1) {
        if (b0 != kEventRing) return BlockEntity::TriggerEvent(b0, b1);
        if (ILevelWrite* level = GetLevel()) UpdateEntities(*level);
        m_resonationTicks = 0;
        m_clickDirection = (b1 >= 0 && b1 < 6) ? b1 : 2;
        m_ticks = 0;
        m_shaking = true;
        return true;
    }

    void BellBlockEntity::CarryClientState(const BlockEntity& previous) {
        if (const auto* bell = dynamic_cast<const BellBlockEntity*>(&previous)) {
            m_ticks = bell->m_ticks;
            m_shaking = bell->m_shaking;
            m_clickDirection = bell->m_clickDirection;
            m_resonating = bell->m_resonating;
            m_resonationTicks = bell->m_resonationTicks;
            m_nearbyEntities = bell->m_nearbyEntities;
            m_haveNearby = bell->m_haveNearby;
            m_lastRingTimestamp = bell->m_lastRingTimestamp;
        }
    }

    void BellBlockEntity::UpdateEntities(ILevelWrite& level) {
        EntityLevel* entities = level.Entities();
        const glm::ivec3 pos = GetWorldPos();
        const int64_t gameTime = level.GameTime();
        if (entities && (gameTime > m_lastRingTimestamp + kMinTicksBetweenSearches || !m_haveNearby)) {
            m_lastRingTimestamp = gameTime;
            // new AABB(pos).inflate(48): the cell grown 48 each way.
            const AABB box = AABB::FromMinMax(glm::vec3(pos) - glm::vec3(static_cast<float>(kSearchRadius)),
                                              glm::vec3(pos) + glm::vec3(1.0f + static_cast<float>(kSearchRadius)));
            std::vector<Entity*> found;
            entities->GetEntitiesInBox(box, nullptr, found);
            m_nearbyEntities.clear();
            for (Entity* e : found) {
                if (e && e->AsLiving()) m_nearbyEntities.push_back(e->GetId());
            }
            m_haveNearby = true;
        }
        if (level.IsClientSide() || !entities) return;
        for (int32_t id : m_nearbyEntities) {
            Entity* e = entities->ResolveEntityById(id);
            LivingEntity* living = e ? e->AsLiving() : nullptr;
            if (!living || !living->IsAlive() || living->IsRemoved()) continue;
            if (!CloserToCenterThan(pos, living->position, static_cast<double>(kHearBellRadius))) continue;
            Brain* brain = living->GetBrain();
            // setMemory on a brain without the memory is a no-op in MC.
            if (brain && brain->IsRegistered(MemoryModule::HeardBellTime)) {
                brain->SetMemory(MemoryModule::HeardBellTime, level.GameTime());
            }
        }
    }

    bool BellBlockEntity::AreRaidersNearby(ILevelWrite& level) const {
        EntityLevel* entities = level.Entities();
        if (!entities) return false;
        for (int32_t id : m_nearbyEntities) {
            Entity* e = entities->ResolveEntityById(id);
            if (!e || !e->IsAlive() || e->IsRemoved() || !e->AsLiving()) continue;
            if (!CloserToCenterThan(GetWorldPos(), e->position, static_cast<double>(kHearBellRadius))) continue;
            if (IsRaider(*e)) return true;
        }
        return false;
    }

    void BellBlockEntity::MakeRaidersGlow(ILevelWrite& level) const {
        EntityLevel* entities = level.Entities();
        if (!entities) return;
        for (int32_t id : m_nearbyEntities) {
            Entity* e = entities->ResolveEntityById(id);
            LivingEntity* living = e ? e->AsLiving() : nullptr;
            if (!living || !living->IsAlive() || living->IsRemoved()) continue;
            if (!CloserToCenterThan(GetWorldPos(), living->position, static_cast<double>(kHighlightRaidersRadius))) continue;
            if (!IsRaider(*living)) continue;
            living->AddEffect(MobEffectInstance(MobEffectId::Glowing, kGlowDuration));
        }
    }

    void BellBlockEntity::ShowBellParticles(ILevelWrite& level) const {
        EntityLevel* entities = level.Entities();
        if (!entities) return;
        const glm::ivec3 bell = GetWorldPos();
        int particleColor = 16700985;
        int nearbyRaiderCount = 0;
        for (int32_t id : m_nearbyEntities) {
            Entity* e = entities->ResolveEntityById(id);
            if (e && CloserToCenterThan(bell, e->position, 48.0)) ++nearbyRaiderCount;
        }
        for (int32_t id : m_nearbyEntities) {
            Entity* e = entities->ResolveEntityById(id);
            if (!e || !e->IsAlive() || e->IsRemoved() || !e->AsLiving()) continue;
            if (!CloserToCenterThan(bell, e->position, 48.0) || !IsRaider(*e)) continue;
            const double dx = e->position.x - static_cast<double>(bell.x);
            const double dz = e->position.z - static_cast<double>(bell.z);
            const double distBtwn = std::sqrt(dx * dx + dz * dz);
            const double x3 = static_cast<double>(static_cast<float>(bell.x) + 0.5f) + 1.0 / distBtwn * dx;
            const double z3 = static_cast<double>(static_cast<float>(bell.z) + 0.5f) + 1.0 / distBtwn * dz;
            const int particleCount = std::clamp((nearbyRaiderCount - 21) / -2, 3, 15);
            for (int i = 0; i < particleCount; ++i) {
                particleColor += 5;
                // ColorParticleOption.create(ENTITY_EFFECT, color): an RGB
                // int, alpha 255.
                level.AddParticle(ParticleOptions::Color(ParticleKind::EntityEffect,
                                                         0xFF000000u | static_cast<uint32_t>(particleColor & 0xFFFFFF)),
                                  x3, static_cast<double>(static_cast<float>(bell.y) + 0.5f), z3, 0.0, 0.0, 0.0);
            }
        }
    }

    void BellBlockEntity::TickCommon(ILevelWrite& level, bool server) {
        if (m_shaking) ++m_ticks;
        if (m_ticks >= kDuration) {
            m_shaking = false;
            m_ticks = 0;
        }
        if (m_ticks >= kTicksBeforeResonation && m_resonationTicks == 0 && AreRaidersNearby(level)) {
            m_resonating = true;
            // level.playSound(null, ...): the server's broadcast reaches every
            // client; a client level plays nothing for a null player.
            if (server) {
                level.PlaySound(nullptr, GetWorldPos(), SoundEvents::BELL_RESONATE, SoundSource::Blocks, 1.0f, 1.0f);
            }
        }
        if (m_resonating) {
            if (m_resonationTicks < kMaxResonationTicks) {
                ++m_resonationTicks;
            } else {
                if (server) MakeRaidersGlow(level);
                else ShowBellParticles(level);
                m_resonating = false;
            }
        }
    }

    void BellBlockEntity::Tick(World* world, float /*deltaTime*/) {
        if (!world) return;
        if (!GetLevel()) SetLevel(world);
        TickCommon(*world, /*server=*/true);
    }

    void BellBlockEntity::ClientTick(ILevelWrite& level) {
        if (!GetLevel()) SetLevel(&level);
        TickCommon(level, /*server=*/false);
    }

} // namespace Game
