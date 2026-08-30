// File: src/common/entity/PrimedTnt.hpp
//
// MC net.minecraft.world.entity.item.PrimedTnt — lit TNT, mid-fuse.
//
// Same architecture note as FallingBlockEntity next door: MC models this as a
// plain Entity, this engine derives Game::Mob with the mob machinery inert,
// because the tracking / wire / NBT / client-factory pipelines are Mob-shaped.
//
// TICK ORDER DIFFERS FROM FallingBlockEntity, and the difference is vanilla's,
// not an oversight:
//
//     FallingBlockEntity:  move -> [land damp] -> velocity *= 0.98
//     PrimedTnt:           move -> velocity *= 0.98 -> [ground damp]
//
// So a landing falling block keeps 0.98 of its already-damped motion while
// landing TNT damps motion that has already been dragged. The visible effect
// is that TNT settles fractionally faster. Do not "unify" these into a shared
// helper — the asymmetry is the parity.
//
// The fuse counts down AFTER the movement, and the client-side smoke plume
// only appears on ticks the fuse survives — so the last tick before detonation
// shows no smoke, which is also vanilla.
#pragma once

#include "common/core/EntityRef.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/block/BlockState.hpp"

namespace Game {

    class PrimedTnt : public Mob {
    public:
        explicit PrimedTnt(EntityLevel* level);

        // ── MC constants (PrimedTnt.java) ──────────────────────────────────
        static constexpr int    kDefaultFuse      = 80;      // DEFAULT_FUSE_TIME
        static constexpr float  kDefaultPower     = 4.0f;    // DEFAULT_EXPLOSION_POWER
        static constexpr float  kMaxPower         = 128.0f;  // the load-time clamp
        static constexpr double kGravity          = 0.04;
        static constexpr double kAirDrag           = 0.98;
        static constexpr double kLandHorizontal    = 0.7;
        static constexpr double kLandVertical      = -0.5;
        // The spawn hop: a random horizontal nudge of this magnitude plus a
        // fixed upward kick. It is what makes a stack of primed TNT scatter
        // instead of stacking into a column.
        static constexpr double kSpawnHorizontal   = 0.02;
        static constexpr double kSpawnVertical     = 0.2;
        // MC explodes at getY(0.0625) — one sixteenth of the way up the
        // entity's own height, NOT at its centre.
        static constexpr double kExplodeHeightFrac = 0.0625;

        // MC PrimedTnt(level, x, y, z, owner).
        void InitPrimed(const glm::dvec3& pos, Entity* owner);

        void Tick() override;

        // ── Mob machinery, switched off ────────────────────────────────────
        double GetGravity() const override { return kGravity; }
        // MC hurtServer returns false outright — you cannot punch out a fuse.
        bool Hurt(MobDamageSource, float, Entity*) override { return false; }
        bool ExplosionPushOnly() const override { return true; }
        bool IsPushable() const override { return false; }
        // MC PrimedTnt.isPickable returns !isRemoved() — vanilla lets a lit
        // TNT swallow your crosshair. This deliberately does not, so you can
        // place a block through it or light the next TNT in a row behind it
        // without waiting out the fuse. See Entity::IsPickable.
        bool IsPickable() const override { return false; }
        void CheckDespawn() override {}                    // the fuse is the timer
        bool RemoveWhenFarAway(double) const override { return false; }
        // MC EntityType.TNT is registered .fireImmune().
        bool FireImmune() const override { return true; }

        // ── State ──────────────────────────────────────────────────────────
        int  GetFuse() const { return m_fuse; }
        void SetFuse(int ticks) { m_fuse = ticks; }

        float ExplosionPower() const { return m_explosionPower; }
        void  SetExplosionPower(float p);

        // MC PrimedTnt.usedPortal, set by PrimedTnt.teleport. A TNT that rode
        // a portal explodes with USED_PORTAL_DAMAGE_CALCULATOR, which spares
        // NETHER_PORTAL blocks — otherwise the first TNT through a portal
        // takes the portal out and strands whatever was behind it.
        //
        // Nothing sets it yet: Server::PortalTravel only moves PLAYERS between
        // dimensions (see its non-player branch). This is the one line that
        // needs adding there the day entity travel lands, and it is spelled
        // out here so it is not rediscovered as a bug then.
        bool UsedPortal() const { return m_usedPortal; }
        void SetUsedPortal(bool v) { m_usedPortal = v; }

        // MC's synched block_state — which block this TNT is rendered as. Only
        // ever `tnt` today, but it is a real field in vanilla (a datapack can
        // prime something else) and it costs nothing to carry.
        BlockState CarriedState() const { return m_blockState; }
        void       SetCarriedState(BlockState s) { m_blockState = s; }

        // MC's owner reference — who gets the kill credit. Held as a UUID so it
        // survives a save and an offline owner, exactly like Projectile's.
        void       SetOwner(Entity* owner) { m_ownerRef.Set(owner); }
        void       SetOwnerUuid(const Uuid& uuid) { m_ownerRef.SetUnresolved(uuid); }
        const EntityRef& OwnerRef() const { return m_ownerRef; }
        Entity*    GetOwner();

        // The fuse rides the existing per-type animation byte rather than a new
        // wire field: it fits in one byte (80 ticks), `animState` is already
        // documented as meaning whatever the entity class says it means, and
        // AddEntityS2C already carries it. The client counts down locally from
        // there, as MC's own client does, so no per-tick resend is needed.
        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(m_fuse < 0 ? 0 : (m_fuse > 255 ? 255 : m_fuse));
        }
        void SetAnimStateByte(uint8_t v) override { m_fuse = static_cast<int>(v); }
        bool AnimStateTicksOnClient() const override { return true; }

    private:
        void Explode();
        void TickFuseTail();
        uint64_t RestRegionStamp() const;

        int        m_fuse = kDefaultFuse;
        float      m_explosionPower = kDefaultPower;
        bool       m_usedPortal      = false;   // not persisted; MC does not save it
        BlockState m_blockState{};
        EntityRef  m_ownerRef;
    };

} // namespace Game
