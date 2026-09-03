// File: src/common/entity/EndCrystal.hpp
//
// MC net.minecraft.world.entity.boss.enderdragon.EndCrystal — the pillar
// crystal the dragon heals from, and the respawn-ritual crystal.
//
// Same architecture note as PrimedTnt / FallingBlockEntity: MC models this as
// a plain Entity, this engine derives Game::Mob with the mob machinery inert,
// because the tracking / wire / NBT / client-factory pipelines are Mob-shaped.
// A crystal never moves at all (MC updateInterval is Integer.MAX_VALUE), so
// Tick() replaces Mob::Tick wholesale — no gravity, no mover.
//
// Wire: `showBottom` rides the variant byte (static after spawn, like the
// sheep's wool byte). The BEAM TARGET — which changes at runtime during the
// respawn ritual — rides its own EndCrystalBeamS2C packet, sent by
// ServerEntityTracker on tracking start and whenever SetBeamTarget flips the
// dirty flag (MC syncs it as the DATA_BEAM_TARGET entity-data entry).
#pragma once

#include "common/entity/Mob.hpp"

namespace Game {

    class EndCrystal : public Mob {
    public:
        explicit EndCrystal(EntityLevel* level);

        // MC EndCrystal.time — the client bob/rotation clock, seeded random
        // so a row of crystals doesn't bob in lockstep.
        int time = 0;

        void Tick() override;

        // MC hurtServer: any real hit destroys the crystal — explosion 6.0
        // unless the hit itself was one — and reports to the dragon fight.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC EndCrystal.isPickable() is true — a crystal swallows crosshair
        // and arrow alike (that is how the fight is played).
        bool IsPickable() const override { return true; }
        bool IsPushable() const override { return false; }
        bool ExplosionPushOnly() const override { return true; }
        // MC EntityType.END_CRYSTAL is fireImmune.
        bool FireImmune() const override { return true; }
        void CheckDespawn() override {}
        bool RemoveWhenFarAway(double) const override { return false; }

        // ── Beam target (MC DATA_BEAM_TARGET) ─────────────────────────────
        bool HasBeamTarget() const { return m_hasBeamTarget; }
        const glm::ivec3& BeamTarget() const { return m_beamTarget; }
        void SetBeamTarget(const glm::ivec3& target);
        void ClearBeamTarget();
        // The tracker's send-on-change latch. Read-and-clear.
        bool ConsumeBeamDirty() {
            const bool was = m_beamDirty;
            m_beamDirty = false;
            return was;
        }

        // ── Show bottom (MC DATA_SHOW_BOTTOM) — the bedrock base slab ─────
        // True for worldgen/ritual crystals; the item places with false.
        bool ShowsBottom() const { return m_showBottom; }
        void SetShowBottom(bool v) { m_showBottom = v; }
        uint8_t GetVariantByte() const override { return m_showBottom ? 1 : 0; }
        void    SetVariantByte(uint8_t v) override { m_showBottom = (v & 1) != 0; }

    private:
        void OnDestroyedBy(Entity* attacker);

        glm::ivec3 m_beamTarget{0};
        bool m_hasBeamTarget = false;
        bool m_beamDirty = false;
        bool m_showBottom = true;   // MC DEFAULT_SHOW_BOTTOM
    };

} // namespace Game
