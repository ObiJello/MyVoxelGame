// File: src/common/entity/projectile/ThrowableProjectile.hpp
//
// MC net.minecraft.world.entity.projectile.ThrowableProjectile and the
// throwableitemprojectile family: Snowball, ThrownEgg, ThrownSplashPotion.
//
// Physics is ThrowableProjectile.tick transcribed: gravity 0.03 (0.05 for a
// potion), then drag 0.99 (0.8 in water), then the hit clip along the
// post-drag velocity, position update, rotation lerp at 0.2, and the on-hit
// dispatch. Same Mob-pipeline deviation as Arrow.hpp documents.
#pragma once

#include "common/entity/projectile/Projectile.hpp"

namespace Game {

    class ThrowableProjectile : public Projectile {
    public:
        ThrowableProjectile(EntityTypeId type, EntityLevel* level);

        // MC ThrowableItemProjectile(type, owner, ...): spawn at the owner's
        // eye height minus 0.1, owner recorded for damage attribution.
        void SetOwnerAndPosition(LivingEntity& owner);

        void Tick() override;

    protected:
        // MC ThrowableProjectile.getDefaultGravity.
        virtual double GetDefaultGravity() const { return 0.03; }
    };

    // MC Snowball. 0 damage — 3 against blazes — and gone on any hit.
    class Snowball : public ThrowableProjectile {
    public:
        explicit Snowball(EntityLevel* level)
            : ThrowableProjectile(EntityTypeId::Snowball, level) {}

    protected:
        void OnHitEntity(LivingEntity& target) override;
        void OnHit(const HitResult& hit) override;
    };

    // MC ThrownEgg. 0 damage; 1-in-8 hatches a baby chicken (1-in-32 of
    // those hatch four). Nothing in the mob set throws one — it exists for
    // parity with the projectile family and for the player-thrown item later.
    class ThrownEgg : public ThrowableProjectile {
    public:
        explicit ThrownEgg(EntityLevel* level)
            : ThrowableProjectile(EntityTypeId::Egg, level) {}

    protected:
        void OnHitEntity(LivingEntity& target) override;
        void OnHit(const HitResult& hit) override;
    };

    // MC ThrownSplashPotion. The payload is a list of MobEffectInstances —
    // MC's PotionContents reduced to what a potion actually delivers — and
    // OnHit is onHitAsPotion: everything within the (4, 2, 4)-inflated box
    // and inside 4 blocks takes scale = 1 - dist/4 of each effect,
    // instantaneous ones through applyInstantenousEffect, durations scaled
    // and dropped when the result would last under a second.
    //
    // Defaults to HARMING (instant damage, amplifier 0) — the witch's
    // baseline throw and what a /summon'd splash potion carries here.
    class ThrownSplashPotion : public ThrowableProjectile {
    public:
        explicit ThrownSplashPotion(EntityLevel* level)
            : ThrowableProjectile(EntityTypeId::SplashPotion, level) {
            m_effects.emplace_back(MobEffectId::InstantDamage, 1);
        }

        // Replace the payload (MC: the ItemStack's PotionContents).
        void SetEffects(std::vector<MobEffectInstance> effects) {
            m_effects = std::move(effects);
        }

    protected:
        double GetDefaultGravity() const override { return 0.05; }  // AbstractThrownPotion
        void OnHit(const HitResult& hit) override;

    private:
        std::vector<MobEffectInstance> m_effects;
    };

} // namespace Game
