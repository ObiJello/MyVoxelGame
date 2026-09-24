// File: src/common/entity/projectile/ResonanceArrow.cpp
//
// See ResonanceArrow.hpp.
#include "common/entity/projectile/ResonanceArrow.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/level/HushItems.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    bool ResonanceArrow::CanHitEntity(const Entity& entity) const {
        if (std::find(m_pierced.begin(), m_pierced.end(), entity.GetId()) != m_pierced.end()) {
            return false;
        }
        return Arrow::CanHitEntity(entity);
    }

    void ResonanceArrow::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        const glm::dvec3 at = hit.location;
        if (m_pierceLeft > 0) {
            // MC AbstractArrow.onHitEntity with pierceLevel > 0: the hit
            // lands (the arrow's own damage, the owner credited), the id goes
            // on the ignore list, and the arrow keeps its velocity — it is
            // NOT discarded and NOT deflected.
            const double speed = glm::length(velocity);
            const int damage = static_cast<int>(std::ceil(
                std::clamp(speed * m_baseDamage, 0.0, 2.147483647e9)));
            Entity* owner = GetOwner();
            if (auto* livingOwner = dynamic_cast<LivingEntity*>(owner)) {
                livingOwner->SetLastHurtMob(&target);
            }
            DealHitDamage(target, hit, MobDamageSource::Projectile,
                          static_cast<float>(damage), owner ? owner : this);
            m_pierced.push_back(target.GetId());
            --m_pierceLeft;
            Burst(at);
            return;
        }
        // Pierce spent: the plain arrow's hit (damage, discard or deflect).
        Arrow::OnHitEntity(target, hit);
        Burst(at);
    }

    void ResonanceArrow::OnHitBlockArrow(const glm::dvec3& hitPos, const glm::ivec3& blockPos) {
        Arrow::OnHitBlockArrow(hitPos, blockPos);
        Burst(hitPos);
    }

    void ResonanceArrow::Burst(const glm::dvec3& at) {
        if (!m_level || m_level->IsClientSide()) return;
        Entity* owner = GetOwner();
        const double r = static_cast<double>(HushItems::kBurstRadius);
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(AABB::FromMinMax(glm::vec3(at - glm::dvec3(r)),
                                                   glm::vec3(at + glm::dvec3(r))),
                                  this, nearby);
        for (Entity* e : nearby) {
            if (!e || e == owner || e->IsRemoved()) continue;
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            const glm::dvec3 d = living->position - at;
            if (glm::dot(d, d) > r * r) continue;
            // Magic: armour does not blunt a sound, and it is in MC's
            // no_knockback tag, so the only shove is the burst's own, pointed
            // from the impact (Knockback's dx/dz run from the pusher toward
            // the victim's origin: impact minus victim).
            living->HurtFrom(MobDamageSource::Magic, HushItems::kBurstDamage,
                             owner ? owner : this, this);
            living->Knockback(0.6, at.x - living->position.x, at.z - living->position.z);
        }
        m_level->PlaySound(nullptr, at, "obeycraft:entity.resonance_arrow.burst", GetSoundSource(), 0.6f, 1.6f);
        HushItems::BroadcastSonicBurst(m_level->Dimension(), at, HushItems::kBurstRadius);
    }

} // namespace Game
