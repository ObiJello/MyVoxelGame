#include "Input.hpp"
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

    // Scroll callback: accumulates scroll deltas
    static void ScrollCallback(GLFWwindow* /*window*/, double xoffset, double yoffset) {
        scrollX += xoffset;
        scrollY += yoffset;
    }

    // Mouse-motion callback: calculates deltaX/deltaY
    static void MouseCallback(GLFWwindow* /*window*/, double xpos, double ypos) {
        deltaX = xpos - lastX;
        deltaY = lastY - ypos; // invert Y so upward motion is positive dy
        lastX = xpos;
        lastY = ypos;
    }

    void Init(GLFWwindow* window) {
        gWindow = window;
        // Register callbacks
        glfwSetScrollCallback(gWindow, ScrollCallback);
        glfwSetCursorPosCallback(gWindow, MouseCallback);
        // Initialize lastX/lastY from the current cursor position
        glfwGetCursorPos(gWindow, &lastX, &lastY);
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
}