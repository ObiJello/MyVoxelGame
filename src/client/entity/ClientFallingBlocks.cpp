// File: src/client/entity/ClientFallingBlocks.cpp
#include "client/entity/ClientFallingBlocks.hpp"

#include "common/core/Profiling_Tracy.hpp"
#include "common/core/TickParallel.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>

namespace Client {

    std::unique_ptr<ClientFallingBlocks> g_clientFallingBlocks;

    namespace {
        glm::vec3 Half() {
            const auto& t = Game::GetEntityTypeInfo(Game::EntityTypeId::FallingBlock);
            return glm::vec3(t.width * 0.5f, t.height * 0.5f, t.width * 0.5f);
        }
    }

    void ClientFallingBlocks::FillProxy(size_t i) {
        BlockEntityProxy& p = m_proxies[i];
        p.prevPos  = m_prevPos[i];
        p.pos      = m_pos[i];
        p.half     = Half();
        p.stateRaw = m_state[i];
        p.fuse     = 0;
        p.type     = static_cast<uint8_t>(Game::EntityTypeId::FallingBlock);
        p.onGround = m_onGround[i];
        p.drawable = 1;
    }

    void ClientFallingBlocks::Spawn(int32_t id, const glm::dvec3& pos, const glm::vec3& vel,
                                    uint32_t stateRaw) {
        const auto it = m_index.find(id);
        if (it != m_index.end()) {
            // Re-announced (the server re-sent an add for an id we hold):
            // reset it in place, exactly as ClientMobManager::Spawn does.
            const size_t i = it->second;
            m_pos[i] = pos; m_prevPos[i] = pos; m_vel[i] = glm::dvec3(vel);
            m_codecBase[i] = pos; m_target[i] = pos; m_interpSteps[i] = 0;
            m_state[i] = stateRaw; m_onGround[i] = 0;
            FillProxy(i);
            return;
        }
        const size_t i = m_id.size();
        m_index.emplace(id, i);
        m_id.push_back(id);
        m_pos.push_back(pos);
        m_prevPos.push_back(pos);
        m_vel.push_back(glm::dvec3(vel));
        m_codecBase.push_back(pos);
        m_target.push_back(pos);
        m_interpSteps.push_back(0);
        m_state.push_back(stateRaw);
        m_onGround.push_back(0);
        m_proxies.emplace_back();
        FillProxy(i);
    }

    void ClientFallingBlocks::MoveDelta(int32_t id, bool hasPos, const glm::dvec3& delta,
                                        bool onGround) {
        const auto it = m_index.find(id);
        if (it == m_index.end()) return;
        const size_t i = it->second;
        if (hasPos) {
            // Applied to the CODEC BASE, not the simulated position — see
            // ClientMobManager::GetCodecBase.
            const glm::dvec3 target = m_codecBase[i] + delta;
            m_codecBase[i] = target;
            const glm::dvec3 d = target - m_pos[i];
            if (glm::dot(d, d) > ClientMobManager::kSnapDistanceSq) {
                m_pos[i] = target;
                m_interpSteps[i] = 0;
            } else {
                m_target[i] = target;
                m_interpSteps[i] = ClientMobManager::kInterpSteps;
            }
        }
        m_onGround[i] = onGround ? 1 : 0;
    }

    void ClientFallingBlocks::Teleport(int32_t id, const glm::dvec3& pos, const glm::vec3& vel,
                                       bool onGround) {
        const auto it = m_index.find(id);
        if (it == m_index.end()) return;
        const size_t i = it->second;
        m_codecBase[i] = pos;
        const glm::dvec3 d = pos - m_pos[i];
        if (glm::dot(d, d) > ClientMobManager::kSnapDistanceSq) {
            m_pos[i] = pos;
            m_interpSteps[i] = 0;
        } else {
            m_target[i] = pos;
            m_interpSteps[i] = ClientMobManager::kInterpSteps;
        }
        m_vel[i] = glm::dvec3(vel);
        m_onGround[i] = onGround ? 1 : 0;
    }

    void ClientFallingBlocks::SetMotion(int32_t id, const glm::vec3& vel) {
        const auto it = m_index.find(id);
        if (it == m_index.end()) return;
        m_vel[it->second] = glm::dvec3(vel);
    }

    void ClientFallingBlocks::Remove(int32_t id) {
        const auto it = m_index.find(id);
        if (it == m_index.end()) return;
        const size_t i = it->second;
        const size_t last = m_id.size() - 1;
        if (i != last) {
            m_id[i] = m_id[last]; m_pos[i] = m_pos[last]; m_prevPos[i] = m_prevPos[last];
            m_vel[i] = m_vel[last]; m_codecBase[i] = m_codecBase[last];
            m_target[i] = m_target[last]; m_interpSteps[i] = m_interpSteps[last];
            m_state[i] = m_state[last]; m_onGround[i] = m_onGround[last];
            m_proxies[i] = m_proxies[last];
            m_index[m_id[i]] = i;
        }
        m_id.pop_back(); m_pos.pop_back(); m_prevPos.pop_back(); m_vel.pop_back();
        m_codecBase.pop_back(); m_target.pop_back(); m_interpSteps.pop_back();
        m_state.pop_back(); m_onGround.pop_back(); m_proxies.pop_back();
        m_index.erase(it);
    }

    void ClientFallingBlocks::Clear() {
        m_id.clear(); m_pos.clear(); m_prevPos.clear(); m_vel.clear(); m_codecBase.clear();
        m_target.clear(); m_interpSteps.clear(); m_state.clear(); m_onGround.clear();
        m_proxies.clear(); m_index.clear();
    }

    void ClientFallingBlocks::Tick() {
        const size_t n = m_id.size();
        if (n == 0) return;
        PROFILE_ZONE_N("ClientFalling.Tick");
        PROFILE_PLOT("ClientFalling/Count", static_cast<int64_t>(n));

        Game::PhysicsContext ctx;
        ctx.blockAccess = m_blocks;
        const glm::vec3 half = Half();

        // Per row, in the order ClientMobManager's parallel pass uses for a
        // falling block: render-prev snapshot, correction toward the server's
        // last position, then MC's client tick — gravity, the approximate
        // mover, drag ALWAYS last.
        const auto body = [&](size_t i) {
            m_prevPos[i] = m_pos[i];
            if (m_interpSteps[i] > 0) {
                const double alpha = 1.0 / static_cast<double>(m_interpSteps[i]);
                m_pos[i] += (m_target[i] - m_pos[i]) * alpha;
                --m_interpSteps[i];
            }
            glm::dvec3& vel = m_vel[i];
            vel.y -= Game::FallingBlockEntity::kGravity;
            bool onGround = m_onGround[i] != 0;
            bool hc = false, vc = false;
            if (m_blocks) {
                // No open-sky shortcut here, deliberately: for a one-cell box
                // the two overlap tests below read 1-2 cells, which is
                // CHEAPER than the section-flag walk IsRegionAllAir does
                // (measured: 12.7 -> 18.8 ms a tick at 1.37M with the
                // shortcut). The server store's shortcut replaces the full
                // swept mover, a different trade.
                Game::MoveApproximate(m_pos[i], vel, half, vel, onGround, hc, vc, ctx);
            }
            m_onGround[i] = onGround ? 1 : 0;
            vel *= Game::FallingBlockEntity::kAirDrag;
            FillProxy(i);
        };
        if (n >= 2048 && Core::ParallelWidth() > 1) {
            Core::ParallelFor(n, 256, body);
        } else {
            for (size_t i = 0; i < n; ++i) body(i);
        }
    }

} // namespace Client
