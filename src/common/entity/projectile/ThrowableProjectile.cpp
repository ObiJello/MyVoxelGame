// File: src/common/entity/projectile/ThrowableProjectile.cpp
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
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

    // ── ThrownEnderpearl ───────────────────────────────────────────────────

    void ThrownEnderpearl::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // MC onHitEntity: hurt for ZERO — the hit registers (flash, kill
        // credit chain) but deals nothing; the 5.0 lands on the THROWER.
        DealHitDamage(target, hit, MobDamageSource::Projectile, 0.0f, GetOwner());
    }

    void ThrownEnderpearl::OnHit(const HitResult& hit) {
        // MC ThrownEnderpearl.onHit, transcribed. (The 32 PORTAL particles
        // are a client visual with no ParticleKind here — skipped like the
        // eye of ender's shatter burst.)
        Projectile::OnHit(hit);
        if (!m_level) return;
        if (m_level->IsClientSide()) {
            // The client copy waits for the server's removal, like the TNT.
            return;
        }
        if (IsRemoved()) return;

        Entity* owner = GetOwner();
        auto* livingOwner = dynamic_cast<LivingEntity*>(owner);
        // MC isAllowedToTeleportOwner: alive (and not sleeping — no sleep
        // system); the cross-dimension branch cannot arise, projectiles do
        // not travel dimensions here.
        const bool mayTeleport =
            owner != nullptr && (livingOwner ? livingOwner->IsAlive()
                                             : owner->IsAlive());
        if (mayTeleport) {
            // MC teleports to oldPosition() — the pearl's pre-impact spot,
            // which is what keeps the thrower out of the wall it hit.
            const glm::dvec3 teleportPos = oldPosition;

            if (owner->IsPlayer()) {
                // MC: 5% endermite at the position the player LEAVES, gated
                // on doMobSpawning.
                if (m_level->Random().NextFloat() < 0.05f &&
                    m_level->DoMobSpawning()) {
                    auto endermite =
                        MakeGenericMob(EntityTypeId::Endermite, m_level);
                    if (endermite) {
                        endermite->position = owner->position;
                        endermite->yRot = endermite->yBodyRot =
                            endermite->yHeadRot = owner->yRot;
                        endermite->FinalizeSpawn(SpawnReason::Triggered, nullptr);
                        m_level->AddFreshEntity(std::move(endermite));
                    }
                }

                // Player movement is client-authoritative, so the move is a
                // packet — EntityLevel::TeleportPlayer, the server bridge's
                // seam. MC: resetFallDistance + 5.0 ender_pearl damage after
                // the teleport (the closest source here is FALL — feather
                // falling reduces both in vanilla).
                if (livingOwner && m_level->TeleportPlayer(*livingOwner, teleportPos)) {
                    livingOwner->Hurt(MobDamageSource::Fall, 5.0f, nullptr);
                }
            } else {
                // MC's non-player branch: move the entity, reset its fall.
                owner->position = teleportPos;
                owner->fallDistance = 0.0f;
                owner->needsSync = true;
            }
            // MC playSound PLAYER_TELEPORT — sound system stub.
        }

        Discard();
    }

    // ── Snowball ───────────────────────────────────────────────────────────

    void Snowball::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // MC Snowball.onHitEntity: 3 vs blazes, 0 otherwise — the zero still
        // counts as a hit (hurt flash + knockback), which is MC's behaviour.
        const float damage = target.GetType() == EntityTypeId::Blaze ? 3.0f : 0.0f;
        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }
        DealHitDamage(target, hit, MobDamageSource::Projectile, damage,
                      GetOwner() ? GetOwner() : this);
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

    void ThrownEgg::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        DealHitDamage(target, hit, MobDamageSource::Projectile, 0.0f,
                      GetOwner() ? GetOwner() : this);
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
