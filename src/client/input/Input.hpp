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
        Slash,
        Alpha1, Alpha2, Alpha3, Alpha4, Alpha5, Alpha6, Alpha7, Alpha8, Alpha9,
        F3,
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
}