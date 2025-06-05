// File: src/platform/PlatformMain.cpp
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "Input.hpp"
#include "Log.hpp"
#include "Config.hpp"

// Include game headers
#include "../game/BlockRegistry.hpp"
#include "../game/ChunkProvider.hpp"

// Include rendering headers
#include "../render/Camera.hpp"
#include "../render/ChunkRenderer.hpp"
#include "../render/Frustum.hpp"
#include "../render/Shader.hpp"   // Shader::SetMat4

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace PlatformMain {

    // Callback for OpenGL debug messages
    void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam)
    {
        Log::Error("[GL DEBUG] %s", message);
    }

    int Run(int argc, char** argv)
    {
        // 1) Initialize logging
        Log::Init();
        Log::Info("Starting MyVoxelGame");

        // 2) Initialize BlockRegistry and request a test chunk at (0,0)
        Game::BlockRegistry::Init();
        Game::ChunkProvider::RequestChunk({0, 0});

        // 3) Initialize GLFW
        if (!glfwInit()) {
            Log::Error("Failed to initialize GLFW");
            return -1;
        }

        // 4) Request OpenGL context (version from Config)
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Config::OpenGLMajor);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Config::OpenGLMinor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    #endif
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);

        // 5) Create the window
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

        // 6) Initialize input (disable cursor for mouse‐look)
        Input::Init(window);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        // 7) Make the OpenGL context current and load GLAD
        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            Log::Error("Failed to initialize GLAD");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -3;
        }

        // 8) Print GPU/GL version
        Log::Info("Vendor:   %s", glGetString(GL_VENDOR));
        Log::Info("Renderer: %s", glGetString(GL_RENDERER));
        Log::Info("Version:  %s", glGetString(GL_VERSION));

    #ifndef NDEBUG
        if (GLAD_GL_KHR_debug) {
            glEnable(GL_DEBUG_OUTPUT);
            glDebugMessageCallback(glDebugOutput, nullptr);
            Log::Info("KHR_debug enabled");
        }
    #endif

        // 9) Compile + link the block shader
        Shader blockShader({
            "shaders/block.vert",
            "shaders/block.frag"
        });

        // 10) Enable depth testing
        glEnable(GL_DEPTH_TEST);

        // 11) Initialize camera
        Render::Camera camera;
        glfwSwapInterval(1); // vsync on

        // 12) Main loop
        while (!glfwWindowShouldClose(window)) {
            // a) Update time
            Time::Tick();
            float dt = static_cast<float>(Time::Delta());

            // b) Close on Escape
            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            // c) Update camera from input
            camera.Update(dt);

            // d) Clear screen & depth buffer
            glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // e) Pop finished MeshData and upload to GPU
            {
                Game::MeshData* meshPtr = nullptr;
                while (Game::PopMeshData(meshPtr)) {
                    blockShader.Use();
                    Render::UploadMesh(meshPtr);
                }
            }

            // f) Build projection and view matrices
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            float aspect = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);

            glm::mat4 proj = glm::perspective(
                glm::radians(camera.fov),
                aspect,
                0.1f,
                1000.0f
            );
            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 mvp  = proj * view;

            // g) Extract frustum planes from MVP
            Frustum frust = Frustum::FromMatrix(mvp);

            // h) Draw visible chunk meshes
            blockShader.Use();
            blockShader.SetMat4("uMVP", mvp);
            for (const auto& cm : Render::g_chunkMeshes) {
                AABB box = cm.GetAABB();
                if (!frust.IsBoxVisible(box)) {
                    continue;
                }
                cm.Draw();
            }

            // i) Poll events & swap buffers
            glfwPollEvents();
            glfwSwapBuffers(window);

            // j) Reset per-frame input deltas
            Input::ResetMouseDelta();
            Input::ResetScrollOffset();
        }

        // 13) Cleanup: delete VAOs/VBOs/EBOs
        for (auto& cm : Render::g_chunkMeshes) {
            glDeleteVertexArrays(1, &cm.vao);
            glDeleteBuffers(1, &cm.vbo);
            glDeleteBuffers(1, &cm.ebo);
        }

        Log::Info("Shutting down");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

} // namespace PlatformMain