// File: src/client/world/HushStillnessState.hpp
//
// The client's copy of the Hush's stillness (server/level/HushStillness.hpp),
// as last reported by HushStillnessS2C. Written on the network I/O thread
// (ClientConnection's handler — a raw handler, like ServerPausedS2C, because
// all it does is store two numbers), read on the render thread by
// Render::HushAtmosphere, which turns the on/off edge into the eased look.
//
// `serial` bumps on every packet so the reader sees a restart (a /stillness
// while one is on) as a fresh deadline even though `active` did not change.
#pragma once

#include <atomic>
#include <cstdint>

namespace Client::HushStillnessState {

    inline std::atomic<bool>     g_active{false};
    inline std::atomic<uint32_t> g_remainingTicks{0};
    inline std::atomic<uint32_t> g_serial{0};

    // Network I/O thread.
    inline void OnPacket(bool active, uint32_t remainingTicks) {
        g_remainingTicks.store(remainingTicks, std::memory_order_relaxed);
        g_active.store(active, std::memory_order_relaxed);
        g_serial.fetch_add(1, std::memory_order_release);
    }

} // namespace Client::HushStillnessState
