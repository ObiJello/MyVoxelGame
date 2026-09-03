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
//
// Tokens: sky, opaque, cutout, translucent, players, items, mobs,
//         blockentities, particles, clouds, helditem, hud
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/core/Log.hpp"

namespace Render {
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
        }
        return on;
    }
}
