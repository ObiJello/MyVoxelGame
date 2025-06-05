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
        RightMouse
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
}