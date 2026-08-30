// File: src/common/entity/projectile/ThrowableProjectile.cpp
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/core/JavaRandom.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    ThrowableProjectile::ThrowableProjectile(EntityTypeId type, EntityLevel* level)
        : Projectile(type, level) {}

    void ThrowableProjectile::SetOwnerAndPosition(LivingEntity& owner) {
        SetOwner(&owner);
        position = glm::dvec3(owner.position.x, owner.GetEyeY() - 0.1,
                              owner.position.z);
    }

    void ThrowableProjectile::Tick() {
        if (!m_level || !m_level->Blocks()) return;
        const bool serverSide = !m_level->IsClientSide();

        // MC order: applyGravity, applyInertia, THEN clip along the result.
        velocity.y -= GetDefaultGravity();
        velocity *= static_cast<double>(IsInWater() ? 0.8f : 0.99f);

        const glm::dvec3 origin = position;
        const HitResult hit = Clip(origin, velocity, serverSide);

        position = hit.IsHit() ? hit.location : origin + velocity;
        RotateTowardsMovement(0.2f);

        Entity::BaseTick();

        if (hit.IsHit() && IsAlive()) OnHit(hit);
    }

    // ── Snowball ───────────────────────────────────────────────────────────

    void Snowball::OnHitEntity(LivingEntity& target) {
        // MC Snowball.onHitEntity: 3 vs blazes, 0 otherwise — the zero still
        // counts as a hit (hurt flash + knockback), which is MC's behaviour.
        const float damage = target.GetType() == EntityTypeId::Blaze ? 3.0f : 0.0f;
        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }
        target.Hurt(MobDamageSource::Projectile, damage, GetOwner() ? GetOwner() : this);
    }

    void Snowball::OnHit(const HitResult& hit) {
        ThrowableProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) {
            // MC broadcasts entity event 3 for the poof particles; the client
            // has no particle system yet, so the event is a no-op there.
            m_level->BroadcastEntityEvent(*this, 3);
            Discard();
        }
    }

    // ── ThrownEgg ──────────────────────────────────────────────────────────

    void ThrownEgg::OnHitEntity(LivingEntity& target) {
        target.Hurt(MobDamageSource::Projectile, 0.0f, GetOwner() ? GetOwner() : this);
    }

    void ThrownEgg::OnHit(const HitResult& hit) {
        ThrowableProjectile::OnHit(hit);
        if (!m_level || m_level->IsClientSide()) return;

        JavaRandom& rng = m_level->Random();
        if (rng.NextInt(8) == 0) {
            int count = 1;
            if (rng.NextInt(32) == 0) count = 4;

            for (int i = 0; i < count; ++i) {
                auto chicken = std::make_unique<Chicken>(m_level);
                chicken->SetAge(AgeableMob::kBabyStartAge);
                chicken->position = position;
                chicken->yRot = yRot;
                m_level->AddFreshEntity(std::move(chicken));
            }
        }

        m_level->BroadcastEntityEvent(*this, 3);
        Discard();
    }

    // ── ThrownSplashPotion ─────────────────────────────────────────────────

    void ThrownSplashPotion::OnHit(const HitResult& hit) {
        ThrowableProjectile::OnHit(hit);
        if (!m_level || m_level->IsClientSide()) return;

        // MC AbstractThrownPotion.onHit → ThrownSplashPotion.onHitAsPotion:
        // everything within the box inflated by (4, 2, 4) and inside 4 blocks
        // takes scale = 1 - dist/4 of each effect. (MC measures dist between
        // AABBs; centre-to-centre is the same simplification Creeper::Explode
        // uses. MC also re-centres the box on the exact hit location; the tick
        // granularity difference is sub-block.)
        AABB box = GetAABB();
        box.min -= glm::vec3(4.0f, 2.0f, 4.0f);
        box.max += glm::vec3(4.0f, 2.0f, 4.0f);

        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(box, this, nearby);
        for (Entity* e : nearby) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;

            const double distSq = DistanceToSqr(*living);
            if (distSq >= 16.0) continue;

            const double scale = 1.0 - std::sqrt(distSq) / 4.0;
            for (const MobEffectInstance& effect : m_effects) {
                if (IsInstantenousEffect(effect.effect)) {
                    // Harming's damage is indirect MAGIC — bypasses armor,
                    // attributed to the thrower so retaliation targets the
                    // witch (ApplyInstantenousEffect handles both).
                    ApplyInstantenousEffect(this, GetOwner(), *living,
                                            effect.effect, effect.amplifier, scale);
                } else {
                    MobEffectInstance scaled(effect.effect,
                                             effect.MapScaledDuration(scale),
                                             effect.amplifier,
                                             effect.ambient, effect.visible);
                    // MC drops a scaled effect that would end within a second.
                    if (!scaled.EndsWithin(20)) {
                        living->AddEffect(std::move(scaled),
                                          GetOwner() ? GetOwner() : this);
                    }
                }
            }
        }

        m_level->BroadcastEntityEvent(*this, 3);
        Discard();
    }

} // namespace Game
