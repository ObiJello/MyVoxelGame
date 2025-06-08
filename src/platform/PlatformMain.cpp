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

#ifndef NDEBUG
// Only include ImGui in debug builds
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#endif

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

        // 7) Make the OpenGL context current and load GLAD
        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            Log::Error("Failed to initialize GLAD");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -3;
        }

        // 6) Initialize input AFTER making context current (disable cursor for mouse‐look)
        Input::Init(window);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        // 8) Print GPU/GL version3
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
        // Place camera above the chunk so we can see terrain
        camera.position = glm::vec3(0.0f, 80.0f, 0.0f);
        camera.yaw      = 0.0f;  // look along +Z
        camera.pitch    = 0.0f;  // level
        glfwSwapInterval(1); // vsync on

    #ifndef NDEBUG
        // 12a) Setup Dear ImGui context (debug only)
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");
    #endif

        // 12) Main loop
        int logCounter = 0;
        while (!glfwWindowShouldClose(window)) {
            // a) Poll events FIRST to get fresh input data
            glfwPollEvents();

            // b) Update time
            Time::Tick();
            float dt = static_cast<float>(Time::Delta());

            // c) Close on Escape
            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            // d) Update camera from input (BEFORE resetting input deltas)
            camera.Update(dt);

    #ifndef NDEBUG
            // 12b) Start the ImGui frame (debug only)
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
    #endif

            // e) Clear screen & depth buffer
            glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // f) Pop finished MeshData and upload to GPU
            {
                Game::MeshData* meshPtr = nullptr;
                int uploadedThisFrame = 0;
                while (Game::PopMeshData(meshPtr) && meshPtr != nullptr) {
                    // Validate mesh data before uploading
                    Log::Debug("Processing mesh: chunk (%d,%d) section %d, vertices=%zu, indices=%zu",
                              meshPtr->chunkXZ.x, meshPtr->chunkXZ.y, meshPtr->sectionIndex,
                              meshPtr->vertices.size(), meshPtr->indices.size());

                    if (meshPtr->vertices.empty()) {
                        Log::Warning("Skipping upload of empty mesh for chunk (%d,%d) section %d",
                                    meshPtr->chunkXZ.x, meshPtr->chunkXZ.y, meshPtr->sectionIndex);
                        delete meshPtr;
                        continue;
                    }

                    // Check for corrupted coordinates
                    if (meshPtr->chunkXZ.x < -1000 || meshPtr->chunkXZ.x > 1000 ||
                        meshPtr->chunkXZ.y < -1000 || meshPtr->chunkXZ.y > 1000) {
                        Log::Error("Corrupted chunk coordinates detected: (%d,%d), skipping upload",
                                  meshPtr->chunkXZ.x, meshPtr->chunkXZ.y);
                        delete meshPtr;
                        continue;
                    }

                    blockShader.Use();
                    Render::UploadMesh(meshPtr);
                    uploadedThisFrame++;

                    Log::Info("Uploaded mesh for chunk (%d,%d) section %d with %zu vertices, %zu indices",
                             meshPtr->chunkXZ.x, meshPtr->chunkXZ.y, meshPtr->sectionIndex,
                             meshPtr->vertices.size(), meshPtr->indices.size());
                }

                if (uploadedThisFrame > 0) {
                    Log::Debug("Uploaded %d meshes this frame", uploadedThisFrame);
                }
            }

            // g) Build projection and view matrices
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

            // h) Draw visible chunk meshes - WITH PROPER MODEL MATRIX
            blockShader.Use();

            // Extract frustum from projection * view
            Frustum frust = Frustum::FromMatrix(proj * view);

            int renderedCount = 0;
            int totalChunks = static_cast<int>(Render::g_chunkMeshes.size());

            for (const auto& cm : Render::g_chunkMeshes) {
                // Get world-space AABB
                AABB box = cm.GetAABB();

                // Test frustum culling
                bool isVisible = frust.IsBoxVisible(box);
                if (!isVisible) {
                    continue; // Skip this chunk - it's outside the view frustum
                }

                // Create model matrix and MVP for rendering
                glm::mat4 model = glm::translate(glm::mat4(1.0f), cm.worldOffset);
                glm::mat4 mvp = proj * view * model;

                blockShader.SetMat4("uMVP", mvp);
                cm.Draw();
                renderedCount++;
            }

    #ifndef NDEBUG
            // i) Build ImGui debug window (debug only)
            {
                ImGui::Begin("Debug Info");
                float fps = 1.0f / static_cast<float>(Time::Delta());
                ImGui::Text("FPS: %.1f", fps);

                glm::vec3 camPos = camera.position;
                ImGui::Text("Camera: (%.1f, %.1f, %.1f)", camPos.x, camPos.y, camPos.z);
                ImGui::Text("Yaw: %.1f, Pitch: %.1f", camera.yaw, camera.pitch);

                ImGui::Text("Total chunk sections: %zu", Render::g_chunkMeshes.size());
                ImGui::Text("Rendered this frame: %d", renderedCount);

                // Show current mouse delta for debugging
                auto [dx, dy] = Input::GetMouseDelta();
                ImGui::Text("Mouse Delta: (%.2f, %.2f)", dx, dy);

                ImGui::End();
            }

            // j) Render ImGui on top (debug only)
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    #endif

            // k) Swap buffers
            glfwSwapBuffers(window);

            // l) Reset per-frame input deltas AFTER camera has used them
            Input::ResetMouseDelta();
            Input::ResetScrollOffset();
        }

    #ifndef NDEBUG
        // 13a) Cleanup ImGui (debug only)
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    #endif

        // 13b) Cleanup: delete VAOs/VBOs/EBOs
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