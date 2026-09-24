// File:  src/client/input/Input.hpp
#pragma once

#include <utility>
#include <string>

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
        LeftAlt,   // fill tool modifier (Option on a Mac)
        Escape,
        LeftMouse,
        RightMouse,
        LeftShift,
        Tab,
        N,
        P,
        T,
        F,   // swap main/off hand (MC default)
        C,   // second half of the F+C debug free-camera chord (raw key, no bind)
        Q,   // drop held item (MC default)
        Slash,
        Alpha1, Alpha2, Alpha3, Alpha4, Alpha5, Alpha6, Alpha7, Alpha8, Alpha9,
        F3,
        F5,  // cycle camera perspective (MC default)
        F8,  // culling diagnostics: dump view-ray section states to the log
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

    // The system clipboard, for text fields (Cmd/Ctrl+V and +C).
    std::string GetClipboardText();
    void SetClipboardText(const std::string& text);

    // Check if a key was just pressed this frame (not held)
    bool IsKeyPressed(Key key);

    // Call once per frame to update key press states
    void UpdateKeyStates();

    // Character input queue (for text entry — filled by glfwSetCharCallback)
    bool HasCharInput();
    unsigned int PopCharInput();

    // ── Raw key events (MC KeyboardHandler.keyPress) ────────────────────
    // Every keyboard PRESS and RELEASE, in order, screen or no screen, with
    // the GLFW key code and modifier bits. This is what the F3 debug chords
    // consume: a chord is "a key pressed while F3 is held" and needs the
    // press EVENT (not a polled level) and the F3 RELEASE (to know whether
    // the modifier was used as one). Repeats are not queued. Drained once
    // per frame by the debug key handler; capped so an unattended queue
    // cannot grow.
    struct RawKeyEvent {
        int glfwKey = 0;
        int action  = 0;   // GLFW_PRESS or GLFW_RELEASE
        int mods    = 0;
    };
    bool PopRawKeyEvent(RawKeyEvent& out);
    void ClearRawKeyEvents();
    // Live level of a physical key by GLFW code (glfwGetKey).
    bool IsGlfwKeyDown(int glfwKey);

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
    // Drop every queued UI key press. Chat and the inventory take their
    // keys by other routes and never drain this queue, so what was typed
    // in them (Tab, Space, Enter) used to sit here and replay into the
    // next screen that opened — a pause menu that focused and pressed its
    // own "Save and Quit to Title" button out of a chat line.
    void ClearUiKeyPresses();

    // Escape, from the key callback: true once per physical press since the
    // last call. Polling glfwGetKey missed a tap shorter than a frame, which
    // at portal-view frame times meant pressing it twice.
    bool ConsumeEscapePress();

    // Live level of a mouse button by GLFW code (glfwGetMouseButton).
    bool IsGlfwMouseButtonDown(int glfwButton);

    // ========================================================================
    // REMOTE INPUT  (/control — see common/network/packets/game/ControlPackets.hpp)
    // ========================================================================
    //
    // A controlled client's input comes from another player's window. In
    // remote mode this window's own GLFW callbacks are ignored and the
    // controller's events are pushed through the SAME handlers the callbacks
    // use, so every consumer above — bindings, click queues, the raw key
    // stream, the UI key queue, chars, scroll, mouse deltas — sees them as
    // if they had happened here. Level reads (IsKeyDown, IsGlfwKeyDown,
    // IsGlfwMouseButtonDown, GetMousePosition) answer from a shadow of the
    // remote state instead of asking GLFW.
    void SetRemoteMode(bool remote);
    bool IsRemoteMode();
    void RemoteKey(int glfwKey, int action, int mods);
    void RemoteMouseButton(int glfwButton, int action, int mods);
    void RemoteChar(unsigned int codepoint);
    // Raw offsets; the scroll settings are applied here as the callback would.
    void RemoteScroll(double xoffset, double yoffset);
    // Already in GetMouseDelta's form (dy positive = up).
    void RemoteMotion(double dx, double dy);
    // Every remote key and button up, every queued click dropped.
    void RemoteReleaseAll();

    // What the controlled player keeps of their OWN window while in remote
    // mode: Escape (their pause menu), the cursor-toggle binding (Tab), and
    // the OS mouse for that menu. Everything else of theirs is ignored.
    bool ConsumeLocalEscapePress();
    std::pair<double, double> GetLocalMousePosition();   // glfwGetCursorPos, never the override
    bool IsLocalMouseButtonDown(int glfwButton);          // glfwGetMouseButton, never the shadow

    // A cursor position that GetMousePosition returns instead of GLFW's:
    // the controller's virtual cursor on both sides (the controller's own
    // OS cursor stays captured for mouse-look; the controlled window's is
    // not the one being moved).
    void SetCursorOverride(bool enabled, double x, double y);
    bool HasCursorOverride();

    // Event capture, the controller's half: while enabled, every key,
    // mouse-button, char and scroll event this window receives is ALSO
    // queued here (before any UI gating), to be sent as the frame's
    // ControlInput. Local handling is unaffected.
    struct CapturedEvent {
        enum class Kind : uint8_t { Key, MouseButton, Char, Scroll };
        Kind   kind = Kind::Key;
        int    a = 0;      // key / button / codepoint
        int    b = 0;      // action
        int    c = 0;      // mods
        double x = 0.0;    // scroll offsets
        double y = 0.0;
    };
    void SetCaptureEvents(bool enabled);
    bool PopCapturedEvent(CapturedEvent& out);
}