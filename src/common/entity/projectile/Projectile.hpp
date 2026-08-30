// File: src/common/entity/projectile/Projectile.hpp
//
// MC net.minecraft.world.entity.projectile.Projectile — the shared half of
// every non-arrow projectile, plus the hit-scan helpers the arrow already
// proved out.
//
// ARCHITECTURE: same deliberate deviation as Arrow.hpp documents — MC
// projectiles are plain Entities, but this engine's tracking/wire/client
// pipeline is built around Game::Mob, so projectiles derive Mob with all of
// the mob machinery inert (no goals, Tick overridden wholesale, Hurt
// rejected, MobCategory::Misc so they never touch a spawn cap or census).
//
// The block-clip / entity-ray logic here is LIFTED from Arrow.cpp (which now
// calls it too): a 0.05-block march against collision shapes for the block
// half — the same resolution Sensing's line-of-sight uses, since the engine
// has no shape raycast — and a slab-test ray against entity AABBs inflated by
// 0.3 (MC Projectile.canHitEntity's inflate) for the entity half.
#pragma once

#include "common/core/EntityRef.hpp"

#include "common/entity/Mob.hpp"

namespace Game {

    struct IBlockAccess;

    class Projectile : public Mob {
    public:
        // Projectiles ARE serialized. The base Entity default (true) applies:
        // the owner reference that used to make this impossible now round-trips
        // as a UUID through EntityRef, resolved lazily like every other
        // cross-entity reference.

        Projectile(EntityTypeId type, EntityLevel* level);

        // The shooter, as an identity rather than a pointer, so it survives a
        // save. Resolved lazily: the shooter is usually a player who may be
        // offline, or a mob in a chunk that is not loaded, and vanilla keeps
        // the reference indefinitely in both cases.
        void    SetOwner(Entity* owner) { m_ownerRef.Set(owner); }
        void    SetOwnerUuid(const Uuid& uuid) { m_ownerRef.SetUnresolved(uuid); }
        Entity* GetOwner();                       // non-const: resolves lazily
        const EntityRef& OwnerRef() const { return m_ownerRef; }

        // MC Projectile.shoot: normalise, add triangle(0, 0.0172275*inaccuracy)
        // per axis, scale by velocity; rotation snaps to the result.
        void Shoot(double xd, double yd, double zd, float velocity, float inaccuracy);

        // MC Projectile.getDimensionChangingDelay (Projectile.java:343) — 2
        // ticks, not Entity's 300. A thrown thing crossing a portal should
        // keep going, not sit in it.
        int GetDimensionChangingDelay() const override {
            return Portals::kProjectilePortalCooldown;
        }

        // Projectiles never despawn by player distance and are not damageable
        // (ShulkerBullet overrides Hurt — it can be shot down).
        void CheckDespawn() override {}
        bool RemoveWhenFarAway(double) const override { return false; }
        bool Hurt(MobDamageSource, float, Entity*) override { return false; }

        void ClearReferenceTo(const Entity* entity) override {
            Mob::ClearReferenceTo(entity);
            // Demote the pointer; keep the identity unless the shooter
            // actually died, so an arrow still knows who fired it after that
            // player walks out of the chunk.
            m_ownerRef.OnEntityRemoved(entity);
        }

        // MC HitResult, reduced to what the port produces.
        struct HitResult {
            enum class Type : uint8_t { Miss, Block, Entity };
            Type          type = Type::Miss;
            double        t = 1.0;          // fraction of this tick's movement
            glm::ivec3    blockPos{0};      // valid for Block hits
            LivingEntity* entity = nullptr; // valid for Entity hits
            glm::dvec3    location{0.0};    // world-space hit point

            bool IsHit()    const { return type != Type::Miss; }
            bool IsBlock()  const { return type == Type::Block; }
            bool IsEntity() const { return type == Type::Entity; }
        };

    protected:
        // MC ProjectileUtil.getHitResultOnMoveVector over `movement` from
        // `origin`. Entities are only searched when `checkEntities` (the
        // callers pass server-side, matching Arrow's behaviour — the client
        // copy clips blocks only and lets the server's hits arrive on the
        // wire). The nearest of block/entity hit wins, exactly as in MC.
        HitResult Clip(const glm::dvec3& origin, const glm::dvec3& movement,
                       bool checkEntities);

        // MC Projectile.canHitEntity: alive, not the owner, not this. Wind
        // charges add their own exclusions on top.
        virtual bool CanHitEntity(const Entity& entity) const;

        // MC Projectile.onHit — dispatches to the entity/block halves. The
        // subclass overrides this, calls the base FIRST (MC's super.onHit()),
        // then does its both-cases work (discard, explode).
        virtual void OnHit(const HitResult& hit);
        virtual void OnHitEntity(LivingEntity& target) { (void)target; }
        virtual void OnHitBlock(const HitResult& hit) { (void)hit; }

        // MC BlockBehaviour.onProjectileHit — give the BLOCK a chance to react
        // before the projectile does. Today one block cares: TNT lights when a
        // burning projectile hits it. Non-virtual because the dispatch is per
        // BLOCK, not per projectile.
        void NotifyBlockOfProjectileHit(const HitResult& hit);

        // MC ProjectileUtil.rotateTowardsMovement / Projectile.updateRotation
        // — both lerp the rotation pair toward the velocity at `step`/tick
        // (0.2 for most projectiles, 0.5 for the shulker bullet).
        void RotateTowardsMovement(float step);
        static float LerpRotation(float from, float to, float step);

        // ExplodeDamageOnly and KnockbackBurst lived here. Both are gone:
        // they were a near-duplicate of the creeper's explosion helper with
        // the exposure raycast MISSING, which is why a ghast fireball used to
        // deal full damage through a wall. Everything that explodes now goes
        // through Game::Explode (common/world/level/Explosion.hpp), which does
        // the block half too.

        // ── Shared with Arrow ──────────────────────────────────────────────

        // Does this block's collision shape contain the world-space point?
        static bool CollisionShapeContains(const IBlockAccess& blocks,
                                           const glm::ivec3& bp,
                                           const glm::dvec3& point);

        // Ray vs AABB slab test. Entry parameter in [0,1], negative on miss.
        static double RayAabb(const glm::dvec3& origin, const glm::dvec3& dir,
                              const AABB& box);

        EntityRef m_ownerRef;
    };

} // namespace Game
