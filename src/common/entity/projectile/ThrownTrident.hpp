// File: src/common/entity/projectile/ThrownTrident.hpp
//
// MC net.minecraft.world.entity.projectile.arrow.ThrownTrident — an
// AbstractArrow variant, exactly as MC derives it: same gravity (0.05), same
// air inertia (0.99), but water inertia 0.99 instead of 0.6 (it flies
// underwater), fixed 8.0 damage instead of velocity-scaled, and after its
// first landed hit it stops finding entities (`dealtDamage`) and drops nearly
// dead in the water — the visible "bounce off" every trident hit has.
//
// SKIPPED, by design for this wave (each also marked at its site):
//   * LOYALTY return flight — enchantments do not exist; a mob-thrown
//     trident has loyalty 0 in MC too, so drowned behaviour is exact;
//   * pickup — mob-thrown tridents are Pickup.DISALLOWED in MC and despawn
//     by the same 1200-tick clock the arrow uses, which is what this does.
#pragma once

#include "common/entity/projectile/Arrow.hpp"

namespace Game {

    class ThrownTrident : public Arrow {
    public:
        explicit ThrownTrident(EntityLevel* level)
            : Arrow(EntityTypeId::Trident, level) {}

        static constexpr float kTridentDamage = 8.0f;

        void Tick() override {
            // MC ThrownTrident.tick head: 4 ticks in the ground marks the
            // damage dealt (so a picked-loose trident no longer hits).
            if (m_inGroundTime > 4) m_dealtDamage = true;
            Arrow::Tick();
        }

        // Save/load: MC's "DealtDamage". Restore-only — Tick re-derives it
        // from m_inGroundTime anyway, but a trident saved mid-flight after a
        // hit would otherwise become able to hit again on load.
        bool IsDealtDamage() const { return m_dealtDamage; }
        void SetDealtDamage(bool v) { m_dealtDamage = v; }

    protected:
        float GetWaterInertia() const override { return 0.99f; }
        bool  FindsHitEntities() const override { return !m_dealtDamage; }
        void  OnHitEntity(LivingEntity& target, const HitResult& hit) override;

    private:
        bool m_dealtDamage = false;
    };

} // namespace Game
