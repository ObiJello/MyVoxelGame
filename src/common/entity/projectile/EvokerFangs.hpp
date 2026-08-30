// File: src/common/entity/projectile/EvokerFangs.hpp
//
// MC net.minecraft.world.entity.projectile.EvokerFangs — the evoker's fang
// trap. A plain Entity in MC (it never moves); here it rides the projectile
// Misc pipeline the way the other non-mob entities do (see Projectile.hpp's
// architecture note), with all the mob machinery inert.
//
// Timeline, transcribed from EvokerFangs.tick():
//   warmupDelayTicks counts down (0 for the close rings, i for the line);
//   at warmup == -8 every living entity in the box inflated (0.2, 0, 0.2)
//   takes the 6-damage bite; the first negative-warmup tick broadcasts
//   entity event 4 (the client arms its snap animation and, in MC, the
//   attack sound); lifeTicks (22) then counts out and the fangs discard.
//
// Client sync: event 4 arms clientSideAttackStarted; the anim byte carries
// the same bit so a late-arriving client agrees. The fang MODEL is the
// skipped piece — no hand-written EvokerFangsModel exists in the model
// registry, so the entity is invisible until one lands (the bite, timing and
// sync are all in; reported for the coordinator).
#pragma once

#include "common/entity/projectile/Projectile.hpp"

namespace Game {

    class EvokerFangs : public Projectile {
    public:
        explicit EvokerFangs(EntityLevel* level)
            : Projectile(EntityTypeId::EvokerFangs, level) {}

        // MC EvokerFangs(level, x, y, z, rotationRadians, warmupDelayTicks,
        // owner).
        void Init(double x, double y, double z, float rotationRadians,
                  int warmupDelayTicks, LivingEntity* owner);

        void Tick() override;

        // MC handleEntityEvent 4 — the attack starts client-side.
        void HandleEntityEvent(uint8_t id) override;

        // MC EvokerFangsRenderer feeds getAnimationProgress to the model.
        float GetAnimationProgress(float partialTick) const;

        uint8_t GetAnimStateByte() const override {
            return m_clientSideAttackStarted ? 1 : 0;
        }
        void SetAnimStateByte(uint8_t v) override {
            m_clientSideAttackStarted = (v & 1) != 0;
        }

        // MC EvokerFangs.hurtServer returns false — fangs cannot be hit.
        // (Projectile's base Hurt already refuses.)

        // Save/load: MC writes only "Warmup" and "Owner" for fangs.
        // m_lifeTicks is deliberately NOT persisted — vanilla does not store
        // it either, so a saved fang restarts its 22-tick life on load.
        int  GetWarmupDelay() const { return m_warmupDelayTicks; }
        void SetWarmupDelay(int ticks) { m_warmupDelayTicks = ticks; }

    private:
        void DealDamageTo(LivingEntity& target);

        int  m_warmupDelayTicks = 0;
        int  m_lifeTicks = 22;
        bool m_sentSpikeEvent = false;
        bool m_clientSideAttackStarted = false;
    };

} // namespace Game
