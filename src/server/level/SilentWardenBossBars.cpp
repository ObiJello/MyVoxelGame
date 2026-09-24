// File: src/server/level/SilentWardenBossBars.cpp
#include "server/level/SilentWardenBossBars.hpp"

#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/Mob.hpp"
#include "common/network/PacketTypes.hpp"
#include "server/entity/MobManager.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace Server {

    SilentWardenBossBars::SilentWardenBossBars(ServerLevel& level,
                                               PlayerSessionManager* sessions)
        : m_level(level), m_sessions(sessions) {}

    void SilentWardenBossBars::Tick() {
        if (!m_sessions) return;
        MobManager* mobs = m_level.Mobs();
        if (!mobs) return;

        // The living bosses in this level. A warden mid-death (health 0,
        // playing TickDeath) is off the list: MC's ServerBossEvent removes
        // on die(), not on removal.
        std::vector<const Game::Mob*> wardens;
        for (const Game::Mob* mob : mobs->List()) {
            if (mob && (mob->GetType() == Game::EntityTypeId::SilentWarden ||
                        mob->GetType() == Game::EntityTypeId::ChoirMother ||
                        mob->GetType() == Game::EntityTypeId::TheUnsung)
                && !mob->IsRemoved() && mob->GetHealth() > 0.0f) {
                wardens.push_back(mob);
            }
        }

        // Who should see which bar this tick.
        std::unordered_map<uint32_t, Shown> wanted;
        if (!wardens.empty()) {
            const int dimension = static_cast<int>(m_level.Dimension());
            for (const auto& session : m_sessions->GetAllSessions()) {
                if (!session || session->GetDimensionId() != dimension) continue;
                ServerPlayer* player = session->GetPlayer();
                if (!player) continue;
                const glm::dvec3 p = player->getPosition();

                const Game::Mob* nearest = nullptr;
                double nearestSq = kRange * kRange;
                for (const Game::Mob* w : wardens) {
                    const double d = w->DistanceToSqr(p.x, p.y, p.z);
                    if (d <= nearestSq) { nearest = w; nearestSq = d; }
                }
                if (!nearest) continue;

                const float maxHealth = nearest->GetMaxHealth();
                const float progress = maxHealth > 0.0f
                    ? std::clamp(nearest->GetHealth() / maxHealth, 0.0f, 1.0f)
                    : 0.0f;
                wanted.emplace(session->GetConnectionId(),
                               Shown{ nearest->GetId(), nearest->GetType(), progress,
                                      maxHealth > 0.0f ? 1.0f / maxHealth : 1.0f });
            }
        }

        // Adds and progress updates. A player whose nearest boss changed
        // gets a fresh Add (the client's one bar simply re-labels). Progress
        // is resent only when the bar can visibly move — one HP of 300 —
        // the same throttle EndDragonFight::BroadcastBossProgress applies.
        for (const auto& [connectionId, bar] : wanted) {
            auto it = m_shown.find(connectionId);
            if (it == m_shown.end() || it->second.wardenId != bar.wardenId) {
                SendAdd(connectionId, bar.progress, bar.type);
                m_shown[connectionId] = bar;
                continue;
            }
            if (std::fabs(bar.progress - it->second.progress) >= bar.step
                || (bar.progress <= 0.0f) != (it->second.progress <= 0.0f)) {
                SendProgress(connectionId, bar.progress);
                it->second.progress = bar.progress;
            }
        }

        // Removes: out of range, left the dimension, disconnected, or the
        // boss died. SendRemove tolerates a session that is already gone.
        for (auto it = m_shown.begin(); it != m_shown.end();) {
            if (wanted.count(it->first) == 0) {
                SendRemove(it->first);
                it = m_shown.erase(it);
            } else {
                ++it;
            }
        }
    }

    void SilentWardenBossBars::RemoveAll() {
        for (const auto& [connectionId, bar] : m_shown) SendRemove(connectionId);
        m_shown.clear();
    }

    void SilentWardenBossBars::SendAdd(uint32_t connectionId, float progress,
                                       Game::EntityTypeId type) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::BossEventS2CPacket p;
        p.op       = Network::BossEventS2CPacket::Op::Add;
        p.progress = progress;
        // Blue, smooth — the dragon's bar is pink/smooth; the wither's in MC
        // is purple/smooth. A bar shape of its own for the Hush.
        p.color    = Network::BossEventS2CPacket::Color::Blue;
        p.notches  = 0;
        p.name     = "Silent Warden";
        if (type == Game::EntityTypeId::ChoirMother) {
            // The Choir Mother: purple, NOTCHED_6 — her phases break at
            // 2/3 and 1/3 of her health (ChoirMother::Phase), which fall on
            // the overlay's 4th and 2nd notch.
            p.color   = Network::BossEventS2CPacket::Color::Purple;
            p.notches = 6;
            p.name    = "The Choir Mother";
        } else if (type == Game::EntityTypeId::TheUnsung) {
            // Aurelith's Unsung: white — the light it stole — NOTCHED_6, its
            // phases breaking at 2/3 and 1/3 (TheUnsung::HealthPhase).
            p.color   = Network::BossEventS2CPacket::Color::White;
            p.notches = 6;
            p.name    = "The Unsung";
        }
        session->GetConnection()->SendPacket(
            static_cast<uint8_t>(Network::PacketId::BossEventS2C),
            Network::Serialization::Serialize(p));
    }

    void SilentWardenBossBars::SendProgress(uint32_t connectionId, float progress) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::BossEventS2CPacket p;
        p.op       = Network::BossEventS2CPacket::Op::UpdateProgress;
        p.progress = progress;
        session->GetConnection()->SendPacket(
            static_cast<uint8_t>(Network::PacketId::BossEventS2C),
            Network::Serialization::Serialize(p));
    }

    void SilentWardenBossBars::SendRemove(uint32_t connectionId) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::BossEventS2CPacket p;
        p.op = Network::BossEventS2CPacket::Op::Remove;
        session->GetConnection()->SendPacket(
            static_cast<uint8_t>(Network::PacketId::BossEventS2C),
            Network::Serialization::Serialize(p));
    }

} // namespace Server
