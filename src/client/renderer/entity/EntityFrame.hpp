// File: src/client/renderer/entity/EntityFrame.hpp
//
// The frame serial the streaming-buffer renderers (mobs, players, XP orbs,
// block cubes, End portals, the GUI, ...) key their buffer versioning on.
// WHY it exists is spelled out in EntityCulling.hpp's "Frame serial" section
// — in short: the Vulkan and Metal backends' UpdateBuffer is an immediate
// host memcpy and draws run at submit, so a renderer keeps one buffer set
// per frame the GPU may still be reading while the CPU writes the next —
// Slots() of them, RenderBackend::FramesInFlight (2 on OpenGL and Vulkan,
// 3 on Metal) — and each call within a frame must append at a cursor rather
// than rewrite offset 0 (the portal pass calls every entity renderer again).
// Two sets with three frames in flight drew the entities and End portals
// from a buffer the next frame was already overwriting (2026-10-07).
#pragma once

#include <algorithm>
#include <cstdint>

namespace Render::EntityFrame {

    inline uint32_t g_serial = 0;

    // Once per frame, before the first entity renderer runs. MAIN THREAD.
    inline void Begin() { ++g_serial; }
    inline uint32_t Serial() { return g_serial; }

    // Buffer sets a streaming renderer keeps. kMaxSlots sizes the arrays;
    // Slots() is how many are created and cycled, set from the backend's
    // FramesInFlight when it is created (PlatformMain), before any renderer
    // initialises.
    inline constexpr int kMaxSlots = 3;
    inline int g_slots = 2;
    inline void SetSlots(int n) { g_slots = std::clamp(n, 1, kMaxSlots); }
    inline int  Slots() { return g_slots; }

    // A renderer's place in the ring: Advance() once per call; true on the
    // first call of a frame (the renderer resets its append cursor and moves
    // to the next buffer set), false on the portal re-entries of the same
    // frame (same set, append).
    struct Cursor {
        uint32_t lastSerial = 0;
        int      slot = 0;
        bool Advance() {
            const uint32_t s = Serial();
            if (s == 0 || s != lastSerial) {
                lastSerial = s;
                slot = (slot + 1) % Slots();
                return true;
            }
            return false;
        }
    };

} // namespace Render::EntityFrame
