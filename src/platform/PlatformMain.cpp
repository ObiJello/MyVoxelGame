#include "PlatformMain.hpp"
#include "Time.hpp"
#include "Input.hpp"
#include "Log.hpp"
#include "Config.hpp"
#include "QuadRenderer.hpp"
#include "BlockRegistry.hpp"
#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace PlatformMain {

    // Callback for OpenGL errors (optional)
    void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam)
    {
        Log::Error("[GL DEBUG] %s", message);
    }

    int Run(int argc, char** argv)
    {
        // 1) Initialize logging first
        Log::Init();
        Log::Info("Starting MyVoxelGame");

        // 2) Initialize block registry
        Game::BlockRegistry::Init();

        // 3) Initialize GLFW
        if (!glfwInit()) {
            Log::Error("Failed to initialize GLFW");
            return -1;
        }

        // 4) Request an OpenGL context (version from Config)
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Config::OpenGLMajor);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Config::OpenGLMinor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    #endif
        // Enable debug context
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);

        // 5) Create the window (using Config)
        GLFWwindow* window = glfwCreateWindow(
            Config::WindowWidth,
            Config::WindowHeight,
            Config::WindowTitle,
            nullptr,
            nullptr
        );
        if (!window) {
            Log::Error("Failed to create GLFW window");
            glfwTerminate();
            return -2;
        }

        // Initialize input with the window
        Input::Init(window);

        // 6) Make the context current, load GLAD
        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            Log::Error("Failed to initialize GLAD");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -3;
        }

        // 7) Print GPU/GL version to verify
        Log::Info("Vendor:   %s", glGetString(GL_VENDOR));
        Log::Info("Renderer: %s", glGetString(GL_RENDERER));
        Log::Info("Version:  %s", glGetString(GL_VERSION));

        // 8) Enable debug output if available and in debug mode
    #ifndef NDEBUG
        if (GLAD_GL_KHR_debug) {
            glEnable(GL_DEBUG_OUTPUT);
            glDebugMessageCallback(glDebugOutput, nullptr);
            Log::Info("KHR_debug enabled");
        }
    #endif

        // 9) Initialize our quad renderer
        QuadRenderer quadRenderer;
        glfwSwapInterval(1); // vsync

        while (!glfwWindowShouldClose(window)) {
            // Update time
            Time::Tick();
            double dt = Time::Delta();

            // Close window on Escape
            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            // Draw the quad (which also clears with its own shader)
            quadRenderer.Draw();

            // Poll events & swap buffers
            glfwPollEvents();
            glfwSwapBuffers(window);

            // Reset per-frame input
            Input::ResetScrollOffset();
        }

        // 10) Cleanup
        Log::Info("Shutting down");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

} // namespace PlatformMain