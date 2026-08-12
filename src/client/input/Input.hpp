// File:  src/client/input/Input.hpp
#pragma once

#include <utility>

// Forward declare GLFWwindow
struct GLFWwindow;

namespace Input {
    // Enumerate keys and mouse buttons your game cares about
    enum class Key {
        W,
        A,
        S,
        D,
        Up,
        Down,
        Left,
        Right,
        Space,
        LeftControl,
        Escape,
        LeftMouse,
        RightMouse,
        LeftShift,
        Tab,
        N,
        P,
        T,
        F,   // swap main/off hand (MC default)
        Q,   // drop held item (MC default)
        Slash,
        Alpha1, Alpha2, Alpha3, Alpha4, Alpha5, Alpha6, Alpha7, Alpha8, Alpha9,
        F3,
        F5,  // cycle camera perspective (MC default)
        F11,
        Tilde,
    };

    // Initialize the input system with a pointer to the GLFW window
    void Init(GLFWwindow* window);

    // Return true if the specified key is currently held down
    bool IsKeyDown(Key key);

    // Return true if the specified mouse button is held down
    bool IsMouseButtonDown(Key mouseButton);

    // Get the current cursor position (x, y) in window coordinates
    std::pair<double, double> GetMousePosition();

    // Get how far the mouse moved since the last frame (dx, dy)
    std::pair<double, double> GetMouseDelta();

    // Reset the accumulated mouse-delta values to zero; call once per frame
    void ResetMouseDelta();

    // Drop the delta AND re-arm the first-move guard, so the next cursor
    // position seeds the tracker instead of producing a delta against it.
    //
    // Required whenever the cursor's coordinate space changes underneath us:
    // GLFW_CURSOR_DISABLED reports virtual unbounded coordinates while
    // GLFW_CURSOR_NORMAL reports real window coordinates, so a delta measured
    // across that switch is the distance between two unrelated spaces. Feed
    // that to mouse-look and the view snaps somewhere arbitrary.
    //
    // MC does exactly this via MouseHandler.ignoreFirstMove, re-armed in
    // grabMouse (:407) and cursorEntered (:422) — not just once at startup.
    void ResetMouseTracking();

    // Get the scroll-wheel offsets since the last frame (xoffset, yoffset)
    std::pair<double, double> GetScrollOffset();

    // Reset the accumulated scroll offsets to zero; call once per frame
    void ResetScrollOffset();

    // Check if a key was just pressed this frame (not held)
    bool IsKeyPressed(Key key);

    // Call once per frame to update key press states
    void UpdateKeyStates();

    // Character input queue (for text entry — filled by glfwSetCharCallback)
    bool HasCharInput();
    unsigned int PopCharInput();

    // ========================================================================
    // EVENT-DRIVEN ACTION INPUT  (port of MC KeyMapping + MouseHandler.onButton)
    // ========================================================================
    //
    // Polling a button's level and diffing it against last frame cannot tell a
    // NEW press from a press that was already held — which is why a click that
    // dismissed a screen used to carry straight into the world and break the
    // block under the crosshair. MC avoids this structurally: its GLFW button
    // callback only writes gameplay state when no screen is open
    // (MouseHandler.java:131-146), so a press consumed by a screen is never
    // recorded at all, and the later release just re-confirms "up".
    //
    // This mirrors that. Presses arrive as EVENTS and are counted
    // (MC's KeyMapping.clickCount); gameplay drains discrete clicks with
    // ConsumeClick() and reads held state with IsDown().

    // Mirrors `minecraft.screen != null`. While true, button/key events are
    // delivered to the UI and do NOT touch gameplay state.
    void SetUiActive(bool active);
    bool IsUiActive();

    // MC KeyMapping.consumeClick — pops one queued press, false when empty.
    // Drain in a `while` loop so a burst of clicks in one frame all register.
    bool ConsumeClick(Key key);

    // MC KeyMapping.isDown — live held state, only ever set by events that
    // happened while the UI was inactive.
    bool IsDown(Key key);

    // MC KeyMapping.releaseAll — call when a screen OPENS. Drops every queued
    // click and clears held state, so nothing survives into or across the UI.
    void ReleaseAll();

    // MC KeyMapping.setAll, via MouseHandler.grabMouse — call when a screen
    // CLOSES. Re-reads physical state for KEYBOARD keys only; mouse buttons are
    // deliberately left up so a still-held click can't act on the world
    // (KeyMapping.shouldSetOnIngameFocus restricts this to Type.KEYSYM, and MC
    // skips it entirely on macOS: InputQuirks.RESTORE_KEY_STATE_AFTER_MOUSE_GRAB).
    void RestoreKeyboardState();

    // Key presses destined for the UI, queued while SetUiActive(true).
    //
    // Screens used to be fed a hardcoded whitelist of navigation keys polled
    // per frame, so a letter key never reached Screen::KeyPressed at all —
    // which is why the Key Binds screen could capture a mouse button but not a
    // keyboard key. MC delivers every key straight to `screen.keyPressed` from
    // its GLFW callback; this queue is that path. `mods` comes from GLFW rather
    // than being re-polled, so shift/ctrl state matches the press exactly.
    //
    // Returns false when the queue is empty. Drain it in a while loop.
    bool PopUiKeyPress(int& glfwKey, int& glfwMods);
}