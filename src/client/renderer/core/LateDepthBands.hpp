#pragma once

// Late depth bands: what the viewmodels and GUI items do instead of clearing
// the frame's depth, on the frames whose depth the next frame reads.
//
// The hand, the portal gun and the GUI's 3D items each clear the frame's
// depth before drawing so nothing in the world can cover them (MC
// GameRenderer's clear before renderItemInHand). That leaves the frame
// ending on a depth buffer that is no longer the world's — and the rain of
// the next frame tests against exactly that depth (WeatherEffectRenderer,
// Rain Resolution: Half on Vulkan, drawn ahead of the frame's pass from the
// previous frame's depth). So on those frames each of them draws into its
// own thin viewport depth range near 0 instead, depth test unchanged:
//
//     GUI items [0, w)   hand [w, 2w)   portal gun [2w, 3w)   world [3w, 1]
//
// Nearer wins, so the order is the clears' (the gun draws first, then the
// hand over it, then the GUI over both), and the world can't reach the
// bands short of the near plane (3w = 0.03 is within ~1.5 mm of it at the
// default 5 cm). The rain reads any stored depth below 3w as no occluder,
// so last frame's hand or hotbar never cuts a hole in this frame's rain.
//
// Active on every Metal frame (WeatherEffectRenderer::RenderAhead marks
// it; OBEY_DEPTH_CLEARS=1 restores the clears): the three full-screen depth
// clears they replace cost 0.22 ms serial and −9..19 % of the live GPU frame
// on the M4 (Xcode + ABBA, 2026-10-07), a band is a viewport change. On
// Vulkan only while the half-res rain reads the depth (MoltenVK's clears
// measured no worse than the bands); OpenGL clears as it always has. w is
// 0.01 on a float depth buffer and 0.05 on a 24-bit one.

#include "client/renderer/backend/RenderBackend.hpp"

#include <cstdint>

namespace Render::LateDepthBands {

    enum class Layer : uint8_t { Gui = 0, Hand = 1, PortalGun = 2 };

    namespace Detail {
        inline uint64_t g_activeFrame = 0;   // RenderBackend::FrameNumber() of the marked frame
        inline float    g_width       = 0.01f;
    }

    // The weather renderer, once a frame before anything draws.
    inline void MarkFrame(bool active, bool floatDepth) {
        Detail::g_activeFrame = (active && g_renderBackend) ? g_renderBackend->FrameNumber() : 0;
        Detail::g_width = floatDepth ? 0.01f : 0.05f;
    }

    inline bool Active() {
        return Detail::g_activeFrame != 0 && g_renderBackend &&
               g_renderBackend->FrameNumber() == Detail::g_activeFrame;
    }

    inline float Width() { return Detail::g_width; }

    // Depth below which the frame's stored depth is a band, not the world.
    inline float WorldNear() { return 3.0f * Detail::g_width; }

    // Instead of the layer's depth clear: true if the bands are active this
    // frame — the caller then skips its clear, draws, and calls End().
    inline bool Begin(Layer layer) {
        if (!Active()) return false;
        const float w = Detail::g_width;
        const float lo = static_cast<float>(static_cast<int>(layer)) * w;
        g_renderBackend->SetDepthRange(lo, lo + w);
        return true;
    }

    inline void End() {
        if (g_renderBackend) g_renderBackend->SetDepthRange(0.0f, 1.0f);
    }

} // namespace Render::LateDepthBands
