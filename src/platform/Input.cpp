#include "Input.hpp"
#include <GLFW/glfw3.h>
#include <unordered_map>

namespace Input {
    static GLFWwindow* gWindow = nullptr;

    // Scroll offsets accumulate until ResetScrollOffset is called
    static double scrollX = 0.0;
    static double scrollY = 0.0;

    // Callback for scroll events
    static void ScrollCallback(GLFWwindow* /*window*/, double xoffset, double yoffset) {
        scrollX += xoffset;
        scrollY += yoffset;
    }

    void Init(GLFWwindow* window) {
        gWindow = window;
        // Register scroll callback
        glfwSetScrollCallback(gWindow, ScrollCallback);
    }

    bool IsKeyDown(Key key) {
        if (!gWindow) return false;
        int glfwKey;
        switch (key) {
            case Key::W:        glfwKey = GLFW_KEY_W;      break;
            case Key::A:        glfwKey = GLFW_KEY_A;      break;
            case Key::S:        glfwKey = GLFW_KEY_S;      break;
            case Key::D:        glfwKey = GLFW_KEY_D;      break;
            case Key::Up:       glfwKey = GLFW_KEY_UP;     break;
            case Key::Down:     glfwKey = GLFW_KEY_DOWN;   break;
            case Key::Left:     glfwKey = GLFW_KEY_LEFT;   break;
            case Key::Right:    glfwKey = GLFW_KEY_RIGHT;  break;
            case Key::Space:    glfwKey = GLFW_KEY_SPACE;  break;
            case Key::Escape:   glfwKey = GLFW_KEY_ESCAPE; break;
            default:            return false;
        }
        return glfwGetKey(gWindow, glfwKey) == GLFW_PRESS;
    }

    bool IsMouseButtonDown(Key mouseButton) {
        if (!gWindow) return false;
        int glfwButton;
        switch (mouseButton) {
            case Key::LeftMouse:  glfwButton = GLFW_MOUSE_BUTTON_LEFT;  break;
            case Key::RightMouse: glfwButton = GLFW_MOUSE_BUTTON_RIGHT; break;
            default:              return false;
        }
        return glfwGetMouseButton(gWindow, glfwButton) == GLFW_PRESS;
    }

    std::pair<double, double> GetMousePosition() {
        if (!gWindow) return {0.0, 0.0};
        double xpos, ypos;
        glfwGetCursorPos(gWindow, &xpos, &ypos);
        return {xpos, ypos};
    }

    std::pair<double, double> GetScrollOffset() {
        return { scrollX, scrollY };
    }

    void ResetScrollOffset() {
        scrollX = 0.0;
        scrollY = 0.0;
    }
}
