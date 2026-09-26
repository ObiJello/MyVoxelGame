// File: src/common/entity/projectile/Arrow.cpp
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

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

    void Arrow::SetPotionFromPickupStack(const ItemStack& pickup) {
        m_potion = Game::GetPotionContents(pickup);
        m_potionDurationScale = Game::GetPotionDurationScale(pickup);
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
        // MC onHitBlock: the thunk, 1.2 / (0.9..1.1).
        PlaySound(GetHitGroundSound(), 1.0f, 1.2f / (m_level->Random().NextFloat() * 0.2f + 0.9f));
        // MC AbstractArrow.onHitBlock: the launcher's hit_block effects
        // (Channeling on a lightning rod), a worn-out launcher dropping the
        // copy (onItemBreak).
        if (ItemStack* weapon = GetWeaponItem(); weapon && !m_level->IsClientSide()) {
            LivingEntity* owner = GetOwner() ? GetOwner()->AsLiving() : nullptr;
            EnchantmentHelper::OnHitBlock(*m_level, *weapon, owner, *this, hitPos,
                                          m_level->Blocks()->GetBlockState(blockPos.x, blockPos.y, blockPos.z),
                                          [this](const ItemStack& broken) { OnItemBreak(broken); });
        }
    }

    void Arrow::DoKnockback(LivingEntity& target, const DamageSourceInfo& source) {
        float knockback = 0.0f;
        if (ItemStack* weapon = GetWeaponItem(); weapon && m_level && !m_level->IsClientSide()) {
            knockback = EnchantmentHelper::ModifyKnockback(*m_level, *weapon, target, source, 0.0f);
        }
        if (knockback <= 0.0f) return;
        const double resistance =
            std::max(0.0, 1.0 - target.GetAttributeValue(Attribute::KnockbackResistance));
        // deltaMovement.multiply(1, 0, 1).normalize().scale(kb * 0.6 * res).
        glm::dvec3 flight(velocity.x, 0.0, velocity.z);
        const double len = glm::length(flight);
        if (len < 1.0e-4) return;   // Vec3.normalize's zero for a vertical shot
        flight *= static_cast<double>(knockback) * 0.6 * resistance / len;
        if (flight.x * flight.x + flight.z * flight.z > 0.0) {
            // Entity.push(x, 0.1, z).
            target.AddDeltaMovement(glm::dvec3(flight.x, 0.1, flight.z));
        }
    }

    void Arrow::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        const double speed = glm::length(velocity);
        // damageSources().arrow(this, owner ?: this).
        Entity* const owner = GetOwner();
        const DamageSourceInfo source =
            DamageSourceInfo::Of(MobDamageSource::Projectile, owner ? owner : this, this);
        // MC AbstractArrow.onHitEntity: the launcher's `minecraft:damage`
        // effects rewrite the base damage (Power's +1, +0.5 a level) before
        // the speed multiplies it.
        double arrowDamage = m_baseDamage;
        ItemStack* const weapon = GetWeaponItem();
        if (weapon && m_level && !m_level->IsClientSide()) {
            arrowDamage = static_cast<double>(EnchantmentHelper::ModifyDamage(
                *m_level, *weapon, target, source, static_cast<float>(arrowDamage)));
        }
        int damage = static_cast<int>(std::ceil(
            std::clamp(speed * arrowDamage, 0.0, 2.147483647e9)));
        // A crit arrow (a fully drawn bow): + nextInt(damage / 2 + 2).
        if (m_critArrow && m_level) {
            const long long bonus = m_level->Random().NextInt(damage / 2 + 2);
            damage = static_cast<int>(std::min<long long>(bonus + damage, 2147483647LL));
        }

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
        if (DealHitDamage(target, hit, MobDamageSource::Projectile,
                          static_cast<float>(damage),
                          GetOwner() ? GetOwner() : this)) {
            // MC: doKnockback (Punch), then
            // EnchantmentHelper.doPostAttackEffectsWithItemSource — the
            // target's Thorns and the launcher's post_attack effects.
            DoKnockback(target, source);
            if (m_level && !m_level->IsClientSide()) {
                EnchantmentHelper::DoPostAttackEffectsWithItemSource(*m_level, target, source, weapon);
            }
            // MC AbstractArrow.doPostHurtEffects → Arrow.doPostHurtEffects:
            // potionContents.forEachEffect(mob.addEffect(effect, source),
            // durationScale) — every effect, instant ones included, scaled
            // by the pickup stack's POTION_DURATION_SCALE (1/8 for a tipped
            // arrow), attributed to the shooter (getEffectSource).
            Entity* effectSource = GetOwner() ? GetOwner() : this;
            m_potion.ForEachEffect([&](MobEffectInstance effect) {
                target.AddEffect(std::move(effect), effectSource);
            }, m_potionDurationScale);
            // MC: this.soundEvent (ARROW_HIT). The shooter's ARROW_HIT_PLAYER
            // ding is a game-event packet to that one player — no such packet.
            PlaySound(SoundEvents::ARROW_HIT, 1.0f, 1.2f / (m_level->Random().NextFloat() * 0.2f + 0.9f));
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
            // MC Arrow.tick's server tail: EXPOSED_POTION_DECAY_TIME — after
            // 600 ticks in the ground the pickup becomes a plain arrow and
            // the tip is gone. (The entity event 0 puff is a particle burst
            // with no system here.)
            if (serverSide && m_inGroundTime != 0 && !m_potion.IsEmpty() &&
                m_inGroundTime >= 600) {
                m_potion = PotionContents{};
                m_potionDurationScale = 1.0f;
            }
            return;   // MC's inGround branch never reaches super.tick()
        }

        m_inGroundTime = 0;
        const glm::dvec3 origin = position;
        if (IsInWater()) ApplyInertia(GetWaterInertia());

        const glm::dvec3 movement = velocity;

        // MC AbstractArrow.tick: a crit arrow sheds four CRIT sparks along
        // this tick's path, blown back against the flight. (ServerLevel's
        // addParticle draws nothing; the client's copy spawns them.)
        if (m_critArrow && !serverSide) {
            for (int i = 0; i < 4; ++i) {
                const double t = static_cast<double>(i) / 4.0;
                m_level->AddParticle(ParticleKind::Crit,
                                     origin.x + movement.x * t, origin.y + movement.y * t,
                                     origin.z + movement.z * t,
                                     -movement.x, -movement.y + 0.2, -movement.z);
            }
        }

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
            OnHitEntity(*hit.entity, hit);
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
