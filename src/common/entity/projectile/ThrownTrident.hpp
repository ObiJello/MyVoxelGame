// File: src/common/entity/projectile/ThrownTrident.hpp
//
// MC net.minecraft.world.entity.projectile.arrow.ThrownTrident — an
// AbstractArrow variant, exactly as MC derives it: same gravity (0.05), same
// air inertia (0.99), but water inertia 0.99 instead of 0.6 (it flies
// underwater), fixed 8.0 damage instead of velocity-scaled (through the
// trident's own `minecraft:damage` effects — Impaling), and after its first
// landed hit it stops finding entities (`dealtDamage`) and drops nearly dead
// in the water — the visible "bounce off" every trident hit has.
//
// The thrown trident IS its item (getWeaponItem = the pickup stack): the
// hit reads its Impaling, a Channeling hit calls the storm down, and
// picking it up gives the same stack back — enchantments and wear intact.
//
// LOYALTY (ID_LOYALTY, synched): once it has hit something (or come loose),
// a loyal trident with a living owner turns ghost — no physics — and homes on
// the owner's eyes at 0.05 × loyalty a tick, playing TRIDENT_RETURN once; an
// owner gone or spectating drops it as an item instead. A loyal, collectable
// trident never despawns.
//
// PICKUP: only its owner (or anyone, when it has none) may collect it: a
// player's throw is ALLOWED (CREATIVE_ONLY from creative), a drowned's
// DISALLOWED; a homing trident also returns to its owner's inventory.
//
// ID_FOIL (synched): the enchantment glint the renderer draws on it.
#pragma once

#include "common/entity/projectile/Arrow.hpp"

namespace Game {

    class ThrownTrident : public Arrow {
    public:
        explicit ThrownTrident(EntityLevel* level)
            : Arrow(EntityTypeId::Trident, level) {}

        // MC TridentItem.BASE_DAMAGE / THROW_THRESHOLD_TIME /
        // PROJECTILE_SHOOT_POWER.
        static constexpr float kTridentDamage   = 8.0f;
        static constexpr int   kThrowThreshold  = 10;
        static constexpr float kShootPower      = 2.5f;

        // MC ThrownTrident(level, owner, tridentItem): the stack becomes the
        // pickup stack (and the weapon the hit reads); ID_LOYALTY is its
        // Loyalty (EnchantmentHelper.getTridentReturnToOwnerAcceleration,
        // server side) and ID_FOIL its glint. Call after the level is set.
        void SetTridentItem(const ItemStack& trident);

        int  GetLoyalty() const { return m_loyalty; }
        bool IsFoil() const { return m_foil; }

        // ID_FLAGS (crit, no-physics) in bits 0-1, ID_FOIL bit 2, ID_LOYALTY
        // (0..31 — vanilla's is at most 3) bits 3-7.
        uint8_t GetVariantByte() const override;
        void    SetVariantByte(uint8_t v) override;

        // MC ThrownTrident.getWeaponItem: the pickup stack itself.
        ItemStack* GetWeaponItem() override {
            return m_pickupItemStack.IsEmpty() ? nullptr : &m_pickupItemStack;
        }
        // MC: a worn-out trident (a post_attack / hit_block effect broke it)
        // is killed.
        void OnItemBreak(const ItemStack&) override { Discard(); }

        void Tick() override;

        // MC ThrownTrident.playerTouch: only its owner, or anyone for an
        // ownerless one.
        void PlayerTouch(LivingEntity& player) override;

        // Save/load: MC's "DealtDamage". Restore-only — Tick re-derives it
        // from m_inGroundTime anyway, but a trident saved mid-flight after a
        // hit would otherwise become able to hit again on load.
        bool IsDealtDamage() const { return m_dealtDamage; }
        void SetDealtDamage(bool v) { m_dealtDamage = v; }

        // MC ThrownTrident.getDefaultHitGroundSoundEvent.
        const char* GetHitGroundSound() const override { return SoundEvents::TRIDENT_HIT_GROUND; }

    protected:
        float GetWaterInertia() const override { return 0.99f; }
        bool  FindsHitEntities() const override { return !m_dealtDamage; }
        void  OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        bool  TryPickup(LivingEntity& player) override;
        ItemStack GetDefaultPickupItem() const override;
        void  TickDespawn() override;
        glm::dvec3 HitBlockEffectsLocation(const glm::dvec3& hitPos,
                                           const glm::ivec3& blockPos) const override;

    private:
        // MC isAcceptibleReturnOwner.
        bool IsAcceptableReturnOwner();
        // MC ThrownTrident.ownedBy (Projectile.ownedBy).
        bool OwnedBy(const Entity& entity);

        bool    m_dealtDamage = false;
        uint8_t m_loyalty = 0;
        bool    m_foil = false;
        // MC clientSideReturnTridentTickCount: the return sound plays once.
        int     m_returnTickCount = 0;
    };

} // namespace Game
