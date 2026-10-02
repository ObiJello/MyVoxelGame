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

#include <algorithm>
#include <string>
#include <vector>

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
        // (A trident's weapon IS the trident: it kills itself instead.)
        virtual void OnItemBreak(const ItemStack&) { m_firedFromWeapon = ItemStack{}; }

        // ── Pickup (MC AbstractArrow.pickup / pickupItemStack) ─────────────
        //
        // A player-fired arrow (or trident) can be collected once it sticks:
        // ALLOWED gives the pickup stack back, CREATIVE_ONLY lets a creative
        // player sweep it up (an infinite shot's), DISALLOWED — every mob's —
        // never. Collected by walking into it (Player.aiStep's touch, run
        // here from the arrow's side: TouchPlayers).
        enum class Pickup : uint8_t { Disallowed = 0, Allowed = 1, CreativeOnly = 2 };
        Pickup GetPickup() const { return m_pickup; }
        void   SetPickup(Pickup pickup) { m_pickup = pickup; }
        // MC pickupItemStack: what a pickup hands back — the fired arrow
        // (tipped arrows keep their potion), the thrown trident. Empty = the
        // type's default (getDefaultPickupItem).
        const ItemStack& GetPickupItemStack() const { return m_pickupItemStack; }
        void SetPickupItemStack(const ItemStack& stack) {
            m_pickupItemStack = stack;
            if (!m_pickupItemStack.IsEmpty()) m_pickupItemStack.count = 1;
        }
        // MC getPickupItem: a copy of the pickup stack.
        ItemStack GetPickupItem() const {
            return m_pickupItemStack.IsEmpty() ? GetDefaultPickupItem() : m_pickupItemStack;
        }
        // MC AbstractArrow.playerTouch: stuck (or flying free of physics)
        // and done shaking, a successful tryPickup takes it — the pickup
        // sound, and the arrow is gone. Server only.
        virtual void PlayerTouch(LivingEntity& player);

        // MC AbstractArrow.isNoPhysics / setNoPhysics (ID_FLAGS bit 2): no
        // block or entity collision, no gravity — a Loyalty trident flying
        // home. Synced in the variant byte.
        bool IsNoPhysics() const { return m_noPhysics; }
        void SetNoPhysics(bool noPhysics) { m_noPhysics = noPhysics; }

        // MC AbstractArrow.setCritArrow / isCritArrow — a fully drawn bow's
        // shot, which adds nextInt(damage / 2 + 2) on the hit.
        void SetCritArrow(bool crit) { m_critArrow = crit; }
        bool IsCritArrow() const { return m_critArrow; }
        // MC AbstractArrow ID_FLAGS bit 1 (the crit flag) rides the variant
        // byte — the client needs it for the flight trail.
        // Bit 1 is MC's FLAG_NOPHYSICS.
        uint8_t GetVariantByte() const override {
            return static_cast<uint8_t>((m_critArrow ? 1 : 0) | (m_noPhysics ? 2 : 0));
        }
        void    SetVariantByte(uint8_t v) override {
            m_critArrow = (v & 1) != 0;
            m_noPhysics = (v & 2) != 0;
        }

        // MC AbstractArrow.setPierceLevel / getPierceLevel — a Piercing
        // crossbow's arrow passes through this many entities (and is spent
        // on the next).
        void SetPierceLevel(int level) { m_pierceLevel = std::clamp(level, 0, 127); }
        int  GetPierceLevel() const { return m_pierceLevel; }
        // MC AbstractArrow.setSoundEvent — the hit sound (a crossbow's arrow
        // is CROSSBOW_HIT). Empty = the type's default (GetHitGroundSound).
        void SetSoundEvent(std::string sound) { m_soundEvent = std::move(sound); }
        const char* HitSoundEvent() const {
            return m_soundEvent.empty() ? GetHitGroundSound() : m_soundEvent.c_str();
        }

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
        bool CanHitEntity(const Entity& entity) const override;
        virtual void OnHitBlockArrow(const glm::dvec3& hitPos,
                                     const glm::ivec3& blockPos);

        // MC AbstractArrow.doKnockback: the launcher's `minecraft:knockback`
        // effects (Punch) from 0, applied along the flight as a push of
        // knockback * 0.6 scaled by the target's KNOCKBACK_RESISTANCE.
        void DoKnockback(LivingEntity& target, const DamageSourceInfo& source);

        void ApplyInertia(float inertia);
        bool ShouldFall() const;
        void StartFalling();
        // MC tickDespawn — the ThrownTrident keeps a loyal, collectable one.
        virtual void TickDespawn();

        // MC tryPickup / getDefaultPickupItem.
        virtual bool TryPickup(LivingEntity& player);
        virtual ItemStack GetDefaultPickupItem() const;
        // Player.aiStep's touch of this arrow: every player whose pickup box
        // (its box inflated 1 x 0.5 x 1) overlaps it (server).
        void TouchPlayers();
        // MC ThrownTrident.hitBlockEnchantmentEffects: where the hit_block
        // effects are run — the hit location (the trident clamps it into
        // the struck block, so a lightning rod hit from above is the rod's).
        virtual glm::dvec3 HitBlockEffectsLocation(const glm::dvec3& hitPos,
                                                   const glm::ivec3& blockPos) const {
            (void)blockPos;
            return hitPos;
        }

        // MC Arrow.makeParticle / handleEntityEvent(0), sent from the server.
        void SendTippedParticles(int amount);

        double  m_baseDamage = kArrowBaseDamage;
        // MC Arrow's potion contents and duration scale — what its pickup
        // stack carries (EXPOSED_POTION_DECAY_TIME clears them after 600
        // ticks in the ground, as MC swaps the pickup for a plain arrow).
        PotionContents m_potion;
        float          m_potionDurationScale = 1.0f;
        ItemStack      m_firedFromWeapon;
        bool           m_critArrow = false;
        // MC pierceLevel / piercingIgnoreEntityIds / soundEvent.
        int                  m_pierceLevel = 0;
        std::vector<int32_t> m_piercingIgnore;
        // MC piercedAndKilledEntities: what this piercing flight has killed
        // (ids, resolved when KILLED_BY_ARROW fires); cleared with the
        // ignore list (resetPiercedEntities).
        std::vector<int32_t> m_piercedAndKilled;
        std::string          m_soundEvent;
        Pickup    m_pickup = Pickup::Disallowed;
        ItemStack m_pickupItemStack;
        bool      m_noPhysics = false;
        bool    m_inGround = false;
        int     m_inGroundTime = 0;
        int     m_life = 0;
        int     m_shakeTime = 0;
        BlockID m_lastBlock = BlockID::Air;
    };

} // namespace Game
