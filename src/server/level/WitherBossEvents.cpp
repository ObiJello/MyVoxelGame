// File: src/server/level/WitherBossEvents.cpp
#include "server/level/WitherBossEvents.hpp"

#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/network/PacketTypes.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerEntityTracker.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include <array>
#include <vector>

namespace Server {

    namespace {

        // The bar a wither shows right now (WitherBoss.customServerAiStep's
        // bossEvent.setProgress calls, and makeInvulnerable's 0).
        float ProgressOf(const Game::Wither& wither) {
            const int invulnerableTicks = wither.GetInvulnerableTicks();
            if (invulnerableTicks > 0) {
                return 1.0f - static_cast<float>(invulnerableTicks) / 220.0f;
            }
            const float maxHealth = wither.GetMaxHealth();
            return maxHealth > 0.0f ? wither.GetHealth() / maxHealth : 0.0f;
        }

        // Entity.getDisplayName: the custom name, else the type's name.
        std::string DisplayNameOf(const Game::Wither& wither) {
            if (const auto& name = wither.GetCustomName()) return *name;
            return "Wither";
        }

        // Server thread only, like every caller.
        std::array<bool, 256>& BarIdsInUse() {
            static std::array<bool, 256> inUse{};
            return inUse;
        }

    } // namespace

    WitherBossEvents::WitherBossEvents(ServerLevel& level, PlayerSessionManager* sessions)
        : m_level(level), m_sessions(sessions) {}

    uint8_t WitherBossEvents::AllocateBarId() {
        std::array<bool, 256>& inUse = BarIdsInUse();
        for (int id = 1; id < 256; ++id) {
            if (!inUse[static_cast<size_t>(id)]) {
                inUse[static_cast<size_t>(id)] = true;
                return static_cast<uint8_t>(id);
            }
        }
        return 0;   // 255 withers at once: the rest share no bar
    }

    void WitherBossEvents::ReleaseBarId(uint8_t barId) {
        if (barId != 0) BarIdsInUse()[barId] = false;
    }

    void WitherBossEvents::Tick() {
        MobManager* mobs = m_level.Mobs();
        ServerEntityTracker* tracker = m_level.MobTracker();
        std::unordered_set<int32_t> alive;
        if (mobs) {
            for (const Game::Mob* mob : mobs->List()) {
                if (!mob || mob->GetType() != Game::EntityTypeId::Wither || mob->IsRemoved()) continue;
                const auto& wither = static_cast<const Game::Wither&>(*mob);
                const int32_t id = wither.GetId();
                alive.insert(id);

                auto it = m_events.find(id);
                if (it == m_events.end()) {
                    Event fresh;
                    fresh.barId = AllocateBarId();
                    fresh.name = DisplayNameOf(wither);
                    fresh.progress = ProgressOf(wither);
                    it = m_events.emplace(id, std::move(fresh)).first;
                }
                Event& event = it->second;
                if (event.barId == 0) continue;

                // ServerBossEvent.setProgress / setName broadcast to the
                // event's players when the value changes.
                const float progress = ProgressOf(wither);
                if (progress != event.progress) {
                    event.progress = progress;
                    for (uint32_t connectionId : event.players) SendProgress(connectionId, event);
                }
                const std::string name = DisplayNameOf(wither);
                if (name != event.name) {
                    event.name = name;
                    for (uint32_t connectionId : event.players) SendName(connectionId, event);
                }

                // startSeenByPlayer / stopSeenByPlayer: the tracker's
                // watchers of this wither.
                const std::unordered_set<uint32_t>* watchers = tracker ? tracker->WatchersOf(id) : nullptr;
                for (auto p = event.players.begin(); p != event.players.end();) {
                    if (!watchers || watchers->count(*p) == 0) {
                        SendRemove(*p, event.barId);
                        p = event.players.erase(p);
                    } else {
                        ++p;
                    }
                }
                if (watchers) {
                    for (uint32_t connectionId : *watchers) {
                        if (event.players.insert(connectionId).second) SendAdd(connectionId, event);
                    }
                }
            }
        }

        // A wither that is gone: its trackers stopped seeing it.
        for (auto it = m_events.begin(); it != m_events.end();) {
            if (alive.count(it->first) == 0) {
                for (uint32_t connectionId : it->second.players) SendRemove(connectionId, it->second.barId);
                ReleaseBarId(it->second.barId);
                it = m_events.erase(it);
            } else {
                ++it;
            }
        }
    }

    void WitherBossEvents::RemoveAll() {
        for (auto& [id, event] : m_events) {
            (void)id;
            for (uint32_t connectionId : event.players) SendRemove(connectionId, event.barId);
            ReleaseBarId(event.barId);
        }
        m_events.clear();
    }

    void WitherBossEvents::SendAdd(uint32_t connectionId, const Event& event) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::BossEventS2CPacket p;
        p.op         = Network::BossEventS2CPacket::Op::Add;
        p.barId      = event.barId;
        p.progress   = event.progress;
        p.color      = Network::BossEventS2CPacket::Color::Purple;   // BossBarColor.PURPLE
        p.notches    = 0;                                            // BossBarOverlay.PROGRESS
        p.name       = event.name;
        p.properties = Network::BossEventS2CPacket::kDarkenScreen;   // setDarkenScreen(true)
        session->GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::BossEventS2C),
                                             Network::Serialization::Serialize(p));
    }

    void WitherBossEvents::SendProgress(uint32_t connectionId, const Event& event) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::BossEventS2CPacket p;
        p.op       = Network::BossEventS2CPacket::Op::UpdateProgress;
        p.barId    = event.barId;
        p.progress = event.progress;
        session->GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::BossEventS2C),
                                             Network::Serialization::Serialize(p));
    }

    void WitherBossEvents::SendName(uint32_t connectionId, const Event& event) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::BossEventS2CPacket p;
        p.op    = Network::BossEventS2CPacket::Op::UpdateName;
        p.barId = event.barId;
        p.name  = event.name;
        session->GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::BossEventS2C),
                                             Network::Serialization::Serialize(p));
    }

    void WitherBossEvents::SendRemove(uint32_t connectionId, uint8_t barId) {
        if (!m_sessions) return;
        auto session = m_sessions->GetSessionByConnection(connectionId);
        if (!session || !session->GetConnection()) return;
        Network::BossEventS2CPacket p;
        p.op    = Network::BossEventS2CPacket::Op::Remove;
        p.barId = barId;
        session->GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::BossEventS2C),
                                             Network::Serialization::Serialize(p));
    }

} // namespace Server
