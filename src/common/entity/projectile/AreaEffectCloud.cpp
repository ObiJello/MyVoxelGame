// File: src/common/entity/projectile/AreaEffectCloud.cpp
#include "common/entity/projectile/AreaEffectCloud.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/Mth.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    AreaEffectCloud::AreaEffectCloud(EntityLevel* level)
        : Projectile(EntityTypeId::AreaEffectCloud, level) {
        // MC's constructor: noPhysics = true. The cloud never calls Move(), so
        // physics never applies to begin with — the flag has no field here.
        SetNoGravity(true);
    }

    void AreaEffectCloud::SetRadius(float radius) {
        // MC setRadius: Mth.clamp(radius, 0.0F, 32.0F) into DATA_RADIUS (no
        // client sync — nothing renders the cloud; see the header).
        m_radius = Mth::Clamp(radius, 0.0f, kMaxRadius);
    }

    void AreaEffectCloud::Tick() {
        // MC AreaEffectCloud.tick: super.tick() (baseTick), then the server
        // half. clientTick() is the particle field — no particle system, so
        // the client copy only ages.
        BaseTick();
        if (m_level && !m_level->IsClientSide()) {
            ServerTick();
        }
    }

    void AreaEffectCloud::ServerTick() {
        // MC serverTick, transcribed.
        if (m_duration != -1 && tickCount - m_waitTime >= m_duration) {
            Discard();
            return;
        }

        const bool shouldWait = tickCount < m_waitTime;
        // MC flips DATA_WAITING here (the small-particle mode) — render-only,
        // nothing to flip without particles.
        if (shouldWait) return;

        float radius = m_radius;
        if (m_radiusPerTick != 0.0f) {
            radius += m_radiusPerTick;
            if (radius < kMinimalRadius) {
                Discard();
                return;
            }
            SetRadius(radius);
            radius = m_radius;
        }

        if (tickCount % kTimeBetweenApplications != 0) return;

        // MC: victims.entrySet().removeIf(entry -> tickCount >= value).
        for (auto it = m_victims.begin(); it != m_victims.end();) {
            if (tickCount >= it->second) it = m_victims.erase(it);
            else ++it;
        }

        if (m_effects.empty()) {
            m_victims.clear();
            return;
        }

        // MC PotionContents.forEachEffect(allEffects::add, potionDurationScale):
        // non-instant durations are scaled by potionDurationScale
        // (withScaledDuration — floor, MC's mapDuration lambda).
        std::vector<MobEffectInstance> allEffects;
        allEffects.reserve(m_effects.size());
        for (const MobEffectInstance& e : m_effects) {
            MobEffectInstance scaled = e;
            if (m_potionDurationScale != 1.0f && !scaled.IsInfiniteDuration()) {
                scaled.duration = static_cast<int>(
                    static_cast<float>(scaled.duration) * m_potionDurationScale);
            }
            allEffects.push_back(scaled);
        }

        // MC getEntitiesOfClass(LivingEntity.class, getBoundingBox()).
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(GetAABB(), this, nearby);
        // Players are not in GetEntitiesInBox — mirror the wither's pattern.
        std::vector<LivingEntity*> players;
        m_level->GetPlayers(players);
        const AABB box = GetAABB();
        for (LivingEntity* player : players) {
            if (player && player->GetAABB().Intersects(box)) {
                nearby.push_back(player);
            }
        }

        for (Entity* e : nearby) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            if (m_victims.count(living)) continue;
            // MC entity.isAffectedByPotions() && !noneMatch(canBeAffected):
            // at least one stored effect must be able to land.
            bool anyApplies = false;
            for (const MobEffectInstance& fx : allEffects) {
                if (living->CanBeAffected(fx)) { anyApplies = true; break; }
            }
            if (!anyApplies) continue;

            // MC measures HORIZONTAL distance from the cloud centre.
            const double xd = living->position.x - position.x;
            const double zd = living->position.z - position.z;
            if (xd * xd + zd * zd > static_cast<double>(radius) * radius) continue;

            m_victims[living] = tickCount + m_reapplicationDelay;

            for (const MobEffectInstance& fx : allEffects) {
                if (IsInstantenousEffect(fx.effect)) {
                    // MC: applyInstantenousEffect(level, this, owner, entity,
                    // amplifier, 0.5).
                    ApplyInstantenousEffect(this, GetOwner(), *living,
                                            fx.effect, fx.amplifier, 0.5);
                } else {
                    living->AddEffect(MobEffectInstance(fx), this);
                }
            }

            if (m_radiusOnUse != 0.0f) {
                radius += m_radiusOnUse;
                if (radius < kMinimalRadius) {
                    Discard();
                    return;
                }
                SetRadius(radius);
                radius = m_radius;
            }

            if (m_durationOnUse != 0 && m_duration != -1) {
                m_duration += m_durationOnUse;
                if (m_duration <= 0) {
                    Discard();
                    return;
                }
            }
        }
    }

} // namespace Game
