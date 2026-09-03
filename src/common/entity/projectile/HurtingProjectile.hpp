// File: src/common/entity/projectile/HurtingProjectile.hpp
//
// MC net.minecraft.world.entity.projectile.hurtingprojectile — the
// acceleration-driven projectiles: SmallFireball (blaze), LargeFireball
// (ghast), WitherSkull, DragonFireball, and the wind charges.
//
// Physics is AbstractHurtingProjectile.tick transcribed: each tick the
// velocity gains accelerationPower (0.1) along its own direction and is then
// scaled by the inertia (0.95, 0.8 in water; wind charges 1.0 with zero
// acceleration), so a fireball starts slow and converges on a terminal speed.
// Same Mob-pipeline deviation as Arrow.hpp documents.
#pragma once

#include "common/entity/projectile/Projectile.hpp"
#include "common/world/level/Explosion.hpp"

namespace Game {

    class HurtingProjectile : public Projectile {
    public:
        HurtingProjectile(EntityTypeId type, EntityLevel* level);

        // MC AbstractHurtingProjectile(type, mob, direction, level): position
        // at the shooter, velocity = normalize(direction) * accelerationPower,
        // rotations copied from the shooter.
        void SetOwnerAndDirection(LivingEntity& owner, const glm::dvec3& direction);

        void Tick() override;

        double GetAccelerationPower() const { return m_accelerationPower; }
        void   SetAccelerationPower(double p) { m_accelerationPower = p; }

    protected:
        virtual float GetInertia() const { return 0.95f; }
        virtual float GetLiquidInertia() const { return 0.8f; }
        // MC AbstractHurtingProjectile.shouldBurn — fireballs render (and
        // are) on fire; skulls and wind charges are not.
        virtual bool ShouldBurn() const { return true; }

        double m_accelerationPower = 0.1;   // MC INITAL_ACCELERATION_POWER
    };

    // MC SmallFireball — the blaze's shot: ignites for 5 seconds and deals 5.
    class SmallFireball : public HurtingProjectile {
    public:
        explicit SmallFireball(EntityLevel* level)
            : HurtingProjectile(EntityTypeId::SmallFireball, level) {}

    protected:
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHitBlock(const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;
    };

    // MC LargeFireball — the ghast's: 6 on a direct hit, then an explosion of
    // `explosionPower` blocks. A REAL explosion now (common/world/level/
    // Explosion.hpp) rather than the old damage-only stand-in, which means it
    // both cracks terrain and — more visibly — respects cover.
    class LargeFireball : public HurtingProjectile {
    public:
        LargeFireball(EntityLevel* level, int explosionPower = 1)
            : HurtingProjectile(EntityTypeId::Fireball, level),
              m_explosionPower(explosionPower) {}

    public:
        // Save/load: MC's "ExplosionPower", a byte.
        int  GetExplosionPower() const { return m_explosionPower; }
        void SetExplosionPower(int power) { m_explosionPower = power; }

    protected:
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;

    private:
        int m_explosionPower = 1;   // MC DEFAULT_EXPLOSION_POWER
    };

    // MC WitherSkull — the entity only; the wither boss AI that fires it is a
    // later wave. 8 damage from a living owner (5 magic otherwise), heals the
    // owner 5 on a kill, bursts at radius 1, and a landed hit applies WITHER
    // II for 10s (normal) / 40s (hard).
    // The DANGEROUS (blue) variant flag rides the wire's variant byte — it
    // slows the skull (inertia 0.73) and picks the blue texture.
    class WitherSkull : public HurtingProjectile {
    public:
        explicit WitherSkull(EntityLevel* level)
            : HurtingProjectile(EntityTypeId::WitherSkull, level) {}

        bool IsDangerous() const { return m_dangerous; }
        void SetDangerous(bool v) { m_dangerous = v; }

        uint8_t GetVariantByte() const override { return m_dangerous ? 1 : 0; }
        void    SetVariantByte(uint8_t v) override { m_dangerous = (v & 1) != 0; }

