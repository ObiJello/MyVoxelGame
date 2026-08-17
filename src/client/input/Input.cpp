// File: src/client/input/Input.cpp
#include "Input.hpp"
#include "KeyMapping.hpp"
#include "common/core/Log.hpp"
#include <GLFW/glfw3.h>
#include <unordered_map>
#include <queue>
#include <deque>

namespace Input {
    static GLFWwindow* gWindow = nullptr;

    // Accumulated scroll offsets (reset each frame)
    static double scrollX = 0.0;
    static double scrollY = 0.0;

    // Last-known cursor position
    static double lastX = 0.0;
    static double lastY = 0.0;

    // Frame-to-frame mouse movement
    static double deltaX = 0.0;
    static double deltaY = 0.0;

    // Add a flag to track if this is the first mouse callback
    static bool firstMouse = true;

    // Key press tracking for single-frame press detection
    static std::unordered_map<Key, bool> previousKeyStates;
    static std::unordered_map<Key, bool> currentKeyStates;

    // Character input queue for text entry
    static std::queue<unsigned int> charInputQueue;

    // ── Event-driven action state (MC KeyMapping) ───────────────────────────
    // `down` is the live held state; `clickCount` is the queue of presses not
    // yet consumed. Both are written ONLY from the GLFW callbacks, and only
    // while no UI is active — see the header for why that matters.
    struct ActionState {
        bool down = false;
        int  clickCount = 0;
    };
    static std::unordered_map<Key, ActionState> actionStates;
    static bool uiActive = false;

    // Key presses captured for the UI while a screen is up (see PopUiKeyPress).
    struct UiKeyPress { int key; int mods; };
    static std::deque<UiKeyPress> uiKeyPresses;
    // A screen that stops draining (or a stuck uiActive) must not grow this
    // without bound; 64 is far more than any frame can produce.
    static constexpr size_t kMaxUiKeyPresses = 64;

    // GLFW code -> our Key, for the callbacks. The forward direction already
    // exists in IsKeyDown/IsMouseButtonDown as a switch; this is its inverse
    // and must stay in step with it.
    static bool KeyFromGlfwKey(int glfwKey, Key& out) {
        switch (glfwKey) {
            case GLFW_KEY_W:            out = Key::W; return true;
            case GLFW_KEY_A:            out = Key::A; return true;
            case GLFW_KEY_S:            out = Key::S; return true;
            case GLFW_KEY_D:            out = Key::D; return true;
            case GLFW_KEY_UP:           out = Key::Up; return true;
            case GLFW_KEY_DOWN:         out = Key::Down; return true;
            case GLFW_KEY_LEFT:         out = Key::Left; return true;
            case GLFW_KEY_RIGHT:        out = Key::Right; return true;
            case GLFW_KEY_SPACE:        out = Key::Space; return true;
            case GLFW_KEY_LEFT_CONTROL: out = Key::LeftControl; return true;
            case GLFW_KEY_ESCAPE:       out = Key::Escape; return true;
            case GLFW_KEY_LEFT_SHIFT:   out = Key::LeftShift; return true;
            case GLFW_KEY_TAB:          out = Key::Tab; return true;
            case GLFW_KEY_N:            out = Key::N; return true;
            case GLFW_KEY_P:            out = Key::P; return true;
            case GLFW_KEY_T:            out = Key::T; return true;
            case GLFW_KEY_F:            out = Key::F; return true;
            case GLFW_KEY_Q:            out = Key::Q; return true;
            case GLFW_KEY_SLASH:        out = Key::Slash; return true;
            case GLFW_KEY_1:            out = Key::Alpha1; return true;
            case GLFW_KEY_2:            out = Key::Alpha2; return true;
            case GLFW_KEY_3:            out = Key::Alpha3; return true;
            case GLFW_KEY_4:            out = Key::Alpha4; return true;
            case GLFW_KEY_5:            out = Key::Alpha5; return true;
            case GLFW_KEY_6:            out = Key::Alpha6; return true;
            case GLFW_KEY_7:            out = Key::Alpha7; return true;
            case GLFW_KEY_8:            out = Key::Alpha8; return true;
            case GLFW_KEY_9:            out = Key::Alpha9; return true;
            case GLFW_KEY_F3:           out = Key::F3; return true;
            case GLFW_KEY_F5:           out = Key::F5; return true;
            case GLFW_KEY_F11:          out = Key::F11; return true;
            case GLFW_KEY_GRAVE_ACCENT: out = Key::Tilde; return true;
            default: return false;
        }
    }

