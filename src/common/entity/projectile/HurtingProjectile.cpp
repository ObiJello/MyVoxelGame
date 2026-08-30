// File: src/common/entity/projectile/HurtingProjectile.cpp
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/entity/projectile/AreaEffectCloud.hpp"
#include "common/entity/EntityLevel.hpp"

#include <cmath>
#include <memory>
#include <vector>

namespace Game {

    HurtingProjectile::HurtingProjectile(EntityTypeId type, EntityLevel* level)
        : Projectile(type, level) {}

    void HurtingProjectile::SetOwnerAndDirection(LivingEntity& owner,
                                                 const glm::dvec3& direction) {
        SetOwner(&owner);
        position = owner.position;
        yRot = yRotO = owner.yRot;
        xRot = xRotO = owner.xRot;
        yHeadRot = yBodyRot = yRot;

        // MC assignDirectionalMovement: normalize * accelerationPower.
        const double len = glm::length(direction);
        if (len > 1.0e-9) {
            velocity = direction / len * m_accelerationPower;
            needsSync = true;
        }
    }

    void HurtingProjectile::Tick() {
        if (!m_level || !m_level->Blocks()) return;
        const bool serverSide = !m_level->IsClientSide();

        // MC: a fireball whose OWNER is gone discards itself (the chunk-loaded
        // test rides on Blocks() existing at all in this engine).
        if (serverSide && GetOwner() && GetOwner()->IsRemoved()) {
            Discard();
            return;
        }

        // MC applyInertia: v = (v + normalize(v) * accelerationPower) * inertia.
        {
            const double len = glm::length(velocity);
            glm::dvec3 v = velocity;
            if (len > 1.0e-9 && m_accelerationPower != 0.0) {
                v += velocity / len * m_accelerationPower;
            }
            const float inertia = IsInWater() ? GetLiquidInertia() : GetInertia();
            velocity = v * static_cast<double>(inertia);
        }

        const glm::dvec3 origin = position;
        const HitResult hit = Clip(origin, velocity, serverSide);

        RotateTowardsMovement(0.2f);
        position = hit.IsHit() ? hit.location : origin + velocity;

        Entity::BaseTick();

        // MC: shouldBurn keeps the projectile permanently alight (1s renewed
        // every tick), which is what the on-fire wire flag shows the client.
        if (ShouldBurn()) IgniteForSeconds(1);

        if (hit.IsHit() && IsAlive()) OnHit(hit);
    }

    // ── SmallFireball ──────────────────────────────────────────────────────

    void SmallFireball::OnHitEntity(LivingEntity& target) {
        if (!m_level || m_level->IsClientSide()) return;

        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }

