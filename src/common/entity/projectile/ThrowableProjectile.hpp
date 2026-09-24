// File: src/common/entity/projectile/ThrowableProjectile.hpp
//
// MC net.minecraft.world.entity.projectile.ThrowableProjectile and the
// throwableitemprojectile family: Snowball, ThrownEgg, ThrownEnderpearl and
// the thrown potions (ThrownSplashPotion covers MC's splash and lingering).
//
// Physics is ThrowableProjectile.tick transcribed: gravity 0.03 (0.05 for a
// potion), then drag 0.99 (0.8 in water), then the hit clip along the
// post-drag velocity, position update, rotation lerp at 0.2, and the on-hit
// dispatch. Same Mob-pipeline deviation as Arrow.hpp documents.
#pragma once

#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/alchemy/Potions.hpp"

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
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
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
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;
    };

    // MC ThrownEnderpearl. 0 damage on a direct hit; on ANY hit the OWNER is
    // pulled to the pearl's pre-impact position — the 5% endermite, the 5.0
    // teleport damage and the fall-distance reset included. Ported minus,
    // each noted at its site: the 32 PORTAL impact particles (no such
    // ParticleKind), the teleport sound (sound system stub), MC 1.21's
    // pearl chunk tickets / owner-logout survival / ENDER_PEARLS_VANISH_ON_
    // DEATH gamerule (pearls here live like every other projectile, in
    // loaded chunks with a live owner reference), and dimension travel (the
    // engine's portals move players only — a pearl thrown into a portal
    // sits, it does not cross).
    class ThrownEnderpearl : public ThrowableProjectile {
    public:
        explicit ThrownEnderpearl(EntityLevel* level)
            : ThrowableProjectile(EntityTypeId::EnderPearl, level) {}

    protected:
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;
    };

    // MC AbstractThrownPotion + ThrownSplashPotion + ThrownLingeringPotion —
    // ONE class here, because MC's two differ only in onHitAsPotion and the
    // carried item already says which one this is (ThrowableItemProjectile.
    // getItem: a splash_potion or a lingering_potion stack). The engine has
    // no lingering_potion entity type on the wire, so a thrown lingering
    // potion rides as EntityTypeId::SplashPotion (the ZephyrSnowball-as-
    // Snowball precedent) and its SAVE keeps it lingering: the "Item" compound
    // is written, exactly as MC writes it, and the item decides.
    //
    // OnHit is AbstractThrownPotion.onHit: the water-potion extras
    // (affectEntitiesAround: 1.0 indirect-magic to water-sensitive mobs,
    // extinguish burning ones, rehydrate axolotls; onHitBlock: douse fire,
    // lit candles and lit campfires), then onHitAsPotion when the contents
    // have effects —
    //   splash:    everything in the potion box inflated by (4, 2, 4) and
    //              within 4 blocks (AABB-to-AABB) takes scale = 1 - dist/4:
    //              instantaneous effects through applyInstantaneousEffect at
    //              that scale, the rest with duration scale * d * the stack's
    //              POTION_DURATION_SCALE + 0.5, dropped under a second;
    //   lingering: an AreaEffectCloud (radius 3, radiusOnUse -0.5, 600
    //              ticks, wait 10, shrinking to nothing over its life) that
    //              carries the stack's contents and duration scale (0.25).
    // Not modelled: the splash particle / sound level events (2002 / 2007 /
    // 1053 / 1054 — no particle system for them).
    class ThrownSplashPotion : public ThrowableProjectile {
    public:
        explicit ThrownSplashPotion(EntityLevel* level);

        // MC ThrowableItemProjectile.setItem / getItem. The stack is copied
        // at count 1; its POTION_CONTENTS and POTION_DURATION_SCALE are the
        // payload.
        void SetItem(const ItemStack& stack);
        const ItemStack& GetItem() const { return m_item; }

        // ThrownLingeringPotion vs ThrownSplashPotion.
        bool IsLingering() const;

    protected:
        double GetDefaultGravity() const override { return 0.05; }  // AbstractThrownPotion
        void OnHitBlock(const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;

    private:
        void AffectEntitiesAround(const PotionContents& potion);
        void OnHitAsSplash(const PotionContents& contents, float durationScale,
                           const HitResult& hit);
        void OnHitAsLingering(const HitResult& hit);
        void DouseFire(const glm::ivec3& pos);

        ItemStack m_item;
    };

} // namespace Game
