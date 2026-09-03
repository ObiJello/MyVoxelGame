// File: src/common/entity/projectile/Arrow.hpp
//
// MC net.minecraft.world.entity.projectile.arrow.AbstractArrow + Arrow.
//
// DELIBERATE ARCHITECTURE DEVIATION, documented once here: MC's arrow is a
// plain Entity, but this engine's whole entity pipeline — MobManager on the
// server, ServerEntityTracker on the wire, ClientMobManager mirroring on the
// client — is built around Game::Mob. The arrow therefore derives from Mob
// (via Projectile, which carries the shared hit-scan machinery) and rides
// that pipeline unchanged, with ALL of the mob machinery inert:
//
//   * Tick() is overridden wholesale and never calls Mob::Tick, so goals,
//     navigation and controls never run;
//   * no goals are registered, CheckDespawn is replaced by the arrow's own
//     1200-tick in-ground life, and Hurt() rejects damage (MC arrows are not
//     damageable);
//   * MobCategory::Misc keeps it out of every spawn census and cap.
//
// Generalising the pipeline to Entity is the eventual cleanup; until then this
// buys exact tracking/interpolation/wire behaviour for the cost of a few
// unused members.
//
// The physics is AbstractArrow.tick transcribed: inertia 0.99 (0.6 in water),
// gravity 0.05, 7-tick shake on block hit, 1200-tick in-ground despawn,
// re-fall when the supporting block is removed, damage = ceil(|velocity| *
// baseDamage) where a mob-fired arrow's baseDamage is power*2 +
// triangle(difficultyId * 0.11, 0.57425).
#pragma once

#include "common/entity/projectile/Projectile.hpp"

namespace Game {

    class Arrow : public Projectile {
    public:
        explicit Arrow(EntityLevel* level);

        // MC AbstractArrow constants.
        static constexpr double kArrowBaseDamage = 2.0;
        static constexpr int    kShakeTime = 7;
        static constexpr float  kWaterInertia = 0.6f;
        static constexpr float  kInertia = 0.99f;
        static constexpr int    kDespawnLife = 1200;
        static constexpr double kGravity = 0.05;

        // MC AbstractArrow.setBaseDamageFromMob.
        void SetBaseDamageFromMob(float power);

        // MC Arrow.addEffect — a tipped arrow's payload. Custom effects apply
        // at FULL duration on hit (the /8 division is PotionContents' business
        // on a crafted tipped arrow, which no mob fires). Stray arrows carry
        // SLOWNESS 600 through this.
        void AddEffect(MobEffectInstance effect) {
            m_effects.push_back(std::move(effect));
        }

        bool IsInGroundArrow() const { return m_inGround; }
        int  GetShakeTime() const { return m_shakeTime; }

        // ── Save/load accessors ────────────────────────────────────────────
        //
        // AbstractArrow's persistent state, which is otherwise `protected`.
        // These are restore-only setters: they assign the field and nothing
        // else, because the save already holds the consequences of whatever
        // set them the first time. SetInGround in particular must NOT re-run
        // StartFalling / the hit sound — a stuck arrow that plays its thunk
        // every time the chunk loads is the bug this shape avoids.
        double GetBaseDamage() const { return m_baseDamage; }
        void   SetBaseDamage(double d) { m_baseDamage = d; }
        int    GetLife() const { return m_life; }
        void   SetLife(int life) { m_life = life; }
        int    GetInGroundTime() const { return m_inGroundTime; }
        void   SetInGroundTime(int ticks) { m_inGroundTime = ticks; }
        void   SetShakeTime(int ticks) { m_shakeTime = ticks; }
        void   SetInGround(bool inGround) { m_inGround = inGround; }

        void Tick() override;

    protected:
        // The variant constructor — ThrownTrident is an arrow of a different
        // type id, exactly as MC's `ThrownTrident extends AbstractArrow`.
        Arrow(EntityTypeId type, EntityLevel* level);

        // MC AbstractArrow.getWaterInertia — 0.6; the trident's 0.99 is what
        // lets it fly underwater.
        virtual float GetWaterInertia() const { return kWaterInertia; }

        // MC ThrownTrident.findHitEntity returns null after dealtDamage, so
        // the trident passes through entities on the rebound. Everything else
        // always searches.
        virtual bool FindsHitEntities() const { return true; }

        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        virtual void OnHitBlockArrow(const glm::dvec3& hitPos,
                                     const glm::ivec3& blockPos);

        void ApplyInertia(float inertia);
        bool ShouldFall() const;
        void StartFalling();
        void TickDespawn();

        double  m_baseDamage = kArrowBaseDamage;
        // MC Arrow's potion contents, reduced to the delivered effects.
        std::vector<MobEffectInstance> m_effects;
        bool    m_inGround = false;
        int     m_inGroundTime = 0;
        int     m_life = 0;
        int     m_shakeTime = 0;
        BlockID m_lastBlock = BlockID::Air;
    };

} // namespace Game
