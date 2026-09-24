// File: src/server/ServerStressStats.cpp
#include "ServerStressStats.hpp"
#include "IntegratedServer.hpp"
#include "session/PlayerSessionManager.hpp"
#include "session/PlayerSession.hpp"
#include "network/ServerConnection.hpp"
#include "level/ServerLevel.hpp"
#include "entity/MobManager.hpp"
#include "entity/ItemEntityManager.hpp"
#include "world/MyTerrainGenerator.hpp"
#include "common/world/level/World.hpp"
#include "common/core/Log.hpp"
#include "common/core/ProcessMemory.hpp"
#include "util/LiveCounters.h"   // terrain library live-object gauges
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace Server {

    const char* ServerStressStats::PhaseName(Phase phase) {
        switch (phase) {
            case Phase::Packets:      return "packets";
            case Phase::Sessions:     return "sessions";
            case Phase::BlocksLight:  return "blocks+light";
            case Phase::WatchSet:     return "watchset";
            case Phase::ChunkResults: return "chunkresults";
            case Phase::World:        return "world";
            case Phase::ChunkSend:    return "chunksend";
            case Phase::Broadcast:    return "broadcast";
            case Phase::Maintenance:  return "maintenance";
            case Phase::Count:        break;
        }
        return "?";
    }

    ServerStressStats::ServerStressStats() {
        const char* v = std::getenv("OBEY_SERVER_STATS");
        m_enabled = v && v[0] == '1';
        m_tickNanos.reserve(64);
    }

    ServerStressStats::~ServerStressStats() {
        if (m_csv) std::fclose(m_csv);
    }

    void ServerStressStats::BeginTick() {
        if (!m_enabled) return;
        m_phaseStart = Clock::now();
        m_inTick = true;
    }

    void ServerStressStats::Mark(Phase phase) {
        if (!m_enabled || !m_inTick) return;
        const Clock::time_point now = Clock::now();
        const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_phaseStart).count();
        const size_t i = static_cast<size_t>(phase);
        m_phaseSum[i] += ns;
        m_phaseMax[i] = std::max(m_phaseMax[i], ns);
        m_phaseStart = now;
    }

    void ServerStressStats::EndTick(int64_t tickNanos) {
        if (!m_enabled) return;
        // Whatever ran after the last mark (the tick's tail) is maintenance.
        if (m_inTick) Mark(Phase::Maintenance);
        m_inTick = false;
        m_tickNanos.push_back(tickNanos);
    }

    void ServerStressStats::OpenCsv() {
        if (m_csv || m_csvFailed || m_csvPath.empty()) return;
        m_csv = std::fopen(m_csvPath.c_str(), "w");
        if (!m_csv) {
            m_csvFailed = true;
            Log::Warning("[ServerStats] cannot write %s", m_csvPath.c_str());
            return;
        }
        std::fprintf(m_csv, "second,tps,mspt_avg,mspt_p95,mspt_max");
        for (size_t i = 0; i < static_cast<size_t>(Phase::Count); ++i) {
            std::fprintf(m_csv, ",%s_avg_ms,%s_max_ms", PhaseName(static_cast<Phase>(i)),
                         PhaseName(static_cast<Phase>(i)));
        }
        std::fprintf(m_csv, ",chunk_results,load_backlog,loaded_chunks,mobs,items,players,chunks_sent,chunk_mb_sent,wire_mb_sent,"
                            "send_backlog_max_mb,send_backlog_sum_mb,unacked_max,rate_min,rate_avg,"
                            "batch_rtt_avg_ms,batch_rtt_max_ms,ping_avg_ms,ping_max_ms,"
                            "mem_mb,mem_peak_mb,lib_holders,lib_pending_unload,"
                            "lib_protochunks,lib_noisechunks,lib_holders_deleted\n");
        std::fflush(m_csv);
        Log::Info("[ServerStats] writing %s", m_csvPath.c_str());
    }

    void ServerStressStats::MaybeReport(IntegratedServer& server) {
        if (!m_enabled) return;
        const Clock::time_point now = Clock::now();
        if (m_windowStart.time_since_epoch().count() == 0) {
            m_windowStart = now;
            return;
        }
        const double windowSec = std::chrono::duration<double>(now - m_windowStart).count();
        if (windowSec < 1.0) return;

        // ── Ticks ────────────────────────────────────────────────────────
        const size_t ticks = m_tickNanos.size();
        double avgMs = 0.0, p95Ms = 0.0, maxMs = 0.0;
        if (ticks > 0) {
            std::vector<int64_t> sorted = m_tickNanos;
            std::sort(sorted.begin(), sorted.end());
            int64_t sum = 0;
            for (int64_t v : sorted) sum += v;
            avgMs = static_cast<double>(sum) / static_cast<double>(ticks) / 1.0e6;
            p95Ms = static_cast<double>(sorted[std::min(ticks - 1, (ticks * 95) / 100)]) / 1.0e6;
            maxMs = static_cast<double>(sorted.back()) / 1.0e6;
        }
        const double tps = static_cast<double>(ticks) / windowSec;

        // ── Players ──────────────────────────────────────────────────────
        struct PlayerRow {
            std::string name;
            uint32_t id = 0;
            uint64_t backlogBytes = 0;
            double chunksPerSec = 0.0;
            double wireKBps = 0.0;
            int unacked = 0;
            float rate = 0.0f;
            double rttAvg = 0.0, rttMax = 0.0;
            int32_t pingMs = 0;
            size_t pendingChunks = 0;
        };
        std::vector<PlayerRow> rows;
        uint64_t chunksSent = 0, chunkBytes = 0, wireBytes = 0, backlogMax = 0, backlogSum = 0;
        int unackedMax = 0;
        float rateMin = 0.0f;
        double rateSum = 0.0, rttSum = 0.0, rttMax = 0.0, pingSum = 0.0;
        uint32_t rttCount = 0;
        int32_t pingMax = 0;
        std::unordered_map<uint32_t, PlayerPrev> seen;

        if (PlayerSessionManager* sessions = server.GetSessionManager()) {
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session) continue;
                PlayerRow row;
                row.id = session->GetPlayerId();
                PlayerPrev cur;
                cur.chunks = session->GetChunksSentTotal();
                cur.chunkBytes = session->GetChunkBytesSentTotal();
                if (ServerConnection* conn = session->GetConnection()) {
                    const auto& st = conn->GetStats();
                    cur.wireBytes = st.wireBytesSent.load(std::memory_order_relaxed);
                    row.backlogBytes = st.pendingSendBytes.load(std::memory_order_relaxed);
                    row.pingMs = conn->GetLatencyMs();
                    row.name = conn->GetPlayerName();
                }
                const auto it = m_prev.find(row.id);
                const PlayerPrev prev = it != m_prev.end() ? it->second : PlayerPrev{};
                const uint64_t dChunks = cur.chunks >= prev.chunks ? cur.chunks - prev.chunks : 0;
                const uint64_t dBytes = cur.chunkBytes >= prev.chunkBytes ? cur.chunkBytes - prev.chunkBytes : 0;
                const uint64_t dWire = cur.wireBytes >= prev.wireBytes ? cur.wireBytes - prev.wireBytes : 0;
                seen[row.id] = cur;

                row.chunksPerSec = static_cast<double>(dChunks) / windowSec;
                row.wireKBps = static_cast<double>(dWire) / 1024.0 / windowSec;
                row.unacked = session->GetUnackedBatches();
                row.rate = session->GetDesiredChunksPerTick();
                row.pendingChunks = session->GetPendingChunksToSendCount();
                const PlayerSession::BatchRtt rtt = session->TakeBatchRtt();
                if (rtt.count > 0) {
                    row.rttAvg = rtt.sumMs / rtt.count;
                    row.rttMax = rtt.maxMs;
                    rttSum += rtt.sumMs;
                    rttCount += rtt.count;
                    rttMax = std::max(rttMax, rtt.maxMs);
                }

                chunksSent += dChunks;
                chunkBytes += dBytes;
                wireBytes += dWire;
                backlogMax = std::max(backlogMax, row.backlogBytes);
                backlogSum += row.backlogBytes;
                unackedMax = std::max(unackedMax, row.unacked);
                rateMin = rows.empty() ? row.rate : std::min(rateMin, row.rate);
                rateSum += row.rate;
                pingSum += row.pingMs;
                pingMax = std::max(pingMax, row.pingMs);
                rows.push_back(std::move(row));
            }
        }
        m_prev.swap(seen);

        const size_t players = rows.size();
        const double mb = 1024.0 * 1024.0;
        const double rttAvg = rttCount ? rttSum / rttCount : 0.0;
        const double pingAvg = players ? pingSum / static_cast<double>(players) : 0.0;
        const double rateAvg = players ? rateSum / static_cast<double>(players) : 0.0;
        const size_t backlog = server.GetPendingChunkLoadCount();

        // What the world simulation has to walk: loaded chunks and the
        // entities in them, over every level. A "world" phase that keeps
        // growing is one of these growing.
        size_t loadedChunks = 0, mobs = 0, items = 0;
        // Memory: the process footprint, and the terrain library's chunk
        // holders — which only leave through ChunkMap::processUnloads, so a
        // holder count that climbs with every new area is memory that is
        // never coming back.
        size_t libHolders = 0, libPendingUnload = 0;
        server.ForEachLevel([&](ServerLevel& level) {
            if (const Game::World* w = level.World()) loadedChunks += w->GetLoadedChunkCount();
            if (const MobManager* m = level.Mobs()) mobs += m->Count();
            if (const ItemEntityManager* it = level.Items()) items += it->Count();
            if (const Game::MyTerrainGenerator* gen = level.TerrainGenerator()) {
                libHolders += gen->LibraryChunkCount();
                libPendingUnload += gen->LibraryPendingUnloadCount();
            }
        });
        const Core::ProcessMemory memory = Core::QueryProcessMemory();
        using minecraft::util::LiveCounters;
        const long long protoChunks = LiveCounters::protoChunks().load(std::memory_order_relaxed);
        const long long noiseChunks = LiveCounters::noiseChunks().load(std::memory_order_relaxed);
        const long long holdersDeleted = LiveCounters::holdersDeleted().load(std::memory_order_relaxed);

        // Every 10 s: what the library's holders ARE, by what processUnloads
        // does with them — which band a growing holder count lives in. A
        // full walk of every holder map, hence not every second.
        if (m_secondsElapsed % 10 == 0) {
            Game::MyTerrainGenerator::HolderStatusCounts total;
            server.ForEachLevel([&](ServerLevel& level) {
                if (const Game::MyTerrainGenerator* gen = level.TerrainGenerator()) {
                    const auto c = gen->LibraryHolderStatusCounts();
                    total.noChunk += c.noChunk;
                    total.beforeTerrain += c.beforeTerrain;
                    total.edgeBand += c.edgeBand;
                    total.full += c.full;
                    total.wanted += c.wanted;
                }
            });
            Log::Info("[LibMem] holders=%zu wanted=%zu | full=%zu edgeBand=%zu beforeTerrain=%zu noChunk=%zu | "
                      "protoChunks=%lld noiseChunks=%lld holdersDeleted=%lld | mem=%.0fMB",
                      libHolders, total.wanted, total.full, total.edgeBand, total.beforeTerrain, total.noChunk,
                      protoChunks, noiseChunks, holdersDeleted,
                      static_cast<double>(memory.footprintBytes) / (1024.0 * 1024.0));
        }
        const double memMb = static_cast<double>(memory.footprintBytes) / (1024.0 * 1024.0);
        const double memPeakMb = static_cast<double>(memory.peakBytes) / (1024.0 * 1024.0);

        // The heaviest phase, by average — the headline of where the tick went.
        size_t worst = 0;
        for (size_t i = 1; i < static_cast<size_t>(Phase::Count); ++i) {
            if (m_phaseSum[i] > m_phaseSum[worst]) worst = i;
        }
        const double tickDiv = ticks ? static_cast<double>(ticks) : 1.0;

        Log::Info("[ServerStats] tps=%.1f mspt avg=%.1f p95=%.1f max=%.1f | heaviest=%s %.1fms/tick | "
                  "chunkResults=%llu backlog=%zu abandonedCancelled=%llu loaded=%zu mobs=%zu items=%zu | players=%zu chunks/s=%.0f chunkMB/s=%.2f wireMB/s=%.2f "
                  "sendBacklog max=%.2fMB sum=%.2fMB unacked max=%d rate min=%.1f avg=%.1f | "
                  "batchRTT avg=%.0f max=%.0fms ping avg=%.0f max=%dms | "
                  "mem=%.0fMB peak=%.0fMB libHolders=%zu libPendingUnload=%zu",
                  tps, avgMs, p95Ms, maxMs,
                  PhaseName(static_cast<Phase>(worst)), static_cast<double>(m_phaseSum[worst]) / tickDiv / 1.0e6,
                  static_cast<unsigned long long>(m_chunkResults), backlog,
                  static_cast<unsigned long long>(server.AbandonedLoadsCancelled()), loadedChunks, mobs, items,
                  players, static_cast<double>(chunksSent) / windowSec,
                  static_cast<double>(chunkBytes) / mb / windowSec, static_cast<double>(wireBytes) / mb / windowSec,
                  static_cast<double>(backlogMax) / mb, static_cast<double>(backlogSum) / mb,
                  unackedMax, rateMin, rateAvg, rttAvg, rttMax, pingAvg, pingMax,
                  memMb, memPeakMb, libHolders, libPendingUnload);

        // Phase breakdown, one line, only when the server is behind (a tick
        // over its 50 ms) — the case where it is worth reading.
        if (maxMs > 50.0) {
            char buf[1024];
            int n = std::snprintf(buf, sizeof(buf), "[ServerStats] phases avg/max ms:");
            for (size_t i = 0; i < static_cast<size_t>(Phase::Count) && n > 0 && n < static_cast<int>(sizeof(buf)); ++i) {
                n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %s=%.1f/%.1f",
                                   PhaseName(static_cast<Phase>(i)),
                                   static_cast<double>(m_phaseSum[i]) / tickDiv / 1.0e6,
                                   static_cast<double>(m_phaseMax[i]) / 1.0e6);
            }
            Log::Info("%s", buf);
        }

        if (++m_reportsSinceDetail >= 5 && !rows.empty()) {
            m_reportsSinceDetail = 0;
            std::sort(rows.begin(), rows.end(), [](const PlayerRow& a, const PlayerRow& b) {
                return a.backlogBytes > b.backlogBytes;
            });
            const size_t shown = std::min<size_t>(rows.size(), 5);
            for (size_t i = 0; i < shown; ++i) {
                const PlayerRow& r = rows[i];
                Log::Info("[ServerStats]   player %u '%s': sendBacklog=%.2fMB wire=%.0fKB/s chunks/s=%.0f "
                          "pendingChunks=%zu unacked=%d rate=%.1f batchRTT avg=%.0f max=%.0fms ping=%dms",
                          r.id, r.name.c_str(), static_cast<double>(r.backlogBytes) / mb, r.wireKBps,
                          r.chunksPerSec, r.pendingChunks, r.unacked, r.rate, r.rttAvg, r.rttMax, r.pingMs);
            }
        }

        OpenCsv();
        if (m_csv) {
            std::fprintf(m_csv, "%lld,%.2f,%.2f,%.2f,%.2f", static_cast<long long>(m_secondsElapsed),
                         tps, avgMs, p95Ms, maxMs);
            for (size_t i = 0; i < static_cast<size_t>(Phase::Count); ++i) {
                std::fprintf(m_csv, ",%.2f,%.2f", static_cast<double>(m_phaseSum[i]) / tickDiv / 1.0e6,
                             static_cast<double>(m_phaseMax[i]) / 1.0e6);
            }
            std::fprintf(m_csv, ",%llu,%zu,%zu,%zu,%zu,%zu,%llu,%.3f,%.3f,%.3f,%.3f,%d,%.2f,%.2f,%.1f,%.1f,%.1f,%d,"
                                "%.0f,%.0f,%zu,%zu,%lld,%lld,%lld\n",
                         static_cast<unsigned long long>(m_chunkResults), backlog, loadedChunks, mobs, items, players,
                         static_cast<unsigned long long>(chunksSent),
                         static_cast<double>(chunkBytes) / mb, static_cast<double>(wireBytes) / mb,
                         static_cast<double>(backlogMax) / mb, static_cast<double>(backlogSum) / mb,
                         unackedMax, rateMin, rateAvg, rttAvg, rttMax, pingAvg, pingMax,
                         memMb, memPeakMb, libHolders, libPendingUnload,
                         protoChunks, noiseChunks, holdersDeleted);
            std::fflush(m_csv);
        }

        // Next window.
        ++m_secondsElapsed;
        m_windowStart = now;
        m_tickNanos.clear();
        m_phaseSum.fill(0);
        m_phaseMax.fill(0);
        m_chunkResults = 0;
    }

} // namespace Server
