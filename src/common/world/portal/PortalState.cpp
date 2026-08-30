// File: src/common/world/portal/PortalState.cpp
//
// Line references are to minecraft_code/decompiled_net/minecraft/world/entity/
// PortalProcessor.java and Entity.java.

#include "PortalState.hpp"

#include <algorithm>

namespace Game {

    // Entity.java:2586
    void PortalState::SetAsInsidePortal(BlockID portal, const glm::ivec3& pos,
                                        int cooldownTicks) {
        if (IsOnCooldown()) {
            // Re-arm rather than accumulate. See the header — this is the rule
            // that stops a portal bouncing you back the instant you arrive.
            SetCooldown(cooldownTicks);
            return;
        }

        // MC compares the Portal OBJECT, which is the block singleton — so
        // "same portal" means "same KIND of portal", not "same portal
        // structure". Walking out of one nether portal and into another keeps
        // the accumulated time. Matching that here means comparing block ids.
        if (m_process && m_process->portal == portal) {
            if (!m_process->insidePortalThisTick) {
                m_process->entryPosition        = pos;
                m_process->insidePortalThisTick = true;
            }
            return;
        }

        m_process = Process{ portal, pos, 0, true };
    }

    // Entity.java:2602 + PortalProcessor.java:21
    std::optional<PortalState::Trigger> PortalState::HandleTick(
        int transitionTime, bool allowedToTeleport, int cooldownOnFire)
    {
        // Entity.java:623 processPortalCooldown.
        if (m_cooldown > 0) --m_cooldown;

        if (!m_process) return std::nullopt;

        bool fired = false;
        if (!m_process->insidePortalThisTick) {
            // PortalProcessor.decayTick — four ticks off for every tick spent
            // outside. Stepping out of a portal for one tick costs you four,
            // so brushing past the edge never accumulates a crossing, but
            // briefly clipping out of a portal you are walking through does
            // not reset your progress either.
            m_process->portalTime = std::max(m_process->portalTime - 4, 0);
        } else {
            m_process->insidePortalThisTick = false;
            // Post-increment: with a transition time of 80 this fires on the
            // 81st tick inside. Off-by-one here is a visible 50 ms difference
            // from vanilla in a speedrun context, so keep the order.
            fired = allowedToTeleport && (m_process->portalTime++ >= transitionTime);
        }

        if (fired) {
            // MC arms the cooldown BEFORE resolving the destination
            // (Entity.java:2610), so a destination lookup that fails still
            // leaves the entity on cooldown rather than retrying every tick.
            SetCooldown(cooldownOnFire);
            return Trigger{ m_process->portal, m_process->entryPosition };
        }

        // Entity.java:2620 — an expired processor is dropped so the next
        // portal entered starts a fresh timer.
        if (m_process->portalTime <= 0) {
            m_process.reset();
        }
        return std::nullopt;
    }

} // namespace Game