    // Only consulted inside the non-Apple restore path below.
    [[maybe_unused]] static bool IsMouseKey(Key k) {
        return k == Key::LeftMouse || k == Key::RightMouse;
    }

    // MC MouseHandler.onButton / KeyMapping.set + KeyMapping.click.
    // The UI check is the whole point: a press that belongs to a screen must
    // never reach gameplay state, so that when the screen closes there is no
    // held-down flag and no queued click waiting to fire.
    static void RecordAction(Key key, bool pressed) {
        if (uiActive) return;
        ActionState& st = actionStates[key];
        st.down = pressed;
        if (pressed) ++st.clickCount;
    }

    // MC KeyMapping.forAllKeyMappings — dispatch to EVERY mapping bound to this
    // physical input, not just the first. Two actions sharing a key is exactly
    // what the Controls screen reports as a conflict, and both must still fire.
    static void RecordBoundKey(BoundKey physical, bool pressed) {
        if (uiActive || !physical.IsBound()) return;
        for (KeyMapping* m : AllKeyMappings()) {
            if (m->key != physical) continue;
            m->down = pressed;
            if (pressed) ++m->clickCount;
        }
    }

    static void MouseButtonCallback(GLFWwindow* /*window*/, int button, int action, int /*mods*/) {
        const bool pressed = (action == GLFW_PRESS);
        if (!pressed && action != GLFW_RELEASE) return;

        RecordBoundKey(BoundKey::Mouse(button), pressed);

        // Legacy per-Key path, still used by call sites that haven't moved to
        // named bindings.
        Key key;
        if (button == GLFW_MOUSE_BUTTON_LEFT)       key = Key::LeftMouse;
        else if (button == GLFW_MOUSE_BUTTON_RIGHT) key = Key::RightMouse;
        else return;
        RecordAction(key, pressed);
    }

    static void KeyCallback(GLFWwindow* /*window*/, int glfwKey, int /*scancode*/,
                            int action, int mods) {
        // While a screen is up the press belongs to the UI, not the world.
        if (uiActive) {
            // Arrows honour auto-repeat, so holding one keeps nudging a
            // focused slider instead of demanding a press per step (MC
            // forwards PRESS and REPEAT to Screen.keyPressed). Every other
            // key stays one-shot on purpose: ScreenManager defers stack ops
            // to the next frame, so a repeating ESC would queue several pops
            // out of one hold and blow through the screen stack.
            const bool arrow = glfwKey == GLFW_KEY_LEFT || glfwKey == GLFW_KEY_RIGHT ||
                               glfwKey == GLFW_KEY_UP   || glfwKey == GLFW_KEY_DOWN;
            const bool wanted = action == GLFW_PRESS ||
                                (action == GLFW_REPEAT && arrow);
            if (wanted && uiKeyPresses.size() < kMaxUiKeyPresses) {
                uiKeyPresses.push_back({glfwKey, mods});
            }
            return;
        }

        // GLFW_REPEAT deliberately ignored — MC queues one click per physical
        // press; auto-repeat is a text-entry concern, not an action one.
        const bool pressed = (action == GLFW_PRESS);
        if (!pressed && action != GLFW_RELEASE) return;

        RecordBoundKey(BoundKey::Keyboard(glfwKey), pressed);

        Key key;
        if (!KeyFromGlfwKey(glfwKey, key)) return;
        RecordAction(key, pressed);
    }

    // Character callback: queues typed characters
    static void CharCallback(GLFWwindow* /*window*/, unsigned int codepoint) {
        charInputQueue.push(codepoint);
    }

    // Scroll callback: accumulates scroll deltas
    static void ScrollCallback(GLFWwindow* /*window*/, double xoffset, double yoffset) {
        scrollX += xoffset;
        scrollY += yoffset;
    }

