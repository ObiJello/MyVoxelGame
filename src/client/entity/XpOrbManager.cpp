// File: src/client/entity/XpOrbManager.cpp
#include "XpOrbManager.hpp"
#include "../world/ClientBlockAccess.hpp"
#include "RemotePlayerManager.hpp"

#include <algorithm>
#include <cmath>

namespace Client {

    XpOrbManager* g_xpOrbManager = nullptr;

    void XpOrbManager::Spawn(int32_t id, const glm::dvec3& pos,
                             const glm::vec3& vel, int32_t value) {
        auto& e = m_entities[id];
        const bool isNew = !e.initialized;

        e.sim.id    = id;
        e.sim.value = value;
        e.sim.vel   = glm::dvec3(vel);

        if (isNew) {
            e.sim.pos            = pos;
            e.renderPrevPosition = pos;
            e.prevTickPos        = pos;
            e.interpSteps        = 0;
            e.ageTicks           = 0.0f;
            e.initialized        = true;
        } else {
            InterpolateTo(e, pos);
        }
    }

    void XpOrbManager::Move(int32_t id, const glm::dvec3& pos,
                            const glm::vec3& vel) {
        auto it = m_entities.find(id);
        if (it == m_entities.end()) {
            // Missed or out-of-order spawn; the periodic full refresh will
            // introduce the orb properly within a second.
            return;
        }
        auto& e = it->second;
        e.sim.vel = glm::dvec3(vel);
        InterpolateTo(e, pos);
    }

    void XpOrbManager::InterpolateTo(ClientXpOrb& e, const glm::dvec3& target) {
        const glm::dvec3 err = target - e.sim.pos;
        if (glm::dot(err, err) > kSnapDistanceSq) {
            e.sim.pos            = target;
            e.renderPrevPosition = target;
            e.prevTickPos        = target;
            e.interpSteps        = 0;
            return;
        }
        e.interpSteps  = kInterpSteps;
        e.interpTarget = target;
        e.prevTickPos  = e.sim.pos;
    }

    void XpOrbManager::TakeOrb(int32_t orbId, uint32_t playerId) {
        auto it = m_entities.find(orbId);
        if (it == m_entities.end()) return;
        auto& e = it->second;

        XpOrbPickupAnim anim;
        anim.value          = e.sim.value;
        anim.ageTicks       = e.ageTicks;
        anim.startPos       = e.sim.pos;
        anim.targetPlayerId = playerId;
        anim.life           = 0;
        m_pickups.push_back(anim);

        // MC removes the orb outright on its take packet — a merged orb's
        // remaining counts drain invisibly server-side.
        m_entities.erase(it);
    }

    glm::dvec3 XpOrbManager::ResolveTarget(uint32_t playerId,
                                           const glm::dvec3& localPlayerPos) const {
        glm::dvec3 feet = localPlayerPos;
        if (g_remotePlayerManager) {
            const auto& players = g_remotePlayerManager->GetPlayers();
            auto it = players.find(playerId);
            if (it != players.end()) {
                feet = glm::dvec3(it->second.position);
            }
        }
        // Chest height — midpoint of feet and eyes, like the item pickup.
        feet.y += Game::PlayerPhysics::EYE_HEIGHT_STANDING * 0.5;
        return feet;
    }

    void XpOrbManager::Tick(const glm::dvec3& localPlayerPos) {
        // Pickup animations outlive their orb.
        for (auto& p : m_pickups) {
            ++p.life;
            const glm::dvec3 t = ResolveTarget(p.targetPlayerId, localPlayerPos);
            if (!p.targetSeeded) {
                p.targetPos    = t;
                p.targetPosOld = t;
                p.targetSeeded = true;
            } else {
                p.targetPosOld = p.targetPos;
                p.targetPos    = t;
            }
        }
        m_pickups.erase(
            std::remove_if(m_pickups.begin(), m_pickups.end(),
                           [](const XpOrbPickupAnim& p) { return p.life >= kPickupLifeTicks; }),
            m_pickups.end());

        if (m_entities.empty()) return;

        Game::PhysicsContext ctx;
        ctx.blockAccess = g_clientBlockAccess;

        constexpr double kFollowDistSq = Game::ExperienceOrb::kMaxFollowDist
                                       * Game::ExperienceOrb::kMaxFollowDist;

        for (auto& [id, e] : m_entities) {
            e.renderPrevPosition = e.sim.pos;

            const int cx = static_cast<int>(std::floor(e.sim.pos.x / 16.0));
            const int cz = static_cast<int>(std::floor(e.sim.pos.z / 16.0));
            if (ctx.blockAccess && ctx.IsChunkLoaded(cx, cz)) {
                // followNearbyPlayer, client edition: candidates are the
                // local player plus every remote player. Sticky like MC —
                // keep the current target until it leaves the 8-block range.
                const auto feetOf = [&](int64_t pid, glm::dvec3& out) -> bool {
                    if (pid == kLocalPlayerFollowId) {
                        out = localPlayerPos;
                        return true;
                    }
                    if (pid >= 0 && g_remotePlayerManager) {
                        const auto& players = g_remotePlayerManager->GetPlayers();
                        auto it = players.find(static_cast<uint32_t>(pid));
                        if (it != players.end()) {
                            out = glm::dvec3(it->second.position);
                            return true;
                        }
                    }
                    return false;
                };

                glm::dvec3 feet(0.0);
                bool following = feetOf(e.sim.followingPlayerId, feet);
                if (following) {
                    const glm::dvec3 d = feet - e.sim.pos;
                    if (glm::dot(d, d) > kFollowDistSq) following = false;
                }
                if (!following) {
                    e.sim.followingPlayerId = -1;
                    double bestSq = kFollowDistSq;
                    const glm::dvec3 dl = localPlayerPos - e.sim.pos;
                    if (glm::dot(dl, dl) <= bestSq) {
                        bestSq = glm::dot(dl, dl);
                        e.sim.followingPlayerId = kLocalPlayerFollowId;
                        feet = localPlayerPos;
                        following = true;
                    }
                    if (g_remotePlayerManager) {
                        for (const auto& [pid, rp] : g_remotePlayerManager->GetPlayers()) {
                            const glm::dvec3 dr = glm::dvec3(rp.position) - e.sim.pos;
                            const double distSq = glm::dot(dr, dr);
                            if (distSq <= bestSq) {
                                bestSq = distSq;
                                e.sim.followingPlayerId = static_cast<int64_t>(pid);
                                feet = glm::dvec3(rp.position);
                                following = true;
                            }
                        }
                    }
                }

                glm::dvec3 target(0.0);
                if (following) {
                    target = feet + glm::dvec3(
                        0.0, Game::PlayerPhysics::EYE_HEIGHT_STANDING * 0.5, 0.0);
                }

                e.sim.TickMovement(ctx, following ? &target : nullptr,
                                   m_random, /*isServer=*/false);
            }

            // Pending server correction (MC InterpolationHandler) — the
            // target advances by the local sim's own delta so a correct
            // prediction converges to a no-op.
            if (e.interpSteps > 0) {
                const glm::dvec3 simDelta = e.sim.pos - e.prevTickPos;
                e.interpTarget += simDelta;

                const double alpha = 1.0 / static_cast<double>(e.interpSteps);
                e.sim.pos = glm::mix(e.sim.pos, e.interpTarget, alpha);
                e.interpSteps--;
                e.prevTickPos = e.sim.pos;
            }

            e.ageTicks += 1.0f;
        }
    }

} // namespace Client
