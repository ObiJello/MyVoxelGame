// File: src/server/ServerStressStats.hpp
//
// Once-a-second server load report — the stress test's measuring stick
// (headless server + bot swarm, see src/client/dev/BotSwarm.hpp), and a tool
// for a real hosted session: OBEY_SERVER_STATS=1 turns it on in a normal game.
//
// Per second it logs one "[ServerStats]" line and appends a CSV row:
//   - ticks run (TPS), tick time average / p95 / max (MSPT);
//   - where the tick went, per phase (average and max ms per tick);
//   - chunk results integrated, chunk-load backlog;
//   - players, and across them: chunks + bytes sent per second, wire bytes
//     per second, the worst send backlog (uncompressed bytes queued behind
//     the socket), unacked chunk batches, the requested chunk rate, batch
//     round trip (batch queued -> ack applied) and keep-alive ping.
// Every 5 s it also logs the five players with the deepest send backlog.
//
// Tick thread only: BeginTick/Mark/EndTick/Report all run on the server loop.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace Server {

    class IntegratedServer;

    class ServerStressStats {
    public:
        // The ServerTick phases, in tick order. A mark closes the phase that
        // ran since the previous mark (or since BeginTick).
        enum class Phase : uint8_t {
            Packets,        // drain every connection's C2S queue
            Sessions,       // session ticks
            BlocksLight,    // pause state, block-change flush, light engine
            WatchSet,       // view-distance changes, watch-set diff + in-tick chunk pump
            ChunkResults,   // kept chunks, async chunk load/gen results
            World,          // world simulation (blocks, entities, mobs, portals)
            ChunkSend,      // per-player chunk batches (serialisation)
            Broadcast,      // player positions, entity tracking, time sync
            Maintenance,    // unload, idle noise release, autosave, logs
            Count
        };
        static const char* PhaseName(Phase phase);

        ServerStressStats();
        ~ServerStressStats();
        ServerStressStats(const ServerStressStats&) = delete;
        ServerStressStats& operator=(const ServerStressStats&) = delete;

        // OBEY_SERVER_STATS=1 enables it; a headless server forces it on.
        bool Enabled() const { return m_enabled; }
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        // Where the CSV goes; empty = log lines only. Opened lazily.
        void SetCsvPath(std::string path) { m_csvPath = std::move(path); }

        void BeginTick();
        void Mark(Phase phase);
        void EndTick(int64_t tickNanos);
        void AddChunkResults(int count) { m_chunkResults += static_cast<uint64_t>(count > 0 ? count : 0); }

        // Once a second (cheap to call every tick).
        void MaybeReport(IntegratedServer& server);

    private:
        using Clock = std::chrono::steady_clock;

        void OpenCsv();

        bool m_enabled = false;
        std::string m_csvPath;
        std::FILE* m_csv = nullptr;
        bool m_csvFailed = false;

        // Current tick.
        Clock::time_point m_phaseStart{};
        bool m_inTick = false;

        // This second.
        std::vector<int64_t> m_tickNanos;
        std::array<int64_t, static_cast<size_t>(Phase::Count)> m_phaseSum{};
        std::array<int64_t, static_cast<size_t>(Phase::Count)> m_phaseMax{};
        uint64_t m_chunkResults = 0;
        Clock::time_point m_windowStart{};
        int m_reportsSinceDetail = 0;
        int64_t m_secondsElapsed = 0;

        // Per-player totals at the previous report, for per-second deltas.
        struct PlayerPrev {
            uint64_t chunks = 0;
            uint64_t chunkBytes = 0;
            uint64_t wireBytes = 0;
        };
        std::unordered_map<uint32_t, PlayerPrev> m_prev;
    };

} // namespace Server