    // Mouse-motion callback: calculates deltaX/deltaY
    static void MouseCallback(GLFWwindow* /*window*/, double xpos, double ypos) {
        // Skip the first mouse callback to avoid a large jump
        if (firstMouse) {
            lastX = xpos;
            lastY = ypos;
            firstMouse = false;
            return;
        }

        deltaX = xpos - lastX;
        deltaY = lastY - ypos; // invert Y so upward motion is positive dy

        lastX = xpos;
        lastY = ypos;
    }

    // MC MouseHandler.cursorEntered (:422). While the cursor is free it can
    // leave the window and come back somewhere entirely different — alt-tab
    // away, move the mouse, come back — and the first position reported after
    // that has no relationship to the last one we saw.
    static void CursorEnterCallback(GLFWwindow* /*window*/, int entered) {
        if (entered) ResetMouseTracking();
    }

    void Init(GLFWwindow* window) {
        gWindow = window;

        // Initialize lastX/lastY from the current cursor position
        glfwGetCursorPos(gWindow, &lastX, &lastY);

        // Register callbacks
        glfwSetScrollCallback(gWindow, ScrollCallback);
        glfwSetCursorPosCallback(gWindow, MouseCallback);
        glfwSetCursorEnterCallback(gWindow, CursorEnterCallback);
        glfwSetCharCallback(gWindow, CharCallback);
        glfwSetMouseButtonCallback(gWindow, MouseButtonCallback);
        // Installed BEFORE ImGui's backend comes up, so ImGui captures this as
        // its chain target and forwards every key down to us. Nothing filters
        // anything out of that chain — Tab included.
        glfwSetKeyCallback(gWindow, KeyCallback);

        // Initialize key state tracking
        previousKeyStates.clear();
        currentKeyStates.clear();
        actionStates.clear();
        uiKeyPresses.clear();
        uiActive = false;
    }

    // ── Event-driven action API (MC KeyMapping) ─────────────────────────────

    void SetUiActive(bool active) { uiActive = active; }
    bool IsUiActive()             { return uiActive; }

    bool PopUiKeyPress(int& glfwKey, int& glfwMods) {
        if (uiKeyPresses.empty()) return false;
        glfwKey  = uiKeyPresses.front().key;
        glfwMods = uiKeyPresses.front().mods;
        uiKeyPresses.pop_front();
        return true;
    }

    bool ConsumeClick(Key key) {
        auto it = actionStates.find(key);
        if (it == actionStates.end() || it->second.clickCount == 0) return false;
        --it->second.clickCount;
        return true;
    }

    bool IsDown(Key key) {
        auto it = actionStates.find(key);
        return it != actionStates.end() && it->second.down;
    }

    void ReleaseAll() {
        // MC KeyMapping.releaseAll: clickCount = 0 AND setDown(false).
        for (auto& [key, st] : actionStates) {
            st.down = false;
            st.clickCount = 0;
        }
        // MC releaseAll walks every KeyMapping; ours must too or a binding
        // would keep its held state across a screen.
        for (KeyMapping* m : AllKeyMappings()) {
            m->down = false;
            m->clickCount = 0;
        }
        // Also flush the legacy polled edges, so a key held while a screen
        // opens doesn't read as freshly pressed to IsKeyPressed() callers.
        previousKeyStates = currentKeyStates;
    }

    void RestoreKeyboardState() {
        // MC KeyMapping.setAll, reached from MouseHandler.grabMouse. Only
        // Type.KEYSYM mappings are restored (shouldSetOnIngameFocus), so a
        // held mouse button stays UP — that asymmetry is what stops the click
        // that dismissed a screen from acting on the world.
#ifndef __APPLE__
        // MC gates this on InputQuirks.RESTORE_KEY_STATE_AFTER_MOUSE_GRAB,
        // which is `!ON_OSX`; match it so held-WASD behaves identically here.
        for (auto& [key, st] : actionStates) {
            if (IsMouseKey(key)) continue;
            st.down = IsKeyDown(key);
        }
        for (KeyMapping* m : AllKeyMappings()) {
            if (m->key.type != BoundKey::Type::Keyboard) continue;   // mouse stays up
            m->down = gWindow && glfwGetKey(gWindow, m->key.code) == GLFW_PRESS;
        }
#endif
        // Re-sync the polled edge maps either way so closing a screen never
        // manufactures a rising edge for something already held.
        UpdateKeyStates();
        previousKeyStates = currentKeyStates;
    }

