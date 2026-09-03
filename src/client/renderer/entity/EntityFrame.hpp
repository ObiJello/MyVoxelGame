// File: src/client/renderer/entity/EntityFrame.hpp
//
// The frame serial the streaming-buffer entity renderers (mobs, players,
// XP orbs, block cubes) key their double-buffering on. WHY it exists is
// spelled out in EntityCulling.hpp's "Frame serial" section — in short: the
// Vulkan backend's UpdateBuffer is an immediate host memcpy and draws run at
// submit, so a buffer set must alternate per FRAME (two frames in flight)
// and each call within a frame must append at a cursor rather than rewrite
// offset 0 (the portal pass calls every entity renderer again).
#pragma once

#include <cstdint>

namespace Render::EntityFrame {

    inline uint32_t g_serial = 0;

    // Once per frame, before the first entity renderer runs. MAIN THREAD.
    inline void Begin() { ++g_serial; }
    inline uint32_t Serial() { return g_serial; }

    // Per-renderer bookkeeping. `Advance` answers which buffer set this call
    // writes (`parity`) and whether the set's cursor must restart (a new
    // frame) or continue (the same frame, a later call). If Begin is never
    // called the serial stays 0 and every call counts as a new frame — the
    // per-call parity flip GuiRenderer uses, correct for once-per-frame
    // renderers.
    struct Cursor {
        uint32_t lastSerial = 0;
        int      parity = 0;
        bool Advance() {
            const uint32_t s = Serial();
            if (s == 0 || s != lastSerial) {
                lastSerial = s;
                parity ^= 1;
                return true;
            }
            return false;
        }
    };

} // namespace Render::EntityFrame
