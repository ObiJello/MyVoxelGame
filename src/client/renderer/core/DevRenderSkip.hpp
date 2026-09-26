#pragma once
// Dev-only A/B switch for render-cost attribution.
//
//   OBEY_SKIP=cutout,mobs ./MyVoxelGame ...
//   OBEY_SKIP=cutout OBEY_SKIP_PERIOD=3 ./MyVoxelGame ...
//
// Skips whole render stages so their cost can be measured by subtraction
// from the frame time. Tile-based GPUs (Apple) make per-pass GPU timestamps
// inside one render pass meaningless, so subtraction is the only honest way
// to attribute GPU time per stage. Same idea as OBEY_VK_NO_INDIRECT.
//
// OBEY_SKIP_PERIOD=<sec> alternates the skip on/off every <sec> seconds
// inside ONE run and logs each transition ("[DevSkip] phase=on|off"). A
// fanless Mac throttles within a minute of sustained load, so two separate
// runs are not comparable — interleaving gives both phases the same clock
// state. Pair the per-second "[Harness]" fps lines with the phase log.
// On macOS each transition is also a Points-of-Interest signpost ("DevSkip",
// "on"/"off"), which a Metal System Trace records on its own clock — that is
// what splits the GPU timeline into skip-on / skip-off frames exactly.
//
// Tokens: sky, opaque, cutout, translucent, players, items, mobs,
//         blockentities, particles, clouds, helditem, outline, hud
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/core/Log.hpp"
#ifdef __APPLE__
#include <os/signpost.h>
#endif

namespace Render {
    // One "Frame" Points-of-Interest signpost per presented frame while
    // OBEY_SKIP is set — the frame count a Metal System Trace cannot give on
    // GL (Apple's GL presents outside CAMetalLayer, so the trace has no
    // per-frame present events there).
    inline void DevSkipFrameMark() {
#ifdef __APPLE__
        static const bool s_active = std::getenv("OBEY_SKIP") != nullptr;
        if (!s_active) return;
        static os_log_t s_log = os_log_create("com.obeycraft.dev", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
        os_signpost_event_emit(s_log, OS_SIGNPOST_ID_EXCLUSIVE, "Frame");
#endif
    }

    inline bool DevSkip(const char* stage) {
        struct Cfg {
            std::vector<std::string> tokens;
            double period = 0.0;
            std::chrono::steady_clock::time_point start;
            bool lastPhase = false;
        };
        static Cfg cfg = [] {
            Cfg c;
            if (const char* env = std::getenv("OBEY_SKIP")) {
                std::string cur;
                for (const char* p = env;; ++p) {
                    if (*p == ',' || *p == '\0') {
                        if (!cur.empty()) c.tokens.push_back(cur);
                        cur.clear();
                        if (*p == '\0') break;
                    } else {
                        cur.push_back(*p);
                    }
                }
            }
            if (const char* p = std::getenv("OBEY_SKIP_PERIOD")) c.period = std::atof(p);
            c.start = std::chrono::steady_clock::now();
            return c;
        }();
        if (cfg.tokens.empty()) return false;
        bool listed = false;
        for (const auto& t : cfg.tokens)
            if (std::strcmp(t.c_str(), stage) == 0) { listed = true; break; }
        if (!listed) return false;
        if (cfg.period <= 0.0) return true;
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - cfg.start).count();
        const bool on = (static_cast<long long>(elapsed / cfg.period) % 2) == 1;
        if (on != cfg.lastPhase) {
            cfg.lastPhase = on;
            Log::Info("[DevSkip] phase=%s", on ? "on" : "off");
#ifdef __APPLE__
            static os_log_t s_log = os_log_create("com.obeycraft.dev", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
            if (on) os_signpost_event_emit(s_log, OS_SIGNPOST_ID_EXCLUSIVE, "DevSkip", "on");
            else    os_signpost_event_emit(s_log, OS_SIGNPOST_ID_EXCLUSIVE, "DevSkip", "off");
#endif
        }
        return on;
    }
}
