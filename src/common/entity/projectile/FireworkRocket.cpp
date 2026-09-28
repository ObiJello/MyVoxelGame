// File: src/common/entity/projectile/FireworkRocket.cpp
//
// MC FireworkRocketEntity (26.3), transcribed. See FireworkRocket.hpp.
#include "common/entity/projectile/FireworkRocket.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/particle/ParticleOptions.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        FireworkAttachHost* g_clientAttachHost = nullptr;

        // MC FireworkRocketEntity.getDefaultItem.
        ItemStack DefaultItem() { return ItemStack(Items::FireworkRocket, 1); }

        // MC getEntitiesOfClass(LivingEntity.class, …): the engine's
        // Mob-shaped entities that are NOT LivingEntities in MC — every MISC
        // type but the golems, the villager and the armor stand (projectiles,
        // decorations, vehicles, TNT, falling blocks, crystals) — are no
        // target of the blast.
        bool IsMcLivingEntity(const LivingEntity& e) {
            if (e.IsPlayer()) return true;
            if (e.TypeInfo().category != MobCategory::Misc) return true;
            switch (e.GetType()) {
                case EntityTypeId::CopperGolem:
                case EntityTypeId::IronGolem:
                case EntityTypeId::SnowGolem:
                case EntityTypeId::Villager:
                case EntityTypeId::ArmorStand:
                    return true;
                default:
                    return false;
            }
        }

        // MC Entity.calculateViewVector(xRot, yRot).
        glm::dvec3 ViewVector(float xRotDeg, float yRotDeg) {
            const double f = static_cast<double>(xRotDeg) * Mth::kDegToRad;
            const double g = -static_cast<double>(yRotDeg) * Mth::kDegToRad;
            const double h = std::cos(g), i = std::sin(g);
            const double j = std::cos(f), k = std::sin(f);
            return { i * j, -k, h * j };
        }
    }

    void FireworkRocket::SetClientAttachHost(FireworkAttachHost* host) { g_clientAttachHost = host; }

    FireworkRocket::FireworkRocket(EntityLevel* level)
        : Projectile(EntityTypeId::FireworkRocket, level), m_item(DefaultItem()) {}

    void FireworkRocket::SetItem(const ItemStack& stack) {
        m_item = stack.IsEmpty() ? DefaultItem() : stack;
        m_dataDirty = true;
    }

    void FireworkRocket::SetAttachedToId(int32_t id) {
        if (id != m_attachedToId) m_dataDirty = true;
        m_attachedToId = id;
    }

    void FireworkRocket::InitLaunch(const glm::dvec3& pos, const ItemStack& sourceItemStack) {
        position = pos;
        oldPosition = pos;
        SetItem(sourceItemStack);
        m_life = 0;
        int flightCount = 1;
        if (auto fireworks = sourceItemStack.get(DataComponents::FIREWORKS)) {
            flightCount += fireworks->flightDuration;
        }
        JavaRandom& r = m_level->Random();
        velocity = glm::dvec3(r.Triangle(0.0, 0.002297), 0.05, r.Triangle(0.0, 0.002297));
        m_lifetime = 10 * flightCount + r.NextInt(6) + r.NextInt(7);
        needsSync = true;
    }

    void FireworkRocket::AttachTo(LivingEntity& stuckTo) {
        SetOwner(&stuckTo);
        SetAttachedToId(stuckTo.GetId());
        m_attached = &stuckTo;
    }

    void FireworkRocket::ClearReferenceTo(const Entity* entity) {
        Projectile::ClearReferenceTo(entity);
        if (entity && entity == m_attached) m_attached = nullptr;
    }

    std::vector<FireworkExplosion> FireworkRocket::GetExplosions() const {
        if (auto fireworks = m_item.get(DataComponents::FIREWORKS)) return fireworks->explosions;
        return {};
    }

    bool FireworkRocket::HasExplosion() const {
        auto fireworks = m_item.get(DataComponents::FIREWORKS);
        return fireworks && !fireworks->explosions.empty();
    }

    bool FireworkRocket::QueryAttached(FireworkAttachState& state) {
        if (!m_level) return false;
        if (m_level->IsClientSide()) {
            return g_clientAttachHost && g_clientAttachHost->Query(m_attachedToId, state);
        }
        if (!m_attached || m_attached->IsRemoved()) return false;
        LivingEntity& e = *m_attached;
        state.position = e.position;
        // A player's view carries knockback in `velocity`, not the player's
        // motion; its known movement is what the client reported.
        state.deltaMovement = e.IsPlayer() ? e.GetKnownMovement() : e.velocity;
        state.lookAngle = ViewVector(e.xRot, e.yRot);
        state.fallFlying = e.IsFallFlying();
        // MC Entity.getHandHoldingItemAngle(FIREWORK_ROCKET): a player holding
        // one only in the off hand holds it in the left (main arm RIGHT).
        state.handAngle = glm::dvec3(0.0);
        if (e.IsPlayer()) {
            const ItemStack* main = e.EquipmentInSlot(EquipmentSlot::MAINHAND);
            const ItemStack* off  = e.EquipmentInSlot(EquipmentSlot::OFFHAND);
            const bool inMain = main && main->itemId == Items::FireworkRocket;
            const bool inOff  = off && off->itemId == Items::FireworkRocket;
            if (inMain || inOff) {
                const bool offOnly = inOff && !inMain;
                state.handAngle = ViewVector(0.0f, e.yRot + (offOnly ? -80.0f : 80.0f)) * 0.5;
            }
        }
        return true;
    }

    void FireworkRocket::Tick() {
        if (!m_level || !m_level->Blocks()) return;
        const bool serverSide = !m_level->IsClientSide();

        // MC super.tick(): Projectile.tick (leftOwner, in Clip below) →
        // Entity.tick → baseTick.
        Entity::BaseTick();
        if (IsRemoved()) return;

        HitResult hit;
        if (IsAttachedToEntity()) {
            FireworkAttachState state;
            if (QueryAttached(state)) {
                glm::dvec3 handAngle(0.0);
                if (state.fallFlying) {
                    // The boost: toward 1.5 blocks a tick along the look,
                    // plus a tenth of the look.
                    const glm::dvec3 look = state.lookAngle;
                    const glm::dvec3 m = state.deltaMovement;
                    const glm::dvec3 boosted = m + glm::dvec3(
                        look.x * 0.1 + (look.x * 1.5 - m.x) * 0.5,
                        look.y * 0.1 + (look.y * 1.5 - m.y) * 0.5,
                        look.z * 0.1 + (look.z * 1.5 - m.z) * 0.5);
                    if (serverSide) {
                        // A player moves on its own client (the host there
                        // applies the boost); anything else moves here.
                        if (m_attached && !m_attached->IsPlayer()) {
                            m_attached->velocity = boosted;
                            m_attached->needsSync = true;
                        }
                    } else if (g_clientAttachHost) {
                        g_clientAttachHost->SetDeltaMovement(m_attachedToId, boosted);
                    }
                    state.deltaMovement = boosted;
                    handAngle = state.handAngle;
                }
                position = state.position + handAngle;
                velocity = state.deltaMovement;
            }
            hit = Clip(position, velocity, serverSide);
        } else {
            if (!m_shotAtAngle) {
                const double horizontalAcceleration = horizontalCollision ? 1.0 : 1.15;
                velocity = glm::dvec3(velocity.x * horizontalAcceleration, velocity.y,
                                      velocity.z * horizontalAcceleration) +
                           glm::dvec3(0.0, 0.04, 0.0);
            }
            const glm::dvec3 movement = velocity;
            hit = Clip(position, movement, serverSide);
            Move(movement);
            // MC restores the delta after the move: a rocket against a
            // ceiling keeps pushing rather than stopping dead.
            velocity = movement;
        }

        if (IsAlive() && hit.IsHit()) {
            OnHit(hit);
            needsSync = true;
        }
        if (IsRemoved()) return;

        // MC Projectile.updateRotation — lerp toward the flight at 0.2.
        RotateTowardsMovement(0.2f);

        if (m_life == 0 && !IsSilent()) {
            // Sent to everyone by the server; the client's copy (except null)
            // plays nothing, as MC's ClientLevel.playSound does.
            m_level->PlaySound(nullptr, position, SoundEvents::FIREWORK_ROCKET_LAUNCH,
                               SoundSource::Ambient, 3.0f, 1.0f);
        }

        ++m_life;
        // (MC's `life % 2 < 2` gate is always true: a spark every tick.)
        if (!serverSide) {
            // The spark trail: FIREWORK, blown back against the climb.
            JavaRandom& r = m_level->Random();
            m_level->AddParticle(ParticleKind::Firework, position.x, position.y, position.z,
                                 r.NextGaussian() * 0.05, -velocity.y * 0.5, r.NextGaussian() * 0.05);
        }

        if (m_life > m_lifetime && serverSide) Explode();
    }

    bool FireworkRocket::CanHitEntity(const Entity& entity) const {
        return entity.IsPickable() && Projectile::CanHitEntity(entity);
    }

    void FireworkRocket::OnHit(const HitResult& hit) {
        if (hit.IsEntity() && hit.entity) {
            OnHitEntity(*hit.entity, hit);
            return;
        }
        if (!hit.IsBlock()) return;
        // MC onHitBlock: the block's entityInside (nothing a rocket trips
        // here), then — server — a rocket with stars goes off against the
        // block, then super.onHitBlock (the block's onProjectileHit).
        if (m_level && !m_level->IsClientSide() && HasExplosion()) Explode();
        if (!IsRemoved()) NotifyBlockOfProjectileHit(hit);
        OnHitBlock(hit);
    }

    void FireworkRocket::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        (void)target;
        (void)hit;
        // MC onHitEntity: super (nothing) then, server side, explode — with
        // or without stars.
        if (m_level && !m_level->IsClientSide()) Explode();
    }

    void FireworkRocket::Explode() {
        if (!m_level || IsRemoved()) return;
        m_level->BroadcastEntityEvent(*this, 17);
        GameEvent(GameEventId::Explode, GetOwner());
        DealExplosionDamage();
        Discard();
    }

    void FireworkRocket::DealExplosionDamage() {
        const std::vector<FireworkExplosion> explosions = GetExplosions();
        if (explosions.empty()) return;
        const float damageAmount = 5.0f + static_cast<float>(explosions.size() * 2);
        Entity* owner = GetOwner();

        if (m_attached && !m_attached->IsRemoved()) {
            m_attached->HurtFrom(MobDamageSource::Fireworks, damageAmount, owner, this);
        }

        constexpr double kRadius = 5.0;
        const glm::dvec3 rocketPos = position;
        AABB box = GetAABB();
        box.min -= glm::vec3(static_cast<float>(kRadius));
        box.max += glm::vec3(static_cast<float>(kRadius));
        std::vector<Entity*> candidates;
        m_level->GetEntitiesInBox(box, this, candidates);
        for (Entity* e : candidates) {
            auto* target = e ? e->AsLiving() : nullptr;
            if (!target || target == m_attached || target->IsRemoved() || !IsMcLivingEntity(*target)) continue;
            if (DistanceToSqr(*target) > 25.0) continue;

            // Line of sight to the feet, then to the middle of the box
            // (ClipContext COLLIDER, no fluids).
            bool canSee = false;
            for (int testStep = 0; testStep < 2; ++testStep) {
                const glm::dvec3 to(target->position.x,
                                    target->position.y + static_cast<double>(target->GetBbHeight()) * 0.5 * testStep,
                                    target->position.z);
                if (!Clip(rocketPos, to - rocketPos, false).IsHit()) {
                    canSee = true;
                    break;
                }
            }
            if (!canSee) continue;
            const float damage = damageAmount *
                static_cast<float>(std::sqrt((kRadius - DistanceTo(*target)) / kRadius));
            target->HurtFrom(MobDamageSource::Fireworks, damage, owner, this);
        }
    }

    void FireworkRocket::HandleEntityEvent(uint8_t id) {
        if (id == 17 && m_level && m_level->IsClientSide()) {
            // MC ClientLevel.createFireworks(x, y, z, deltaMovement,
            // explosions, !isSilent()).
            std::vector<FireworkExplosion> explosions = GetExplosions();
            JavaRandom& r = m_level->Random();
            if (explosions.empty()) {
                // A bare rocket just puffs (the bound is re-rolled each pass,
                // as MC's loop condition does).
                for (int i = 0; i < r.NextInt(3) + 2; ++i) {
                    m_level->AddParticle(ParticleKind::Poof, position.x, position.y, position.z,
                                         r.NextGaussian() * 0.05, 0.005, r.NextGaussian() * 0.05);
                }
            } else {
                // particleEngine.add(new FireworkParticles.Starter(...)) —
                // straight into the engine, past the particle setting and
                // the distance limiter.
                m_level->AddParticle(ParticleOptions::FireworkStarter(std::move(explosions), !IsSilent()),
                                     /*overrideLimiter=*/true, /*alwaysShow=*/true,
                                     position.x, position.y, position.z,
                                     velocity.x, velocity.y, velocity.z);
            }
        }
        Projectile::HandleEntityEvent(id);
    }

} // namespace Game
