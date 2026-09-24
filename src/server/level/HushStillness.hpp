// File: src/server/level/HushStillness.hpp
//
// The Hush's "stillness" (docs/the-hush.md, Atmosphere). Every 6–12 minutes
// a stillness falls over the level for 8–15 seconds: every mob but the
// bosses stops where it stands and does nothing (Mob::ServerAiStep's early
// out, HushStillnessRules.hpp), and the players there see the world hold its
// breath — the fog closes in, the auroras dim, the sky flattens — until it
// lifts with a soft reverse (the client look, Render::HushAtmosphere).
//
// One per Hush ServerLevel, owned by it and ticked from IntegratedServer's
// per-level mob tick BEFORE the mobs tick, so the flag it raises on the
// level's bridge (ServerLevelBridge::SetStilled) is the one this tick's mobs
// read. The timer only runs while that tick runs — i.e. while the Hush has
// players or forced chunks — so an empty Hush does not burn through its
// stillnesses unseen. It is not saved: a reload schedules a fresh one
// (the design says saving is not required, and nothing persistent depends
// on it).
//
// Clients learn of it through HushStillnessS2C, sent per PLAYER the way
// SilentWardenBossBars keys its bars: each player in the Hush is told once
// when one begins (or when they arrive during one), and told it lifted when
// it ends or when they leave the Hush while it is on.
#pragma once

#include <cstdint>
#include <random>
#include <unordered_set>

namespace Server {

    class ServerLevel;
    class PlayerSessionManager;

    class HushStillness {
    public:
        // 6–12 minutes between stillnesses, 8–15 seconds each (20 TPS).
        static constexpr int kMinIntervalTicks = 6 * 60 * 20;
        static constexpr int kMaxIntervalTicks = 12 * 60 * 20;
        static constexpr int kMinDurationTicks = 8 * 20;
        static constexpr int kMaxDurationTicks = 15 * 20;
        // /stillness <seconds> accepts up to a minute.
        static constexpr int kMaxCommandDurationTicks = 60 * 20;

        HushStillness(ServerLevel& level, PlayerSessionManager* sessions);

        // Once per server tick, before the level's mobs tick: advances the
        // timer, raises/lowers the bridge flag and syncs the players.
        void Tick();

        // Begin one now (the /stillness command). durationTicks <= 0 rolls
        // the natural 8–15 s. An active stillness is restarted with the new
        // length. The next natural one is scheduled from its end.
        void Start(int durationTicks = 0);
        // Lift the current one now (/stillness stop). No-op when none is on.
        void Stop();

        bool Active() const { return m_remainingTicks > 0; }
        int  RemainingTicks() const { return m_remainingTicks; }
        int  TicksUntilNext() const { return m_ticksUntilNext; }

        // Level teardown: tell every player who was told a stillness is on
        // that it lifted, and drop the flag.
        void RemoveAll();

    private:
        int  RollInterval();
        int  RollDuration();
        void ApplyToLevel();
        void SyncPlayers();
        void Send(uint32_t connectionId, bool active);

        ServerLevel&          m_level;
        PlayerSessionManager* m_sessions = nullptr;
        std::mt19937          m_rng;

        int m_remainingTicks = 0;   // > 0 while a stillness is on
        int m_ticksUntilNext = 0;   // counts down while none is on

        // Connections that were told a stillness is on and not yet that it
        // lifted.
        std::unordered_set<uint32_t> m_told;
    };

} // namespace Server
