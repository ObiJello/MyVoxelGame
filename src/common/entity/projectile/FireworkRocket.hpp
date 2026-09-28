// File: src/common/entity/projectile/FireworkRocket.hpp
//
// MC net.minecraft.world.entity.projectile.FireworkRocketEntity — a launched
// firework: it climbs (or, shot at an angle, flies straight), trails sparks,
// and at the end of its lifetime goes off — entity event 17, which each
// client turns into a FireworkParticles.Starter carrying the rocket's
// explosions — hurting whatever living thing near it the blast can see.
// Attached to a gliding player (used from the hand mid-flight) it rides at
// their hand and accelerates them along their look vector.
//
// Rides the projectile pipeline like every other projectile here (see
// Projectile.hpp): a Mob with the mob machinery inert.
//
// SYNCHED DATA (MC DATA_ID_FIREWORKS_ITEM, DATA_ATTACHED_TO_TARGET,
// DATA_SHOT_AT_ANGLE) rides FireworkRocketDataS2C (0x73): sent to a watcher
// right after the add packet and to every watcher whenever it changes. The
// client needs the stack for its explosions and its look, the attachment to
// follow (and, for the local player, be boosted by) the rocket, and the angle
// flag for how it is drawn.
//
// THE BOOST. MC runs the rocket's tick on both sides and the attached
// entity's deltaMovement is written by whichever side owns its movement —
// for a player that is their own client (LocalPlayer). Here the client's mob
// level holds no player entities, so the client half of the attachment goes
// through FireworkAttachHost (set by the client): it answers where the
// attached entity is, whether it glides and where it looks, and takes the
// boosted velocity for the local player. On the server the attached entity is
// the player's view, whose velocity is not the player's movement — the server
// only keeps the rocket at the player's hand.
#pragma once

#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/Item.hpp"
#include "common/data/FireworkExplosion.hpp"

#include <cstdint>
#include <vector>

namespace Game {

    // Client side of an attached rocket (see the header comment).
    struct FireworkAttachState {
        glm::dvec3 position{0.0};
        glm::dvec3 deltaMovement{0.0};      // blocks per tick
        glm::dvec3 lookAngle{0.0, 0.0, 1.0};
        glm::dvec3 handAngle{0.0};          // Entity.getHandHoldingItemAngle(FIREWORK_ROCKET)
        bool       fallFlying = false;
    };

    class FireworkAttachHost {
    public:
        virtual ~FireworkAttachHost() = default;
        // False when the id names nothing the client can place.
        virtual bool Query(int32_t entityId, FireworkAttachState& out) = 0;
        // MC attachedToEntity.setDeltaMovement(...) — only meaningful for the
        // local player, whose movement this client owns.
        virtual void SetDeltaMovement(int32_t entityId, const glm::dvec3& deltaMovement) = 0;
    };

    class FireworkRocket : public Projectile {
    public:
        explicit FireworkRocket(EntityLevel* level);

        // MC FireworkRocketEntity(level, x, y, z, sourceItemStack): the copy
        // of the stack, a tiny random lateral drift and 0.05 up, and the
        // lifetime 10 * (flightDuration + 1) + nextInt(6) + nextInt(7).
        void InitLaunch(const glm::dvec3& pos, const ItemStack& sourceItemStack);
        // MC FireworkRocketEntity(level, stack, stuckTo): launched at, owned
        // by and attached to `stuckTo`.
        void AttachTo(LivingEntity& stuckTo);
        // MC DATA_SHOT_AT_ANGLE — a crossbow's or a dispenser's rocket: no
        // climb, no acceleration, drawn edge-on along its flight.
        void SetShotAtAngle(bool shotAtAngle) { m_shotAtAngle = shotAtAngle; m_dataDirty = true; }
        bool IsShotAtAngle() const { return m_shotAtAngle; }

        void Tick() override;
        void HandleEntityEvent(uint8_t id) override;

        // MC isAttackable() false; not damageable (Projectile's Hurt); and,
        // as an Entity that does not override isPickable, never under the
        // crosshair.
        bool IsAttackable() const override { return false; }
        bool IsPickable() const override { return false; }

        // MC shouldRender / shouldRenderAtSqrDistance: never while attached
        // (the rocket sits in the glider's hand), else within 64 blocks.
        bool ShouldRender() const { return m_attachedToId < 0; }

        // MC getItem — DATA_ID_FIREWORKS_ITEM.
        const ItemStack& GetItem() const { return m_item; }
        void SetItem(const ItemStack& stack);
        // MC getExplosions: the item's FIREWORKS explosions (empty for none).
        std::vector<FireworkExplosion> GetExplosions() const;
        bool HasExplosion() const;

        // MC DATA_ATTACHED_TO_TARGET: the entity id riding the rocket, or -1.
        int32_t GetAttachedToId() const { return m_attachedToId; }
        void    SetAttachedToId(int32_t id);
        bool    IsAttachedToEntity() const { return m_attachedToId >= 0; }

        // Save/load (MC Life / LifeTime / FireworksItem / ShotAtAngle).
        int  GetLife() const { return m_life; }
        void SetLife(int life) { m_life = life; }
        int  GetLifetime() const { return m_lifetime; }
        void SetLifetime(int lifetime) { m_lifetime = lifetime; }

        // The tracker's send-on-change latch for FireworkRocketDataS2C.
        bool ConsumeDataDirty() {
            const bool dirty = m_dataDirty;
            m_dataDirty = false;
            return dirty;
        }

        void ClearReferenceTo(const Entity* entity) override;

        // The client's attachment adapter (see FireworkAttachHost). Null on a
        // dedicated server; set once by the client at startup.
        static void SetClientAttachHost(FireworkAttachHost* host);

    protected:
        // MC Projectile.canHitEntity: target.canBeHitByProjectile() (its
        // isPickable — never another projectile) and the owner rules.
        bool CanHitEntity(const Entity& entity) const override;
        void OnHitEntity(LivingEntity& target, const HitResult& hit) override;
        void OnHit(const HitResult& hit) override;

    private:
        // MC explode(ServerLevel): event 17, GameEvent.EXPLODE, the blast
        // damage, discard.
        void Explode();
        // MC dealExplosionDamage.
        void DealExplosionDamage();
        // The attached entity this tick, whichever side resolves it.
        bool QueryAttached(FireworkAttachState& state);

        ItemStack     m_item;
        int32_t       m_attachedToId = -1;
        // Server: the attached entity (MC attachedToEntity). Cleared if it
        // leaves the level; the rocket then stays put until it goes off.
        LivingEntity* m_attached = nullptr;
        bool          m_shotAtAngle = false;
        int           m_life = 0;
        int           m_lifetime = 0;
        bool          m_dataDirty = true;
    };

} // namespace Game
