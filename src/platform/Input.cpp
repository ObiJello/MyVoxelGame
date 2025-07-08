// File: src/platform/Input.cpp
#include "Input.hpp"
#include "Log.hpp"  // Add this include
#include <GLFW/glfw3.h>
#include <unordered_map>

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

    void Init(GLFWwindow* window) {
        gWindow = window;

        // Initialize lastX/lastY from the current cursor position
        glfwGetCursorPos(gWindow, &lastX, &lastY);

        // Register callbacks
        glfwSetScrollCallback(gWindow, ScrollCallback);
        glfwSetCursorPosCallback(gWindow, MouseCallback);

        // Initialize key state tracking
        previousKeyStates.clear();
        currentKeyStates.clear();
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
            case Key::Alpha1:      glfwKey = GLFW_KEY_1; break;
            case Key::Alpha2:      glfwKey = GLFW_KEY_2; break;
            case Key::Alpha3:      glfwKey = GLFW_KEY_3; break;
            case Key::Alpha4:      glfwKey = GLFW_KEY_4; break;
            case Key::Alpha5:      glfwKey = GLFW_KEY_5; break;
            case Key::Alpha6:      glfwKey = GLFW_KEY_6; break;
            case Key::Alpha7:      glfwKey = GLFW_KEY_7; break;
            case Key::Alpha8:      glfwKey = GLFW_KEY_8; break;
            case Key::Alpha9:      glfwKey = GLFW_KEY_9; break;
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
    }
}