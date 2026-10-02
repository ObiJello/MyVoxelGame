// File: src/client/renderer/core/WorldFramebuffer.hpp
//
// The size the LEVEL is drawn at this frame (Video Settings → Render
// Resolution). At 100 % that is the window's framebuffer; at any other scale
// the level draws into the backend's scaled scene (RenderBackend::
// RequestScaledScene — MC's mainTarget) and only the GUI sees the window's
// size. The main loop publishes both sizes once a frame, before the level,
// and the window's again once the scene is resolved.
//
// A renderer that needs "the framebuffer size" while drawing the level —
// a line width in pixels, a screen-sized copy, an aspect ratio — reads it
// here, never from glfwGetFramebufferSize: the window's size is the wrong
// one whenever the scale is not 100 %. Get returns false before the first
// frame publishes anything; callers fall back to the window then.
#pragma once

namespace Render::WorldFramebuffer {

    struct State {
        int width = 0, height = 0;               // the level's target this frame
        int windowWidth = 0, windowHeight = 0;   // the window's framebuffer
    };
    inline State g_state;

    inline void Set(int width, int height, int windowWidth, int windowHeight) {
        g_state = State{width, height, windowWidth, windowHeight};
    }

    inline bool Get(int& width, int& height) {
        if (g_state.width <= 0 || g_state.height <= 0) return false;
        width = g_state.width;
        height = g_state.height;
        return true;
    }

    // Level pixels per window pixel along X (1 when nothing is published):
    // what a size given in window pixels (a line width) is multiplied by to
    // look the same once the scene is stretched over the window.
    inline float Scale() {
        if (g_state.width <= 0 || g_state.windowWidth <= 0) return 1.0f;
        return static_cast<float>(g_state.width) / static_cast<float>(g_state.windowWidth);
    }

} // namespace Render::WorldFramebuffer
