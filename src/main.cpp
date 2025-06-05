#include <iostream>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

// Callback for OpenGL errors (optional)
void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                            const GLchar* message, const void* userParam)
{
    std::cerr << "[OpenGL Debug] " << message << "\n";
}

int main()
{
    // 1) Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }

    // 2) Request an OpenGL 3.3 core context (change to 4.1+ if you want)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // for macOS
#endif

    // 3) Create the window
    GLFWwindow* window = glfwCreateWindow(1280, 720, "MyVoxelGame v0.1", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return -2;
    }

    // 4) Make the context current, load GLAD
    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD\n";
        return -3;
    }

    // 5) Print GPU/GL version to verify
    std::cout << "Vendor:   " << glGetString(GL_VENDOR) << "\n";
    std::cout << "Renderer: " << glGetString(GL_RENDERER) << "\n";
    std::cout << "Version:  " << glGetString(GL_VERSION) << "\n";

    // 6) (Optional) Enable debug output if in debug mode
#ifndef NDEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(glDebugOutput, nullptr);
#endif

    // 7) Main loop: just clear the screen at 60 Hz until user closes
    glfwSwapInterval(1); // vsync
    while (!glfwWindowShouldClose(window)) {
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Poll events & swap buffers
        glfwPollEvents();
        glfwSwapBuffers(window);
    }

    // 8) Cleanup
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

