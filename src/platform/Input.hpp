#pragma once

#include <utility>

// Forward declare GLFWwindow
struct GLFWwindow;

namespace Input {
    // Enumerate the keys your game cares about.
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
        Escape,
        LeftMouse,
        RightMouse,
        // Add more as needed
    };

    // Initialize the Input module with the GLFW window instance.
    // Must be called once after creating the window.
    void Init(GLFWwindow* window);

    // Returns true if the specified key is currently held down.
    bool IsKeyDown(Key key);

    // Returns true if the specified mouse button is held down.
    bool IsMouseButtonDown(Key mouseButton);

    // Returns mouse cursor position relative to the window (x, y) in screen coordinates.
    std::pair<double, double> GetMousePosition();

    // Returns the scroll wheel offsets since the last frame (xoffset, yoffset).
    std::pair<double, double> GetScrollOffset();

    // Call once per frame to reset the scroll offset.
    void ResetScrollOffset();
}
