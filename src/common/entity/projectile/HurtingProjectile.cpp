// File: src/common/entity/projectile/HurtingProjectile.cpp
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/entity/projectile/AreaEffectCloud.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

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
            // In water: four BUBBLEs a quarter of the motion behind.
            if (IsInWater()) {
                for (int i = 0; i < 4; ++i) {
                    m_level->AddParticle(ParticleKind::Bubble, position.x - velocity.x * 0.25,
                                         position.y - velocity.y * 0.25, position.z - velocity.z * 0.25,
                                         velocity.x, velocity.y, velocity.z);
                }
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

        // MC createParticleTrail (the client copy draws it).
        ParticleOptions trail;
        if (m_level && GetTrailParticle(trail)) {
            m_level->AddParticle(trail, position.x, position.y + 0.5, position.z, 0.0, 0.0, 0.0);
        }
    }

    bool HurtingProjectile::GetTrailParticle(ParticleOptions& out) const {
        out = ParticleOptions(ParticleKind::Smoke);
        return true;
    }

    bool DragonFireball::GetTrailParticle(ParticleOptions& out) const {
        out = ParticleOptions::Power(ParticleKind::DragonBreath, 1.0f);
        return true;
    }

    // ── SmallFireball ──────────────────────────────────────────────────────

    void SmallFireball::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        if (!m_level || m_level->IsClientSide()) return;

        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }

        // MC: ignite FIRST, then hurt — and restore the previous fire ticks
        // when the hit did not land (fire-immune target, invulnerability).
        const int previousFireTicks = target.GetRemainingFireTicks();
        target.IgniteForSeconds(5);
        if (!DealHitDamage(target, hit, MobDamageSource::Projectile, 5.0f,
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

    void LargeFireball::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        if (!m_level || m_level->IsClientSide()) return;
        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }
        DealHitDamage(target, hit, MobDamageSource::Projectile, 6.0f,
                      GetOwner() ? GetOwner() : this);
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

    void WitherSkull::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        if (!m_level || m_level->IsClientSide()) return;

        bool wasHurt;
        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
            wasHurt = DealHitDamage(target, hit, MobDamageSource::Projectile, 8.0f,
                                    livingOwner);
            if (wasHurt && !target.IsAlive()) {
                // MC: a kill heals the wither 5.
                livingOwner->Heal(5.0f);
            }
        } else {
            wasHurt = DealHitDamage(target, hit, MobDamageSource::Generic, 5.0f,
                                    this);
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
                // getEntitiesOfClass: EntitySelector.NO_SPECTATORS.
                if (p && !p->IsSpectator() && p->GetAABB().Intersects(searchBox)) nearby.push_back(p);
            }

            auto cloud = std::make_unique<AreaEffectCloud>(m_level);
            cloud->position = position;
            if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
                cloud->SetOwner(livingOwner);
            }
            // MC: setCustomParticle(PowerParticleOption(DRAGON_BREATH, 1)).
            cloud->SetCustomParticle(ParticleOptions::Power(ParticleKind::DragonBreath, 1.0f));
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

            // MC level event 2006: the dragon-breath impact burst (data 1 =
            // with its sound, -1 = silent).
            m_level->PlayLevelEvent(nullptr, LevelEvent::PARTICLES_DRAGON_FIREBALL_SPLASH, BlockPosition(),
                                    IsSilent() ? -1 : 1);
            m_level->AddFreshEntity(std::move(cloud));
            Discard();
        }
    }

    // ── Wind charges ───────────────────────────────────────────────────────

    void AbstractWindCharge::Tick() {
        // MC: a charge that flies 30 blocks over the build limit
        // (getBlockY() > level.getMaxY() + 30) pops where it is.
        if (m_level && !m_level->IsClientSide() &&
            BlockPosition().y > m_level->GetMaxY() + 30) {
            Explode(position);
            Discard();
            return;
        }
        HurtingProjectile::Tick();
    }

    bool AbstractWindCharge::CanHitEntity(const Entity& entity) const {
        if (dynamic_cast<const AbstractWindCharge*>(&entity)) return false;
        if (entity.GetType() == EntityTypeId::EndCrystal) return false;
        return HurtingProjectile::CanHitEntity(entity);
    }

    void AbstractWindCharge::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        if (!m_level || m_level->IsClientSide()) return;

        auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner());
        if (livingOwner) livingOwner->SetLastHurtMob(&target);
        // MC damageSources().windCharge(this, owner): direct = this charge,
        // causing = the owner (or nobody). 1 damage, then the target's
        // post-attack enchantment effects (its Thorns).
        if (DealHitDamage(target, hit, MobDamageSource::Projectile, 1.0f,
                          GetOwner() ? GetOwner() : this)) {
            const DamageSourceInfo source =
                DamageSourceInfo::Of(MobDamageSource::Projectile, livingOwner, this);
            EnchantmentHelper::DoPostAttackEffects(*m_level, target, source);
        }
        Explode(position);
    }

    void AbstractWindCharge::OnHitBlock(const HitResult& hit) {
        if (!m_level || m_level->IsClientSide()) return;
        // MC: the burst sits a quarter block off the struck face, along its
        // normal (hit location + normal * 0.25). The hit carries a point, not
        // a face, so the face is recovered from where on the cell the point
        // lies — the same recovery NotifyBlockOfProjectileHit makes.
        const glm::dvec3 local = hit.location - glm::dvec3(hit.blockPos) - glm::dvec3(0.5);
        const double ax = std::abs(local.x), ay = std::abs(local.y), az = std::abs(local.z);
        glm::dvec3 normal(0.0);
        if (ay >= ax && ay >= az)  normal.y = local.y > 0.0 ? 1.0 : -1.0;
        else if (ax >= az)         normal.x = local.x > 0.0 ? 1.0 : -1.0;
        else                       normal.z = local.z > 0.0 ? 1.0 : -1.0;
        Explode(hit.location + normal * 0.25);
        Discard();
    }

    void AbstractWindCharge::OnHit(const HitResult& hit) {
        HurtingProjectile::OnHit(hit);
        if (m_level && !m_level->IsClientSide()) Discard();
    }

    void BreezeWindCharge::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // MC Breeze.isInvulnerableTo: breezes shrug off wind-charge damage —
        // the burst still fires, the 1.0 damage does not land.
        if (!m_level || m_level->IsClientSide()) return;
        if (target.GetType() == EntityTypeId::Breeze) {
            Explode(position);
            return;
        }
        AbstractWindCharge::OnHitEntity(target, hit);
    }

} // namespace Game