    bool IsKeyDown(Key key) {
        if (!gWindow) return false;
        int glfwKey;
        switch (key) {
            case Key::W:           glfwKey = GLFW_KEY_W; break;
            case Key::A:           glfwKey = GLFW_KEY_A; break;
            case Key::S:           glfwKey = GLFW_KEY_S; break;
            case Key::D:           glfwKey = GLFW_KEY_D; break;
            case Key::Up:          glfwKey = GLFW_KEY_UP; break;
            case Key::Down:        glfwKey = GLFW_KEY_DOWN; break;
            case Key::Left:        glfwKey = GLFW_KEY_LEFT; break;
            case Key::Right:       glfwKey = GLFW_KEY_RIGHT; break;
            case Key::Space:       glfwKey = GLFW_KEY_SPACE; break;
            case Key::LeftControl: glfwKey = GLFW_KEY_LEFT_CONTROL; break;
            case Key::Escape:      glfwKey = GLFW_KEY_ESCAPE; break;
            case Key::LeftShift:   glfwKey = GLFW_KEY_LEFT_SHIFT; break;
            case Key::Tab:         glfwKey = GLFW_KEY_TAB; break;
            case Key::N:           glfwKey = GLFW_KEY_N; break;
            case Key::P:           glfwKey = GLFW_KEY_P; break;
            case Key::T:           glfwKey = GLFW_KEY_T; break;
            case Key::F:           glfwKey = GLFW_KEY_F; break;
            case Key::Q:           glfwKey = GLFW_KEY_Q; break;
            case Key::Slash:       glfwKey = GLFW_KEY_SLASH; break;
            case Key::Alpha1:      glfwKey = GLFW_KEY_1; break;
            case Key::Alpha2:      glfwKey = GLFW_KEY_2; break;
            case Key::Alpha3:      glfwKey = GLFW_KEY_3; break;
            case Key::Alpha4:      glfwKey = GLFW_KEY_4; break;
            case Key::Alpha5:      glfwKey = GLFW_KEY_5; break;
            case Key::Alpha6:      glfwKey = GLFW_KEY_6; break;
            case Key::Alpha7:      glfwKey = GLFW_KEY_7; break;
            case Key::Alpha8:      glfwKey = GLFW_KEY_8; break;
            case Key::Alpha9:      glfwKey = GLFW_KEY_9; break;
            case Key::F3:          glfwKey = GLFW_KEY_F3; break;
            case Key::F5:          glfwKey = GLFW_KEY_F5; break;
            case Key::F11:         glfwKey = GLFW_KEY_F11; break;
            case Key::Tilde:       glfwKey = GLFW_KEY_GRAVE_ACCENT; break;
            default: return false;
        }
        return glfwGetKey(gWindow, glfwKey) == GLFW_PRESS;
    }

    bool IsMouseButtonDown(Key mouseButton) {
        if (!gWindow) return false;
        int glfwButton;
        switch (mouseButton) {
            case Key::LeftMouse:  glfwButton = GLFW_MOUSE_BUTTON_LEFT;  break;
            case Key::RightMouse: glfwButton = GLFW_MOUSE_BUTTON_RIGHT; break;
            default: return false;
        }
        return glfwGetMouseButton(gWindow, glfwButton) == GLFW_PRESS;
    }

    std::pair<double, double> GetMousePosition() {
        if (!gWindow) return {0.0, 0.0};
        double xpos, ypos;
        glfwGetCursorPos(gWindow, &xpos, &ypos);
        return { xpos, ypos };
    }

    std::pair<double, double> GetMouseDelta() {
        return { deltaX, deltaY };
    }

    void ResetMouseDelta() {
        deltaX = 0.0;
        deltaY = 0.0;
    }

