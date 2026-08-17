// File: src/client/entity/ItemEntityManager.cpp
#include "ItemEntityManager.hpp"
#include "../world/ClientBlockAccess.hpp"
#include "RemotePlayerManager.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace Client {

    std::unique_ptr<ItemEntityManager> g_itemEntityManager = nullptr;

    void ItemEntityManager::Spawn(int32_t id, const glm::dvec3& pos,
                                  const glm::vec3& vel, float bobOffs,
                                  const Game::ItemStack& stack) {
        auto& e = m_entities[id];
        const bool isNew = !e.initialized;

        e.sim.id      = id;
        e.sim.stack   = stack;
        e.sim.bobOffs = bobOffs;
        e.sim.vel     = glm::dvec3(vel);

        if (isNew) {
            // Snap on first sight. Easing in from wherever the map default left
            // us would fly the item in from the origin.
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

    void ItemEntityManager::Move(int32_t id, const glm::dvec3& pos,
                                 const glm::vec3& vel, int32_t count) {
        auto it = m_entities.find(id);
        if (it == m_entities.end()) {
            // A move for an entity we never saw spawn — the spawn packet was
            // missed or arrived out of order. There is no stack to render
            // without it, so ignore; the server's periodic full refresh will
            // introduce the entity properly within a second.
            return;
        }
        auto& e = it->second;

        // Adopt the authoritative velocity so the local simulation continues
        // along the server's trajectory rather than its own guess (MC does this
        // via ClientboundSetEntityMotionPacket).
        e.sim.vel = glm::dvec3(vel);
        if (count > 0) e.sim.stack.count = count;

        InterpolateTo(e, pos);
    }

    void ItemEntityManager::InterpolateTo(ClientItemEntity& e, const glm::dvec3& target) {
        const glm::dvec3 err = target - e.sim.pos;
        if (glm::dot(err, err) > kSnapDistanceSq) {
            // Diverged too far to ease — see the header note.
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

    void ItemEntityManager::TakeItem(int32_t itemId, uint32_t playerId, int32_t amount) {
        auto it = m_entities.find(itemId);
        if (it == m_entities.end()) return;   // never saw it; nothing to animate
        auto& e = it->second;

        // Snapshot the flight. The stack carries only what was actually taken,
        // so a partial pickup flies the collected half and leaves the rest
        // lying there.
        ItemPickupAnim anim;
        anim.stack          = e.sim.stack;
        anim.stack.count    = std::max(1, amount);
        anim.bobOffs        = e.sim.bobOffs;
        anim.ageTicks       = e.ageTicks;
        anim.startPos       = e.sim.pos;
        anim.targetPlayerId = playerId;
        anim.life           = 0;
        // targetPos/targetPosOld are seeded from the collector on the first
        // Tick — TakeItem has no player position to resolve here.
        m_pickups.push_back(anim);

        e.sim.stack.count -= amount;
        if (e.sim.stack.count <= 0) {
            m_entities.erase(it);
        }
    }

    glm::dvec3 ItemEntityManager::ResolveTarget(uint32_t playerId,
                                                const glm::dvec3& localPlayerPos) const {
        glm::dvec3 feet = localPlayerPos;
        if (g_remotePlayerManager) {
            const auto& players = g_remotePlayerManager->GetPlayers();
            auto it = players.find(playerId);
            if (it != players.end()) {
                feet = glm::dvec3(it->second.position);
            }
            // A miss means the collector IS the local player (only remote
            // players are tracked here), so `feet` keeps its default. Same
            // fallback MC takes when the entity id doesn't resolve.
        }
        // MC: (getY() + getEyeY()) / 2 — the midpoint of feet and eyes.
        feet.y += Game::PlayerPhysics::EYE_HEIGHT_STANDING * 0.5;
        return feet;
    }

    void ItemEntityManager::Tick(const glm::dvec3& localPlayerPos) {
        // Pickup animations outlive their entity, so they tick even with no
        // entities left.
        for (auto& p : m_pickups) {
            ++p.life;
            // MC ItemPickupParticle.tick: save, then re-read the collector's
            // position. Keeping both lets the renderer interpolate the target
            // too, so the item tracks a moving player smoothly.
            const glm::dvec3 t = ResolveTarget(p.targetPlayerId, localPlayerPos);
            if (!p.targetSeeded) {
                // First sight of the collector — both slots start there, so
                // nothing interpolates from a stale point.
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
                           [](const ItemPickupAnim& p) { return p.life >= kPickupLifeTicks; }),
            m_pickups.end());

        if (m_entities.empty()) return;

        Game::PhysicsContext ctx;
        ctx.blockAccess = g_clientBlockAccess;

        for (auto& [id, e] : m_entities) {
            // Snapshot BEFORE anything moves — the renderer blends from here to
            // the post-tick position by partialTick.
            e.renderPrevPosition = e.sim.pos;

            // Local physics, same code the server runs. Skipped when the chunk
            // under the item hasn't streamed in yet: there is nothing to
            // collide against, and simulating would drop the item through the
            // floor until the next correction hauled it back.
            const int cx = static_cast<int>(std::floor(e.sim.pos.x / 16.0));
            const int cz = static_cast<int>(std::floor(e.sim.pos.z / 16.0));
            if (ctx.blockAccess && ctx.IsChunkLoaded(cx, cz)) {
                e.sim.TickMovement(ctx);
            }

            // Apply the pending server correction on top (MC
            // InterpolationHandler.interpolate).
            if (e.interpSteps > 0) {
                // Advance the target by however far the local simulation just
                // moved. This is the part that makes a correct prediction
                // converge to nothing instead of the correction dragging the
                // item backwards against its own physics every tick.
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
