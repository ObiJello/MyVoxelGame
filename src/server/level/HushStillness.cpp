// File: src/server/level/HushStillness.cpp
#include "server/level/HushStillness.hpp"

#include "common/core/Log.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/HushStillnessS2CPacket.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include <algorithm>
#include <memory>
#include <random>
#include <unordered_set>
#include <vector>

namespace Server {

    HushStillness::HushStillness(ServerLevel& level, PlayerSessionManager* sessions)
        : m_level(level), m_sessions(sessions), m_rng(std::random_device{}()) {
        // The first one comes a full interval after the level starts
        // ticking: arriving in the Hush is not the moment for it.
        m_ticksUntilNext = RollInterval();
    }

    int HushStillness::RollInterval() {
        return std::uniform_int_distribution<int>(kMinIntervalTicks, kMaxIntervalTicks)(m_rng);
    }

    int HushStillness::RollDuration() {
        return std::uniform_int_distribution<int>(kMinDurationTicks, kMaxDurationTicks)(m_rng);
    }

    void HushStillness::Tick() {
        if (m_remainingTicks > 0) {
            if (--m_remainingTicks == 0) {
                m_ticksUntilNext = RollInterval();
                Log::Info("[Hush] The stillness lifts (next in %d s)", m_ticksUntilNext / 20);
            }
        } else if (--m_ticksUntilNext <= 0) {
            Start();
        }
        ApplyToLevel();
        SyncPlayers();
    }

    void HushStillness::Start(int durationTicks) {
        m_remainingTicks = durationTicks > 0
            ? std::min(durationTicks, kMaxCommandDurationTicks)
            : RollDuration();
        Log::Info("[Hush] A stillness falls (%.1f s)", m_remainingTicks / 20.0);
        // A restart must reach players already told: their client re-reads
        // the remaining time as its new safety deadline.
        for (uint32_t connectionId : m_told) Send(connectionId, true);
        ApplyToLevel();
    }

    void HushStillness::Stop() {
        if (m_remainingTicks <= 0) return;
        m_remainingTicks = 0;
        m_ticksUntilNext = RollInterval();
        Log::Info("[Hush] The stillness is lifted (next in %d s)", m_ticksUntilNext / 20);
        ApplyToLevel();
        SyncPlayers();
    }

    void HushStillness::ApplyToLevel() {
        if (ServerLevelBridge* bridge = m_level.MobLevel()) bridge->SetStilled(Active());
    }

    void HushStillness::SyncPlayers() {
        if (!m_sessions) return;
        // Who is in this level right now.
        std::unordered_set<uint32_t> present;
        const int dimension = static_cast<int>(m_level.Dimension());
        for (const auto& session : m_sessions->GetAllSessions()) {
            if (session && session->GetDimensionId() == dimension) {
                present.insert(session->GetConnectionId());
            }
        }

        // Told "on", but it lifted or they left: tell them it lifted.
        // (Send tolerates a session that is already gone.)
        for (auto it = m_told.begin(); it != m_told.end();) {
            if (!Active() || present.count(*it) == 0) {
                Send(*it, false);
                it = m_told.erase(it);
            } else {
                ++it;
            }
        }
        // On, and not yet told: a start this tick, or an arrival mid-way.
        if (Active()) {
            for (uint32_t connectionId : present) {
                if (m_told.insert(connectionId).second) Send(connectionId, true);
            }
        }
    }

    void HushStillness::RemoveAll() {
        for (uint32_t connectionId : m_told) Send(connectionId, false);
        m_told.clear();
        m_remainingTicks = 0;
        ApplyToLevel();
    }

    void HushStillness::Send(uint32_t connectionId, bool active) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::HushStillnessS2CPacket p;
        p.active         = active;
        p.remainingTicks = active ? static_cast<uint32_t>(m_remainingTicks) : 0u;
        session->GetConnection()->SendPacket(
            static_cast<uint8_t>(Network::PacketId::HushStillnessS2C),
            Network::Serialization::Serialize(p));
    }

} // namespace Server