    void ResetMouseTracking() {
        // firstMouse makes the next MouseCallback re-seed lastX/lastY and
        // return without emitting a delta — the same skip that runs once at
        // startup, which is all it used to do.
        firstMouse = true;
        deltaX = 0.0;
        deltaY = 0.0;
    }

    std::pair<double, double> GetScrollOffset() {
        return { scrollX, scrollY };
    }

    void ResetScrollOffset() {
        scrollX = 0.0;
        scrollY = 0.0;
    }

    bool IsKeyPressed(Key key) {
        // Key is "pressed" if it's currently down but wasn't down last frame
        auto currentIt = currentKeyStates.find(key);
        auto previousIt = previousKeyStates.find(key);

        bool currentlyDown = (currentIt != currentKeyStates.end()) ? currentIt->second : false;
        bool previouslyDown = (previousIt != previousKeyStates.end()) ? previousIt->second : false;

        return currentlyDown && !previouslyDown;
    }

    void UpdateKeyStates() {
        // Update previous states
        previousKeyStates = currentKeyStates;

        // Update current states
        currentKeyStates[Key::W] = IsKeyDown(Key::W);
        currentKeyStates[Key::A] = IsKeyDown(Key::A);
        currentKeyStates[Key::S] = IsKeyDown(Key::S);
        currentKeyStates[Key::D] = IsKeyDown(Key::D);
        currentKeyStates[Key::Up] = IsKeyDown(Key::Up);
        currentKeyStates[Key::Down] = IsKeyDown(Key::Down);
        currentKeyStates[Key::Left] = IsKeyDown(Key::Left);
        currentKeyStates[Key::Right] = IsKeyDown(Key::Right);
        currentKeyStates[Key::Space] = IsKeyDown(Key::Space);
        currentKeyStates[Key::LeftControl] = IsKeyDown(Key::LeftControl);
        currentKeyStates[Key::Escape] = IsKeyDown(Key::Escape);
        currentKeyStates[Key::LeftShift] = IsKeyDown(Key::LeftShift);
        currentKeyStates[Key::Tab] = IsKeyDown(Key::Tab);
        currentKeyStates[Key::N] = IsKeyDown(Key::N);
        currentKeyStates[Key::P] = IsKeyDown(Key::P);
        currentKeyStates[Key::T] = IsKeyDown(Key::T);
        currentKeyStates[Key::F] = IsKeyDown(Key::F);
        currentKeyStates[Key::Q] = IsKeyDown(Key::Q);
        currentKeyStates[Key::Slash] = IsKeyDown(Key::Slash);
        currentKeyStates[Key::Alpha1] = IsKeyDown(Key::Alpha1);
        currentKeyStates[Key::Alpha2] = IsKeyDown(Key::Alpha2);
        currentKeyStates[Key::Alpha3] = IsKeyDown(Key::Alpha3);
        currentKeyStates[Key::Alpha4] = IsKeyDown(Key::Alpha4);
        currentKeyStates[Key::Alpha5] = IsKeyDown(Key::Alpha5);
        currentKeyStates[Key::Alpha6] = IsKeyDown(Key::Alpha6);
        currentKeyStates[Key::Alpha7] = IsKeyDown(Key::Alpha7);
        currentKeyStates[Key::Alpha8] = IsKeyDown(Key::Alpha8);
        currentKeyStates[Key::Alpha9] = IsKeyDown(Key::Alpha9);
        currentKeyStates[Key::LeftMouse] = IsMouseButtonDown(Key::LeftMouse);
        currentKeyStates[Key::RightMouse] = IsMouseButtonDown(Key::RightMouse);
        currentKeyStates[Key::F3] = IsKeyDown(Key::F3);
        currentKeyStates[Key::F5] = IsKeyDown(Key::F5);
        currentKeyStates[Key::F11] = IsKeyDown(Key::F11);
        currentKeyStates[Key::Tilde] = IsKeyDown(Key::Tilde);
    }

    bool HasCharInput() {
        return !charInputQueue.empty();
    }

    unsigned int PopCharInput() {
        if (charInputQueue.empty()) return 0;
        unsigned int c = charInputQueue.front();
        charInputQueue.pop();
        return c;
    }
}