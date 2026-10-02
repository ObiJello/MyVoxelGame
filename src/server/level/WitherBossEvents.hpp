// File: src/server/level/WitherBossEvents.hpp
//
// MC WitherBoss.bossEvent — one ServerBossEvent per wither: PURPLE,
// PROGRESS (smooth), darkenScreen, named after the wither's display name
// (its custom name, else "Wither"), shown to exactly the players whose
// clients track the wither (startSeenByPlayer / stopSeenByPlayer — the
// mob tracker's watcher set here). Its progress is what customServerAiStep
// sets: 1 - invulnerableTicks / 220 through the spawn charge (0 the moment
// makeInvulnerable runs), health / maxHealth after it.
//
// The client keys bars by a byte (BossEventS2C barId): each wither takes one
// of 1..255 from a server-wide pool (0 is the dragon fight's and the Hush
// bosses' bar), so a bar's remove can never hit another level's wither.
//
// One per ServerLevel, every dimension (a wither can be built anywhere),
// ticked from IntegratedServer's per-level tick after the mobs.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Server {

    class ServerLevel;
    class PlayerSessionManager;

    class WitherBossEvents {
    public:
        WitherBossEvents(ServerLevel& level, PlayerSessionManager* sessions);

        void Tick();

        // Every bar down — level teardown.
        void RemoveAll();

    private:
        struct Event {
            uint8_t                      barId = 0;
            std::string                  name;
            float                        progress = 0.0f;
            std::unordered_set<uint32_t> players;   // connection ids shown the bar
        };

        void SendAdd(uint32_t connectionId, const Event& event);
        void SendProgress(uint32_t connectionId, const Event& event);
        void SendName(uint32_t connectionId, const Event& event);
        void SendRemove(uint32_t connectionId, uint8_t barId);

        static uint8_t AllocateBarId();
        static void    ReleaseBarId(uint8_t barId);

        ServerLevel&          m_level;
        PlayerSessionManager* m_sessions = nullptr;
        // wither entity id -> its event.
        std::unordered_map<int32_t, Event> m_events;
    };

} // namespace Server
