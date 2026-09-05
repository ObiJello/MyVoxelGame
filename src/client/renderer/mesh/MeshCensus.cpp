// File: src/client/renderer/mesh/MeshCensus.cpp
#include "MeshCensus.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace Render::MeshCensus {

    namespace {
        struct Entry {
            uint64_t quads[3] = {0, 0, 0};   // emitted quads per layer (what the GPU draws)
            uint64_t faces = 0;              // block faces those quads stand for
            uint64_t mergedQuads = 0;        // emitted quads that are merged rectangles
        };
        constexpr size_t kBlocks = Game::BlockRegistry::Size;

        bool CachedEnabled() {
            static const bool s_enabled = [] {
                const char* e = std::getenv("OBEY_MESH_CENSUS");
                return e && *e && *e != '0';
            }();
            return s_enabled;
        }

        thread_local std::vector<Entry> t_local;
        thread_local uint32_t t_pendingFlush = 0;

        std::mutex g_mutex;
        std::vector<Entry> g_table;
        uint64_t g_sections = 0;
        std::chrono::steady_clock::time_point g_lastDump{};
    }

    bool Enabled() { return CachedEnabled(); }

    void Count(Game::BlockID block, int layer, bool merged, uint32_t faces) {
        if (!CachedEnabled()) return;
        const size_t id = static_cast<size_t>(block);
        if (id >= kBlocks || layer < 0 || layer > 2) return;
        if (t_local.size() < kBlocks) t_local.resize(kBlocks);
        Entry& e = t_local[id];
        e.quads[layer] += 1;
        e.faces += faces;
        if (merged) e.mergedQuads += 1;
        ++t_pendingFlush;
    }

    void FlushThread() {
        if (!CachedEnabled() || t_pendingFlush == 0) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_table.size() < kBlocks) g_table.resize(kBlocks);
        for (size_t i = 0; i < t_local.size(); ++i) {
            Entry& src = t_local[i];
            if (!src.quads[0] && !src.quads[1] && !src.quads[2]) continue;
            Entry& dst = g_table[i];
            for (int l = 0; l < 3; ++l) dst.quads[l] += src.quads[l];
            dst.faces += src.faces;
            dst.mergedQuads += src.mergedQuads;
            src = Entry{};
        }
        ++g_sections;
        t_pendingFlush = 0;
    }

    void DumpIfDue() {
        if (!CachedEnabled()) return;
        const auto now = std::chrono::steady_clock::now();
        if (g_lastDump.time_since_epoch().count() == 0) { g_lastDump = now; return; }
        if (now - g_lastDump < std::chrono::seconds(15)) return;
        g_lastDump = now;

        std::vector<std::pair<size_t, Entry>> rows;
        uint64_t total = 0, faces = 0, byLayer[3] = {0, 0, 0};
        uint64_t sections = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            sections = g_sections;
            for (size_t i = 0; i < g_table.size(); ++i) {
                const Entry& e = g_table[i];
                const uint64_t q = e.quads[0] + e.quads[1] + e.quads[2];
                if (!q) continue;
                rows.emplace_back(i, e);
                total += q; faces += e.faces;
                for (int l = 0; l < 3; ++l) byLayer[l] += e.quads[l];
            }
        }
        if (!total) return;
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.second.quads[0] + a.second.quads[1] + a.second.quads[2] >
                   b.second.quads[0] + b.second.quads[1] + b.second.quads[2];
        });
        // removed% = the share of block faces the merger took out of the
        // draw: 1 - quads emitted / faces present. A 16-block strip is one
        // quad for 16 faces (94% removed); an unmerged face is 0%.
        Log::Info("[MeshCensus] %llu sections built, %.1f M vertices drawn for %.1f M block faces "
                  "(merger removed %.0f%%): opaque %.0f%%, cutout %.0f%%, translucent %.0f%%; "
                  "%.0f vertices per section",
                  static_cast<unsigned long long>(sections), total * 4 / 1e6, faces * 4 / 1e6,
                  faces ? 100.0 * (1.0 - static_cast<double>(total) / faces) : 0.0,
                  100.0 * byLayer[0] / total, 100.0 * byLayer[1] / total, 100.0 * byLayer[2] / total,
                  sections ? total * 4.0 / sections : 0.0);
        Log::Info("[MeshCensus] %-28s %7s %7s %8s %7s %8s", "block", "verts%", "cum%", "removed%", "layer", "faces");
        double cum = 0.0;
        const size_t shown = std::min<size_t>(rows.size(), 30);
        for (size_t r = 0; r < shown; ++r) {
            const Entry& e = rows[r].second;
            const uint64_t q = e.quads[0] + e.quads[1] + e.quads[2];
            const double share = 100.0 * q / total;
            cum += share;
            const int layer = (e.quads[0] >= e.quads[1] && e.quads[0] >= e.quads[2]) ? 0 : (e.quads[1] >= e.quads[2] ? 1 : 2);
            const char* layerName = layer == 0 ? "opaque" : (layer == 1 ? "cutout" : "transl");
            const std::string& name = Game::BlockRegistry::Get(static_cast<Game::BlockID>(rows[r].first)).name;
            const double removed = e.faces ? 100.0 * (1.0 - static_cast<double>(q) / e.faces) : 0.0;
            Log::Info("[MeshCensus] %-28s %6.1f%% %6.1f%% %7.0f%% %7s %8llu",
                      name.c_str(), share, cum, removed, layerName,
                      static_cast<unsigned long long>(e.faces));
        }
    }

} // namespace Render::MeshCensus
