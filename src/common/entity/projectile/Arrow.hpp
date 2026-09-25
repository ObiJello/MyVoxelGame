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
#include "common/entity/Item.hpp"
#include "common/entity/alchemy/Potions.hpp"
#include "common/sound/SoundEvents.hpp"

namespace Game {

    struct DamageSourceInfo;

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

        // MC Arrow.addEffect — setPotionContents(contents.withEffectAdded).
        // The stray's SLOWNESS 600 and the bogged's POISON 100 ride this; a
        // mob's arrow has a plain ARROW pickup stack, so its duration scale
        // stays 1.0 and the effect lands at full length.
        void AddEffect(MobEffectInstance effect) {
            m_potion = m_potion.WithEffectAdded(effect);
        }

        // MC Arrow(level, owner, pickupItemStack, weapon) for a tipped arrow:
        // the pickup stack's POTION_CONTENTS and POTION_DURATION_SCALE (the
        // tipped arrow's 0.125) become the payload — getPotionContents /
        // getPotionDurationScale read them back off that stack.
        void SetPotionFromPickupStack(const ItemStack& pickup);
        const PotionContents& GetPotionContents() const { return m_potion; }
        float GetPotionDurationScale() const { return m_potionDurationScale; }
        void  SetPotionContents(const PotionContents& contents, float durationScale) {
            m_potion = contents;
            m_potionDurationScale = durationScale;
        }

        // MC AbstractArrow.firedFromWeapon — a copy of the launcher, which
        // the arrow's hit reads its enchantments off: Power through
        // modifyDamage, Punch through modifyKnockback, the post_attack
        // effects. Mob-fired arrows carry none (mobs hold no equipment here).
        void SetFiredFromWeapon(const ItemStack& weapon) { m_firedFromWeapon = weapon; }
        const ItemStack& GetFiredFromWeapon() const { return m_firedFromWeapon; }
        // MC AbstractArrow.getWeaponItem: the launcher, or null.
        ItemStack* GetWeaponItem() override {
            return m_firedFromWeapon.IsEmpty() ? nullptr : &m_firedFromWeapon;
        }
        // MC AbstractArrow.onItemBreak: an effect wore the launcher copy out.
        void OnItemBreak(const ItemStack&) { m_firedFromWeapon = ItemStack{}; }

        // MC AbstractArrow.setCritArrow / isCritArrow — a fully drawn bow's
        // shot, which adds nextInt(damage / 2 + 2) on the hit.
        void SetCritArrow(bool crit) { m_critArrow = crit; }
        bool IsCritArrow() const { return m_critArrow; }

        bool IsInGroundArrow() const { return m_inGround; }
        // MC AbstractArrow.isPushedByFluid: an arrow stuck in a block is
        // not carried off by the stream running over it.
        bool IsPushedByFluid() const override { return !m_inGround; }
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

        // MC AbstractArrow.getDefaultHitGroundSoundEvent (ARROW_HIT; the
        // trident's TRIDENT_HIT_GROUND) — played on sticking into a block.
        virtual const char* GetHitGroundSound() const { return SoundEvents::ARROW_HIT; }

        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        virtual void OnHitBlockArrow(const glm::dvec3& hitPos,
                                     const glm::ivec3& blockPos);

        // MC AbstractArrow.doKnockback: the launcher's `minecraft:knockback`
        // effects (Punch) from 0, applied along the flight as a push of
        // knockback * 0.6 scaled by the target's KNOCKBACK_RESISTANCE.
        void DoKnockback(LivingEntity& target, const DamageSourceInfo& source);

        void ApplyInertia(float inertia);
        bool ShouldFall() const;
        void StartFalling();
        void TickDespawn();

        double  m_baseDamage = kArrowBaseDamage;
        // MC Arrow's potion contents and duration scale — what its pickup
        // stack carries (EXPOSED_POTION_DECAY_TIME clears them after 600
        // ticks in the ground, as MC swaps the pickup for a plain arrow).
        PotionContents m_potion;
        float          m_potionDurationScale = 1.0f;
        ItemStack      m_firedFromWeapon;
        bool           m_critArrow = false;
        bool    m_inGround = false;
        int     m_inGroundTime = 0;
        int     m_life = 0;
        int     m_shakeTime = 0;
        BlockID m_lastBlock = BlockID::Air;
    };

} // namespace Game
