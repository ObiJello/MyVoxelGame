// File: src/common/entity/projectile/Arrow.cpp
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    Arrow::Arrow(EntityLevel* level) : Projectile(EntityTypeId::Arrow, level) {}

    Arrow::Arrow(EntityTypeId type, EntityLevel* level) : Projectile(type, level) {}

    void Arrow::SetBaseDamageFromMob(float power) {
        // MC: power * 2 + triangle(difficultyId * 0.11, 0.57425).
        double bonus = 0.0;
        if (m_level) {
            const double mode =
                static_cast<double>(static_cast<int>(m_level->GetDifficulty())) * 0.11;
            bonus = m_level->Random().Triangle(mode, 0.57425);
        }
        m_baseDamage = static_cast<double>(power) * 2.0 + bonus;
    }

    void Arrow::ApplyInertia(float inertia) {
        velocity *= static_cast<double>(inertia);
    }

    bool Arrow::ShouldFall() const {
        if (!m_inGround || !m_level) return false;
        // MC: noCollision(AABB(position).inflate(0.06)) — the supporting block
        // is gone when nothing intersects a tiny box around the arrow.
        AABB probe;
        probe.min = glm::vec3(position) - glm::vec3(0.06f);
        probe.max = glm::vec3(position) + glm::vec3(0.06f);
        PhysicsContext phys;
        phys.blockAccess = m_level->Blocks();
        return !CollidesAt(probe, phys);
    }

    void Arrow::StartFalling() {
        m_inGround = false;
        JavaRandom& rng = m_level->Random();
        velocity.x *= rng.NextFloat() * 0.2f;
        velocity.y *= rng.NextFloat() * 0.2f;
        velocity.z *= rng.NextFloat() * 0.2f;
        m_life = 0;
    }

    void Arrow::TickDespawn() {
        ++m_life;
        if (m_life >= kDespawnLife) Discard();
    }

    void Arrow::OnHitBlockArrow(const glm::dvec3& hitPos, const glm::ivec3& blockPos) {
        m_lastBlock = m_level->Blocks()->GetBlock(blockPos.x, blockPos.y, blockPos.z);

        // MC: back the position off the surface by 0.05 against the movement
        // sign, so the arrow visibly sticks OUT of the block.
        const glm::dvec3 offset(std::copysign(0.05, velocity.x),
                                std::copysign(0.05, velocity.y),
                                std::copysign(0.05, velocity.z));
        position = hitPos - offset;
        velocity = glm::dvec3(0.0);
        m_inGround = true;
        m_shakeTime = kShakeTime;
        needsSync = true;
    }

    void Arrow::OnHitEntity(LivingEntity& target) {
        const double speed = glm::length(velocity);
        const int damage = static_cast<int>(std::ceil(
            std::clamp(speed * m_baseDamage, 0.0, 2.147483647e9)));

        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }

        // MC: a burning arrow ignites (endermen excepted — no teleport-dodge
        // is modelled yet, so the exception waits with the enderman).
        if (IsOnFire()) target.IgniteForSeconds(5);

        // Damage attribution goes to the OWNER so HurtByTargetGoal retaliates
        // against the skeleton, which is what starts vanilla's
        // skeleton-vs-zombie fights. (MC aims the knockback from the arrow's
        // own position; here it derives from the attacker, which for a
        // just-fired arrow points the same way.)
        if (target.Hurt(MobDamageSource::Projectile, static_cast<float>(damage),
                        GetOwner() ? GetOwner() : this)) {
            // MC AbstractArrow.doPostHurtEffects → Arrow.doPostHurtEffects:
            // the tip's effects land after a successful hit, attributed to the
            // shooter (getEffectSource).
            for (const MobEffectInstance& effect : m_effects) {
                target.AddEffect(effect, GetOwner() ? GetOwner() : this);
            }
            Discard();
        } else {
            // MC ProjectileDeflection.REVERSE on an invulnerable target.
            velocity *= -0.5;
            yRot += 180.0f;
            yRotO += 180.0f;
        }
    }

    void Arrow::Tick() {
        if (!m_level || !m_level->Blocks()) return;
        const IBlockAccess& blocks = *m_level->Blocks();
        const bool serverSide = !m_level->IsClientSide();

        // ── Embedded check (MC tick() head) ────────────────────────────────
        {
            const glm::ivec3 bp = BlockPosition();
            if (!m_inGround && CollisionShapeContains(blocks, bp, position)) {
                velocity = glm::dvec3(0.0);
                m_inGround = true;
            }
        }

        if (m_shakeTime > 0) --m_shakeTime;
        if (IsInWater()) m_remainingFireTicks = 0;

        if (m_inGround) {
            if (serverSide) {
                const glm::ivec3 bp = BlockPosition();
                const BlockID current = blocks.GetBlock(bp.x, bp.y, bp.z);
                if (current != m_lastBlock && ShouldFall()) {
                    StartFalling();
                } else {
                    TickDespawn();
                }
            }
            ++m_inGroundTime;
            return;   // MC's inGround branch never reaches super.tick()
        }

        m_inGroundTime = 0;
        const glm::dvec3 origin = position;
        if (IsInWater()) ApplyInertia(GetWaterInertia());

        const glm::dvec3 movement = velocity;

        // ── Rotation follows the velocity (MC lerpRotation at 0.2) ─────────
        const double horiz = std::sqrt(movement.x * movement.x + movement.z * movement.z);
        const float targetYRot =
            static_cast<float>(std::atan2(movement.x, movement.z) * Mth::kRadToDeg);
        const float targetXRot =
            static_cast<float>(std::atan2(movement.y, horiz) * Mth::kRadToDeg);
        xRot = LerpRotation(xRot, targetXRot, 0.2f);
        yRot = LerpRotation(yRot, targetYRot, 0.2f);
        yHeadRot = yBodyRot = yRot;

        // ── Block + entity clip along this tick's movement (Projectile) ────
        const HitResult hit =
            Clip(origin, movement, serverSide && FindsHitEntities());

        // ── Advance, then resolve whichever hit came first ─────────────────
        if (hit.IsEntity() && hit.entity) {
            position = hit.location;
            OnHitEntity(*hit.entity);
            if (IsRemoved()) return;
        } else if (hit.IsBlock()) {
            OnHitBlockArrow(hit.location, hit.blockPos);
        } else {
            position = origin + movement;
        }

        if (!IsInWater()) ApplyInertia(kInertia);
        if (!m_inGround) velocity.y -= kGravity;

        // MC's flying branch ends in super.tick(); Entity::BaseTick is the
        // part of that chain an arrow actually needs (fire, void cull, fall
        // bookkeeping is irrelevant at zero fall damage).
        Entity::BaseTick();
    }

} // namespace Game
