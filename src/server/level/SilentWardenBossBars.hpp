// File: src/server/level/SilentWardenBossBars.hpp
//
// The Silent Warden's boss bar (docs/the-hush.md). MC has no bar on the
// warden; this is the ServerBossEvent shape the End's dragon fight uses
// (EndDragonFight::UpdatePlayers / SendBossAdd / SendBossRemove), keyed per
// PLAYER rather than per event: the client draws one bar (BossBarState), so
// each player sees the nearest living Silent Warden within kRange, and the
// bar follows that warden's health.
//
// One per ServerLevel, every dimension — a boss can be /summon'd anywhere —
// ticked from IntegratedServer's per-level tick beside the dragon fight.
//
// It serves every Hush boss — the Silent Warden, the Choir Mother
// (HushCreatures.hpp) and Aurelith's Unsung (TheUnsung.hpp) — because the
// client draws one bar: each player sees the nearest living boss of any of
// them, labelled and coloured by kind, so they never fight over the single
// bar.
#pragma once

#include "common/entity/GeneratedEntityTypes.hpp"

#include <cstdint>
#include <unordered_map>

namespace Server {

    class ServerLevel;
    class PlayerSessionManager;

    class SilentWardenBossBars {
    public:
        // MC ServerBossEvent has no range of its own; 64 blocks is the
        // warden's own hearing radius, and far enough that the bar appears
        // before the roar.
        static constexpr double kRange = 64.0;

        SilentWardenBossBars(ServerLevel& level, PlayerSessionManager* sessions);

        // Per server tick, after the mobs ticked (so a death this tick pulls
        // the bar the same tick).
        void Tick();

        // Every bar down — level teardown.
        void RemoveAll();

    private:
        struct Shown {
            int32_t wardenId;  // the boss's entity id (either kind)
            Game::EntityTypeId type;
            float   progress;
            float   step;      // one HP of that warden, as a bar fraction
        };

        void SendAdd(uint32_t connectionId, float progress, Game::EntityTypeId type);
        void SendProgress(uint32_t connectionId, float progress);
        void SendRemove(uint32_t connectionId);

        ServerLevel&          m_level;
        PlayerSessionManager* m_sessions = nullptr;
        // connection id -> the bar that player currently sees.
        std::unordered_map<uint32_t, Shown> m_shown;
    };

} // namespace Server
