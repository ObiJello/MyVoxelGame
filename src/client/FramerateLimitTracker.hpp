// File: src/client/FramerateLimitTracker.hpp
//
// Port of MC com.mojang.blaze3d.platform.FramerateLimitTracker (26.3) — the
// frame limit the frame loop actually paces to, from the Max Framerate option
// and the "Reduce FPS When" (inactivityFpsLimit) option:
//
//   window minimized (iconified)                       → 10 fps
//   "AFK" and no input for more than 10 min (600 s)    → 10 fps
//   "AFK" and no input for more than 1 min (60 s)      → min(Max Framerate, 30)
//   out of a level with a screen up (title, menus)     → 60 fps
//   otherwise                                          → Max Framerate
//
// in that order of precedence (getThrottleReason). "Minimized" as the option
// only drops the two AFK rows; minimizing always throttles. A limit of 260
// (Options.UNLIMITED_FRAMERATE_CUTOFF) is "Unlimited": the frame limiter does
// not run (Minecraft.renderFrame). Note MC's menu row is a flat 60, not
// min(limit, 60) — a title screen runs at 60 even with the option at 30.
//
// Every input event resets the AFK clock (onInputReceived — MC calls it from
// KeyboardHandler.keyPress, MouseHandler.onButton/onScroll/onDrop and mouse
// motion while the window is active); here from Input's GLFW callbacks and
// the /control remote-input feed. DELIBERATE DEVIATION: MC's latestInputTime
// starts at 0, so a client nobody has touched yet reads as long-AFK; this one
// starts the clock at startup.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace Client::FramerateLimitTracker {

    inline constexpr int     kOutOfLevelMenuLimit   = 60;
    inline constexpr int     kIconifiedWindowLimit  = 10;
    inline constexpr int     kAfkLimit              = 30;
    inline constexpr int     kLongAfkLimit          = 10;
    inline constexpr int64_t kAfkThresholdMs        = 60000;
    inline constexpr int64_t kLongAfkThresholdMs    = 600000;
    inline constexpr int     kUnlimitedFramerateCutoff = 260;   // MC Options.UNLIMITED_FRAMERATE_CUTOFF

    enum class ThrottleReason { None, WindowIconified, LongAfk, ShortAfk, OutOfLevelMenu };

    // MC Util.getMillis: a monotonic millisecond clock.
    inline int64_t NowMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    namespace Detail {
        inline std::atomic<int64_t>& LatestInputTime() {
            static std::atomic<int64_t> s_latest{NowMillis()};
            return s_latest;
        }
    }

    // MC onInputReceived.
    inline void OnInputReceived() {
        Detail::LatestInputTime().store(NowMillis(), std::memory_order_relaxed);
    }

    // MC getThrottleReason. `afkOption` is inactivityFpsLimit == AFK;
    // `hasLevel` is minecraft.level != null; `screenOrOverlay` is a screen or
    // an overlay (the loading overlay) being up.
    inline ThrottleReason GetThrottleReason(bool iconified, bool afkOption, bool hasLevel,
                                            bool screenOrOverlay) {
        if (iconified) return ThrottleReason::WindowIconified;
        if (afkOption) {
            const int64_t afkTimeMillis =
                NowMillis() - Detail::LatestInputTime().load(std::memory_order_relaxed);
            if (afkTimeMillis > kLongAfkThresholdMs) return ThrottleReason::LongAfk;
            if (afkTimeMillis > kAfkThresholdMs)     return ThrottleReason::ShortAfk;
        }
        return (hasLevel || !screenOrOverlay) ? ThrottleReason::None : ThrottleReason::OutOfLevelMenu;
    }

    // MC getFramerateLimit. `framerateLimit` is the Max Framerate option.
    inline int GetFramerateLimit(int framerateLimit, ThrottleReason reason) {
        switch (reason) {
            case ThrottleReason::None:            return framerateLimit;
            case ThrottleReason::WindowIconified: return kIconifiedWindowLimit;
            case ThrottleReason::LongAfk:         return kLongAfkLimit;
            case ThrottleReason::ShortAfk:        return framerateLimit < kAfkLimit ? framerateLimit : kAfkLimit;
            case ThrottleReason::OutOfLevelMenu:  return kOutOfLevelMenuLimit;
        }
        return framerateLimit;
    }

    // MC isHeavilyThrottled.
    inline bool IsHeavilyThrottled(ThrottleReason reason) {
        return reason == ThrottleReason::WindowIconified || reason == ThrottleReason::LongAfk;
    }

    // Minecraft.renderFrame's frameLimiter step: below the unlimited cutoff,
    // hold the frame to 1/limit seconds. `deadline` is the caller's own
    // (one per frame loop). It advances by one budget a frame and drops to
    // `now` when the frame overran it, so a slow frame never sleeps a further
    // budget; sleep_until parks the thread instead of spinning.
    inline void LimitDisplayFps(int framerateLimit, std::chrono::steady_clock::time_point& deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (framerateLimit <= 0 || framerateLimit >= kUnlimitedFramerateCutoff) {
            deadline = now;   // unlimited: nothing to hold, and no stale deadline later
            return;
        }
        const auto frameBudget = std::chrono::nanoseconds(1'000'000'000LL / framerateLimit);
        deadline += frameBudget;
        if (deadline < now) {
            deadline = now;   // overran the budget: no sleep
        } else {
            std::this_thread::sleep_until(deadline);
        }
    }
} // namespace Client::FramerateLimitTracker
