// File: src/common/entity/projectile/EvokerFangs.cpp
#include "common/entity/projectile/EvokerFangs.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/core/Mth.hpp"

#include <vector>

namespace Game {

    void EvokerFangs::Init(double x, double y, double z, float rotationRadians,
                           int warmupDelayTicks, LivingEntity* owner) {
        m_warmupDelayTicks = warmupDelayTicks;
        SetOwner(owner);
        yRot = yRotO = rotationRadians * Mth::kRadToDeg;
        yBodyRot = yHeadRot = yRot;
        position = glm::dvec3(x, y, z);
    }

    void EvokerFangs::Tick() {
        if (!m_level) return;
        Entity::BaseTick();   // MC super.tick() runs first

        if (m_level->IsClientSide()) {
            // MC's client half counts lifeTicks down once the attack starts
            // (the 12 CRIT particles at lifeTicks == 14 need the particle
            // system). The countdown drives GetAnimationProgress.
            if (m_clientSideAttackStarted) {
                --m_lifeTicks;
            }
            return;
        }

        if (--m_warmupDelayTicks < 0) {
            // MC: the bite lands exactly 8 ticks after the warmup expires.
            if (m_warmupDelayTicks == -8) {
                AABB box = GetAABB();
                box.min -= glm::vec3(0.2f, 0.0f, 0.2f);
                box.max += glm::vec3(0.2f, 0.0f, 0.2f);
                std::vector<Entity*> nearby;
                m_level->GetEntitiesInBox(box, this, nearby);
                for (Entity* e : nearby) {
                    if (auto* living = dynamic_cast<LivingEntity*>(e)) {
                        DealDamageTo(*living);
                    }
                }
            }

            if (!m_sentSpikeEvent) {
                m_level->BroadcastEntityEvent(*this, 4);
                m_sentSpikeEvent = true;
                // Mirrored locally so the anim byte serves late joiners.
                m_clientSideAttackStarted = true;
            }

            if (--m_lifeTicks < 0) {
                Discard();
            }
        }
    }

    void EvokerFangs::DealDamageTo(LivingEntity& target) {
        // MC dealDamageTo: alive, not invulnerable, not the owner, not the
        // owner's ally (no team system — the owner check is the live half),
        // 6.0 indirect magic.
        LivingEntity* owner = dynamic_cast<LivingEntity*>(GetOwner());
        if (!target.IsAlive() || &target == owner) return;
        // MC spares the owner's allies via isAlliedTo — scoreboard teams (no
        // team system here) plus the evoker's vex-ownership chain, which IS
        // live: a fang never bites its caster's own summons.
        if (const auto* vex = dynamic_cast<const Vex*>(&target)) {
            if (vex->GetVexOwner() == owner) return;
        }
        target.Hurt(MobDamageSource::Magic, 6.0f,
                    owner ? static_cast<Entity*>(owner) : this);
    }

    void EvokerFangs::HandleEntityEvent(uint8_t id) {
        if (id == 4) {
            m_clientSideAttackStarted = true;
            // MC plays EVOKER_FANGS_ATTACK here — no sound system.
        } else {
            Projectile::HandleEntityEvent(id);
        }
    }

    float EvokerFangs::GetAnimationProgress(float partialTick) const {
        // MC getAnimationProgress: 0 until the attack starts, then a 20-tick
        // ramp measured off the remaining life (LIFE_OFFSET 2).
        if (!m_clientSideAttackStarted) return 0.0f;
        const int remainingLife = m_lifeTicks - 2;
        return remainingLife <= 0
                   ? 1.0f
                   : 1.0f - (static_cast<float>(remainingLife) - partialTick) / 20.0f;
    }

} // namespace Game