        // MC: ignite FIRST, then hurt — and restore the previous fire ticks
        // when the hit did not land (fire-immune target, invulnerability).
        const int previousFireTicks = target.GetRemainingFireTicks();
        target.IgniteForSeconds(5);
        if (!target.Hurt(MobDamageSource::Projectile, 5.0f,
                         GetOwner() ? GetOwner() : this)) {
            target.SetRemainingFireTicks(previousFireTicks);
        }
    }

    void SmallFireball::OnHitBlock(const HitResult& hit) {
        (void)hit;
        // MC places a fire block on the face it struck (gated on mobGriefing).
        // No fire block exists in this engine yet; when one lands, this is the
        // single place to set it.
    }

    void SmallFireball::OnHit(const HitResult& hit) {
        HurtingProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) Discard();
    }

    // ── LargeFireball ──────────────────────────────────────────────────────

    void LargeFireball::OnHitEntity(LivingEntity& target) {
        if (!m_level || m_level->IsClientSide()) return;
        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }
        target.Hurt(MobDamageSource::Projectile, 6.0f, GetOwner() ? GetOwner() : this);
    }

    void LargeFireball::OnHit(const HitResult& hit) {
        HurtingProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) {
            // MC LargeFireball.onHit: level.explode(this, x, y, z, power,
            // isFireGamerule, ExplosionInteraction.MOB).
            //
            // Now a REAL explosion rather than the old damage-only stand-in.
            // The difference players will notice is not the crater: it is that
            // the shared path runs the exposure raycast, so a fireball no
            // longer deals full damage through a wall.
            ExplosionParams params;
            params.center       = position;
            params.radius       = static_cast<float>(m_explosionPower);
            params.source       = this;
            params.attributedTo = GetOwner();
            params.interaction  = ExplosionInteraction::Mob;
            // MC gates ghast fire on the mobGriefing rule; Explode's MOB
            // mapping already answers that for blocks, and fire follows it.
            params.fire         = m_level->MobGriefing();
            Explode(*m_level, params);
            Discard();
        }
    }

    // ── WitherSkull ────────────────────────────────────────────────────────

    void WitherSkull::OnHitEntity(LivingEntity& target) {
        if (!m_level || m_level->IsClientSide()) return;

        bool wasHurt;
        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
            wasHurt = target.Hurt(MobDamageSource::Projectile, 8.0f, livingOwner);
            if (wasHurt && !target.IsAlive()) {
                // MC: a kill heals the wither 5.
                livingOwner->Heal(5.0f);
            }
        } else {
            wasHurt = target.Hurt(MobDamageSource::Generic, 5.0f, this);
        }

        // MC WitherSkull.onHitEntity: a landed hit applies WITHER II
        // (amplifier 1 in the source, regardless of difficulty) for 10 s on
        // NORMAL / 40 s on HARD, nothing on EASY. Attribution goes to the
        // effect source — the owner when there is one.
        if (wasHurt && m_level) {
            int witherSeconds = 0;
            if (m_level->GetDifficulty() == Difficulty::Normal) witherSeconds = 10;
            else if (m_level->GetDifficulty() == Difficulty::Hard) witherSeconds = 40;
            if (witherSeconds > 0) {
                target.AddEffect(
                    MobEffectInstance(MobEffectId::Wither, 20 * witherSeconds, 1),
                    GetOwner() ? GetOwner() : this);
            }
        }
    }

    void WitherSkull::OnHit(const HitResult& hit) {
        HurtingProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) {
            // MC WitherSkull.onHit: level.explode(this, x, y, z, 1.0F, false,
            // ExplosionInteraction.MOB). Radius 1 is under MC's
            // LARGE_EXPLOSION_RADIUS, so Explode picks the small visual for us.
            ExplosionParams params;
            params.center       = position;
            params.radius       = 1.0f;
            params.source       = this;
            params.attributedTo = GetOwner();
            params.interaction  = ExplosionInteraction::Mob;
            Explode(*m_level, params);
            Discard();
        }
    }

    // ── DragonFireball ─────────────────────────────────────────────────────

    void DragonFireball::OnHit(const HitResult& hit) {
        // MC DragonFireball.onHit: the ownedBy guard skips detonating on the
        // dragon itself; a direct hit on any other entity or a block leaves
        // the harming cloud.
        if (hit.IsEntity() && hit.entity == GetOwner()) return;
        HurtingProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) {
            // MC: getEntitiesOfClass(LivingEntity, bb.inflate(4, 2, 4)) — the
            // cloud recentres on the first living entity within 4 blocks.
            AABB searchBox = GetAABB();
            searchBox.min -= glm::vec3(4.0f, 2.0f, 4.0f);
            searchBox.max += glm::vec3(4.0f, 2.0f, 4.0f);
            std::vector<Entity*> nearby;
            m_level->GetEntitiesInBox(searchBox, this, nearby);
            std::vector<LivingEntity*> players;
            m_level->GetPlayers(players);
            for (LivingEntity* p : players) {
                if (p && p->GetAABB().Intersects(searchBox)) nearby.push_back(p);
            }

            auto cloud = std::make_unique<AreaEffectCloud>(m_level);
            cloud->position = position;
            if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
                cloud->SetOwner(livingOwner);
            }
            // MC: setCustomParticle(DRAGON_BREATH) — no particle system.
            cloud->SetRadius(3.0f);
            cloud->SetDuration(600);
            cloud->SetRadiusPerTick((7.0f - cloud->GetRadius()) /
                                    static_cast<float>(cloud->GetDuration()));
            cloud->SetPotionDurationScale(0.25f);
            cloud->AddCloudEffect(
                MobEffectInstance(MobEffectId::InstantDamage, 1, 1));

            for (Entity* e : nearby) {
                auto* living = dynamic_cast<LivingEntity*>(e);
                if (!living) continue;
                if (DistanceToSqr(*living) < 16.0) {
                    cloud->position = living->position;
                    break;
                }
            }

            // MC level event 2006 (dragon-breath impact) — no particle system.
            m_level->AddFreshEntity(std::move(cloud));
            Discard();
        }
    }

    // ── Wind charges ───────────────────────────────────────────────────────

    void AbstractWindCharge::Tick() {
        // MC: a charge that flies 30 blocks over the build limit pops. The
        // engine's world tops out at y=320 (MC 1.18 world height).
        if (m_level && !m_level->IsClientSide() && position.y > 320.0 + 30.0) {
            Explode(position);
            Discard();
            return;
        }
        HurtingProjectile::Tick();
    }

    bool AbstractWindCharge::CanHitEntity(const Entity& entity) const {
        if (dynamic_cast<const AbstractWindCharge*>(&entity)) return false;
        return HurtingProjectile::CanHitEntity(entity);
    }

    void AbstractWindCharge::OnHitEntity(LivingEntity& target) {
        if (!m_level || m_level->IsClientSide()) return;

        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }
        target.Hurt(MobDamageSource::Projectile, 1.0f, GetOwner() ? GetOwner() : this);
        Explode(position);
    }

    void AbstractWindCharge::OnHitBlock(const HitResult& hit) {
        if (!m_level || m_level->IsClientSide()) return;
        // MC nudges the burst 0.25 blocks off the struck face along its
        // normal; the block march has no face normal, so the burst happens at
        // the impact point — a quarter-block difference inside a radius-1.2+
        // sphere.
        Explode(hit.location);
        Discard();
    }

    void AbstractWindCharge::OnHit(const HitResult& hit) {
        HurtingProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) Discard();
    }

    void BreezeWindCharge::OnHitEntity(LivingEntity& target) {
        // MC Breeze.isInvulnerableTo: breezes shrug off wind-charge damage —
        // the burst still fires, the 1.0 damage does not land.
        if (!m_level || m_level->IsClientSide()) return;
        if (target.GetType() == EntityTypeId::Breeze) {
            Explode(position);
            return;
        }
        AbstractWindCharge::OnHitEntity(target);
    }

} // namespace Game
