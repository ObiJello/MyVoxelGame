// File: src/client/renderer/debug/FlickerDiag.cpp
#include "FlickerDiag.hpp"

#include "common/core/Log.hpp"

#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace Render::FlickerDiag {

    namespace {
        struct Track {
            int64_t  v[3] = {0, 0, 0};   // v[0] = this frame, v[1] = last, v[2] = the one before
            int      n = 0;              // frames recorded (saturates at 3)
            uint64_t frameSeen = 0;
            bool     state = false;      // RecordState: every change logs
            uint32_t suppressed = 0;     // toggles not logged since the last line for this key
        };

        uint64_t g_frame = 1;
        std::map<std::string, Track>       g_tracks;
        std::map<std::string, std::string> g_notes;
        // At most this many lines per second, so a sustained flicker does
        // not drown the log; the suppressed count says how much it hid.
        constexpr double kMaxLinesPerSecond = 8.0;
        double g_lastLineTime = -1e9;
        uint32_t g_linesSuppressed = 0;

        double Now() {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        void Put(const std::string& key, int64_t value, bool state) {
            Track& t = g_tracks[key];
            t.state = state;
            if (t.frameSeen != g_frame) {
                t.v[2] = t.v[1];
                t.v[1] = t.v[0];
                t.frameSeen = g_frame;
                if (t.n < 3) ++t.n;
            }
            t.v[0] = value;
        }
    }

    bool Enabled() {
        // Opt-in (OBEY_FLICKER_DIAG=1). It found the reachable-slot
        // contention of 2026-09-03 (ChunkRenderer::PickEvictionSlot); kept
        // for the next one.
        static const bool enabled = [] {
            const char* v = std::getenv("OBEY_FLICKER_DIAG");
            return v && v[0] == '1';
        }();
        return enabled;
    }

    void Record(const std::string& key, int64_t value)      { if (Enabled()) Put(key, value, false); }
    void RecordState(const std::string& key, int64_t value) { if (Enabled()) Put(key, value, true); }
    void Note(const std::string& key, const std::string& text) { if (Enabled()) g_notes[key] = text; }

    void EndFrame() {
        if (!Enabled()) return;
        std::string line;
        for (auto& [key, t] : g_tracks) {
            if (t.frameSeen != g_frame) continue;
            bool report = false;
            if (t.state) {
                // A mode: any change is worth a line.
                report = t.n >= 2 && t.v[0] != t.v[1];
            } else {
                // A measurement: only the A → B → A shape.
                report = t.n >= 3 && t.v[0] == t.v[2] && t.v[0] != t.v[1];
            }
            if (!report) continue;
            if (!line.empty()) line += " | ";
            line += key + ": " + std::to_string(t.v[2]) + " -> " + std::to_string(t.v[1]) +
                    " -> " + std::to_string(t.v[0]);
            if (t.suppressed) { line += " (+" + std::to_string(t.suppressed) + " hidden)"; t.suppressed = 0; }
        }
        if (!line.empty()) {
            const double now = Now();
            if (now - g_lastLineTime >= 1.0 / kMaxLinesPerSecond) {
                std::string ctx;
                for (const auto& [k, v] : g_notes) ctx += " " + k + "=" + v;
                Log::Info("[Flicker] f%llu %s%s%s", static_cast<unsigned long long>(g_frame), line.c_str(),
                          g_linesSuppressed ? (" (" + std::to_string(g_linesSuppressed) + " lines hidden)").c_str() : "",
                          ctx.c_str());
                g_lastLineTime = now;
                g_linesSuppressed = 0;
            } else {
                ++g_linesSuppressed;
                for (auto& [key, t] : g_tracks) {
                    if (t.frameSeen == g_frame && t.n >= 2 && t.v[0] != t.v[1]) ++t.suppressed;
                }
            }
        }
        g_notes.clear();
        ++g_frame;
    }

} // namespace Render::FlickerDiag