        // The impact explosion visual — answers this port's
        // kEntityEventExplosionSmall stand-in for MC's explosion packet
        // (see EntityLevel.hpp).

    protected:
        float GetInertia() const override {
            return m_dangerous ? 0.73f : HurtingProjectile::GetInertia();
        }
        bool ShouldBurn() const override { return false; }

        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;

    private:
        bool m_dangerous = false;
    };

    // MC DragonFireball — flies, pops, and leaves the lingering dragon-breath
    // AreaEffectCloud (harming II, radius 3 growing to 7 over 600 ticks,
    // recentred on the nearest living entity within 4 blocks), exactly per
    // DragonFireball.onHit. The dragon's StrafePlayer phase is the shooter.
    class DragonFireball : public HurtingProjectile {
    public:
        explicit DragonFireball(EntityLevel* level)
            : HurtingProjectile(EntityTypeId::DragonFireball, level) {}

    protected:
        bool ShouldBurn() const override { return false; }
        void OnHit(const HitResult& hit) override;
    };

    // MC AbstractWindCharge / WindCharge / BreezeWindCharge — zero gravity,
    // zero drag, straight line; on impact a knockback-only burst (no block
    // damage per project policy — creeper precedent — and none of MC's
    // trigger-tag block interactions either).
    class AbstractWindCharge : public HurtingProjectile {
    public:
        AbstractWindCharge(EntityTypeId type, EntityLevel* level)
            : HurtingProjectile(type, level) {
            m_accelerationPower = 0.0;
        }

        void Tick() override;

    protected:
        float GetInertia() const override { return 1.0f; }
        float GetLiquidInertia() const override { return GetInertia(); }
        bool  ShouldBurn() const override { return false; }

        // MC canHitEntity: never another wind charge. (The end-crystal
        // exclusion waits for end crystals.)
        bool CanHitEntity(const Entity& entity) const override;

        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHitBlock(const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;

        virtual void Explode(const glm::dvec3& at) = 0;
    };

    // The player-thrown wind charge: radius 1.2, knockback ×1.22. (The
    // 5-tick no-deflect window is deflection machinery this port does not
    // model — nothing can deflect projectiles yet.)
    class WindCharge : public AbstractWindCharge {
    public:
        explicit WindCharge(EntityLevel* level)
            : AbstractWindCharge(EntityTypeId::WindCharge, level) {}

    protected:
        void Explode(const glm::dvec3& at) override {
            // MC's wind charge: an explosion with entity damage OFF and a 1.22
            // knockback multiplier — it launches, it does not hurt. Radius 1.2
            // and TRIGGER interaction, so it flips levers and pops buttons
            // without breaking anything.
            if (!m_level) return;
            ExplosionParams p;
            p.center              = at;
            p.radius              = 1.2f;
            p.source              = this;
            p.attributedTo        = GetOwner();
            p.interaction         = ExplosionInteraction::Trigger;
            p.damageEntities      = false;
            p.knockbackMultiplier = 1.22f;
            // Qualified: the virtual we are inside shadows the free function.
            Game::Explode(*m_level, p);
        }
    };

    // The breeze's: radius 3.0, no knockback multiplier (MC's shared
    // AbstractWindCharge.EXPLOSION_DAMAGE_CALCULATOR has none).
    class BreezeWindCharge : public AbstractWindCharge {
    public:
        explicit BreezeWindCharge(EntityLevel* level)
            : AbstractWindCharge(EntityTypeId::BreezeWindCharge, level) {}

    protected:
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void Explode(const glm::dvec3& at) override {
            // The breeze's: bigger radius, no multiplier (MC's shared
            // AbstractWindCharge damage calculator has none).
            if (!m_level) return;
            ExplosionParams p;
            p.center              = at;
            p.radius              = 3.0f;
            p.source              = this;
            p.attributedTo        = GetOwner();
            p.interaction         = ExplosionInteraction::Trigger;
            p.damageEntities      = false;
            p.knockbackMultiplier = 1.0f;
            // Qualified: the virtual we are inside shadows the free function.
            Game::Explode(*m_level, p);
        }
    };

} // namespace Game
