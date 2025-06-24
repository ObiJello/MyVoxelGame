// File: src/platform/PlatformMain.cpp (Enhanced with Performance Monitoring)
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "Input.hpp"
#include "Log.hpp"
#include "Config.hpp"

// Include game headers
#include "../game/BlockRegistry.hpp"
#include "../game/ChunkProvider.hpp"
#include "../game/WorldManager.hpp"

// Include rendering headers
#include "../render/Camera.hpp"
#include "../render/ChunkRenderer.hpp"
#include "../render/Frustum.hpp"
#include "../render/Shader.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#ifndef NDEBUG
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#endif

namespace PlatformMain {

    // Performance monitoring structures
    struct PerformanceMetrics {
        float frameTime = 0.0f;
        float meshUploadTime = 0.0f;
        float renderTime = 0.0f;
        int meshesUploadedThisFrame = 0;
        int meshesRenderedThisFrame = 0;
        size_t totalVerticesRendered = 0;
        size_t totalIndicesRendered = 0;

        // Rolling averages
        static constexpr int SAMPLE_COUNT = 60;
        float frameTimes[SAMPLE_COUNT] = {0};
        int sampleIndex = 0;

        void AddFrameTimeSample(float time) {
            frameTimes[sampleIndex] = time;
            sampleIndex = (sampleIndex + 1) % SAMPLE_COUNT;
        }

        float GetAverageFrameTime() const {
            float sum = 0.0f;
            for (int i = 0; i < SAMPLE_COUNT; ++i) {
                sum += frameTimes[i];
            }
            return sum / SAMPLE_COUNT;
        }

        float GetFPS() const {
            float avgTime = GetAverageFrameTime();
            return avgTime > 0.0f ? 1.0f / avgTime : 0.0f;
        }
    };

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
        Log::Info("Starting MyVoxelGame v0.1 with Enhanced Inter-Chunk Face Culling");

        // 2) Initialize BlockRegistry
        Game::BlockRegistry::Init();

        // 3) Initialize GLFW
        if (!glfwInit()) {
            Log::Error("Failed to initialize GLFW");
            return -1;
        }

        // 4) Request OpenGL context
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Config::OpenGLMajor);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Config::OpenGLMinor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    #endif
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);

        // 5) Create window
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

        // 6) Make context current and load GLAD
        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            Log::Error("Failed to initialize GLAD");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -3;
        }

        // 7) Initialize input
        Input::Init(window);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        // 8) Print GPU info
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

        // 9) Compile shaders
        Shader blockShader({
            "shaders/block.vert",
            "shaders/block.frag"
        });

        // 10) Enable OpenGL features
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        // 11) Initialize camera
        Render::Camera camera;
        camera.position = glm::vec3(0.0f, 80.0f, 0.0f);
        camera.yaw = 0.0f;
        camera.pitch = 0.0f;
        glfwSwapInterval(1); // Enable VSync

    #ifndef NDEBUG
        // 12) Setup ImGui
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");
    #endif

        // 13) Performance monitoring
        PerformanceMetrics metrics;
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        // Mouse cursor toggle state
        bool cursorEnabled = false;
        bool tabKeyPressed = false;

        // 14) Main loop
        Log::Info("Entering main render loop with enhanced inter-chunk culling");

        while (!glfwWindowShouldClose(window)) {
            frameStartTime = std::chrono::high_resolution_clock::now();

            // a) Poll events
            glfwPollEvents();

            // b) Update time
            Time::Tick();
            float dt = static_cast<float>(Time::Delta());
            metrics.AddFrameTimeSample(dt);

            // c) Handle input
            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            // d) Update camera
            camera.Update(dt);

            // e) Update world (handles chunk loading/unloading with enhanced meshing)
            Game::WorldManager::Update(camera.position);

    #ifndef NDEBUG
            // f) Start ImGui frame
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
    #endif

            // g) Clear buffers
            glClearColor(0.5f, 0.7f, 1.0f, 1.0f); // Sky blue
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // h) Upload meshes with performance tracking
            auto uploadStartTime = std::chrono::high_resolution_clock::now();
            {
                Game::MeshData* meshPtr = nullptr;
                metrics.meshesUploadedThisFrame = 0;

                // Limit uploads per frame to maintain consistent frame rate
                constexpr int MAX_UPLOADS_PER_FRAME = 8;

                while (metrics.meshesUploadedThisFrame < MAX_UPLOADS_PER_FRAME &&
                       Game::PopMeshData(meshPtr) && meshPtr != nullptr) {

                    // Validate mesh data
                    if (meshPtr->vertices.empty()) {
                        Log::Warning("Skipping upload of empty mesh for chunk (%d,%d) section %d",
                                    meshPtr->chunkXZ.x, meshPtr->chunkXZ.y, meshPtr->sectionIndex);
                        delete meshPtr;
                        continue;
                    }

                    // Check for corrupted coordinates
                    if (meshPtr->chunkXZ.x < -10000 || meshPtr->chunkXZ.x > 10000 ||
                        meshPtr->chunkXZ.y < -10000 || meshPtr->chunkXZ.y > 10000) {
                        Log::Error("Corrupted chunk coordinates: (%d,%d), skipping",
                                  meshPtr->chunkXZ.x, meshPtr->chunkXZ.y);
                        delete meshPtr;
                        continue;
                    }

                    blockShader.Use();
                    Render::UploadMesh(meshPtr);
                    metrics.meshesUploadedThisFrame++;

                    Log::Debug("Uploaded mesh: chunk (%d,%d) section %d, %zu vertices, %zu indices",
                             meshPtr->chunkXZ.x, meshPtr->chunkXZ.y, meshPtr->sectionIndex,
                             meshPtr->vertices.size(), meshPtr->indices.size());
                }
            }
            auto uploadEndTime = std::chrono::high_resolution_clock::now();
            metrics.meshUploadTime = std::chrono::duration<float, std::milli>(uploadEndTime - uploadStartTime).count();

            // i) Render scene with performance tracking
            auto renderStartTime = std::chrono::high_resolution_clock::now();
            {
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

                // Use shader
                blockShader.Use();

                // Frustum culling
                Frustum frust = Frustum::FromMatrix(proj * view);

                // Render visible chunks
                metrics.meshesRenderedThisFrame = 0;
                metrics.totalVerticesRendered = 0;
                metrics.totalIndicesRendered = 0;

                for (const auto& cm : Render::g_chunkMeshes) {
                    AABB box = cm.GetAABB();

                    if (!frust.IsBoxVisible(box)) {
                        continue; // Frustum culled
                    }

                    glm::mat4 model = glm::translate(glm::mat4(1.0f), cm.worldOffset);
                    glm::mat4 mvp = proj * view * model;

                    blockShader.SetMat4("uMVP", mvp);
                    cm.Draw();

                    metrics.meshesRenderedThisFrame++;
                    metrics.totalVerticesRendered += cm.indexCount; // Approximation
                    metrics.totalIndicesRendered += cm.indexCount;
                }
            }
            auto renderEndTime = std::chrono::high_resolution_clock::now();
            metrics.renderTime = std::chrono::duration<float, std::milli>(renderEndTime - renderStartTime).count();

    #ifndef NDEBUG
            // j) ImGui debug interface
            {
                ImGui::Begin("Enhanced Voxel Engine Debug");

                // Performance metrics
                ImGui::Text("Performance");
                ImGui::Separator();
                ImGui::Text("FPS: %.1f (%.2f ms)", metrics.GetFPS(), metrics.GetAverageFrameTime() * 1000.0f);
                ImGui::Text("Frame Time: %.2f ms", dt * 1000.0f);
                ImGui::Text("Mesh Upload: %.2f ms", metrics.meshUploadTime);
                ImGui::Text("Render Time: %.2f ms", metrics.renderTime);
                ImGui::Spacing();

                // World statistics
                ImGui::Text("World Statistics");
                ImGui::Separator();
                glm::vec3 camPos = camera.position;
                ImGui::Text("Camera Position: (%.1f, %.1f, %.1f)", camPos.x, camPos.y, camPos.z);
                ImGui::Text("Camera Rotation: Yaw=%.1f°, Pitch=%.1f°", camera.yaw, camera.pitch);
                ImGui::Text("Loaded Chunks: %zu", Game::WorldManager::GetLoadedChunkCount());
                ImGui::Text("Rendered Sections: %d / %zu", metrics.meshesRenderedThisFrame, Render::g_chunkMeshes.size());
                ImGui::Text("Meshes Uploaded This Frame: %d", metrics.meshesUploadedThisFrame);
                ImGui::Spacing();

                // Rendering statistics
                ImGui::Text("Rendering Statistics");
                ImGui::Separator();
                ImGui::Text("Total Vertices Rendered: %zu", metrics.totalVerticesRendered);
                ImGui::Text("Total Indices Rendered: %zu", metrics.totalIndicesRendered);
                ImGui::Text("Frustum Culled Sections: %zu",
                           Render::g_chunkMeshes.size() - metrics.meshesRenderedThisFrame);

                // Performance graph
                ImGui::PlotLines("Frame Time (ms)", metrics.frameTimes, PerformanceMetrics::SAMPLE_COUNT,
                               metrics.sampleIndex, nullptr, 0.0f, 50.0f, ImVec2(0, 80));

                // Controls
                ImGui::Spacing();
                ImGui::Text("Controls");
                ImGui::Separator();
                ImGui::Text("WASD: Move horizontally");
                ImGui::Text("Space/Shift: Move vertically");
                ImGui::Text("Mouse: Look around");
                ImGui::Text("Escape: Exit");

                // Force remesh button for testing
                if (ImGui::Button("Force Remesh Current Chunk")) {
                    int chunkX = static_cast<int>(std::floor(camPos.x / Game::Math::CHUNK_SIZE_X));
                    int chunkZ = static_cast<int>(std::floor(camPos.z / Game::Math::CHUNK_SIZE_Z));
                    Game::WorldManager::ForceRemeshChunk({chunkX, chunkZ});
                    Log::Info("Force remesh requested for chunk (%d, %d)", chunkX, chunkZ);
                }

                ImGui::End();
            }

            // k) Render ImGui
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    #endif

            // l) Swap buffers
            glfwSwapBuffers(window);

            // m) Reset input deltas
            Input::ResetMouseDelta();
            Input::ResetScrollOffset();

            // n) Calculate total frame time
            auto frameEndTime = std::chrono::high_resolution_clock::now();
            metrics.frameTime = std::chrono::duration<float, std::milli>(frameEndTime - frameStartTime).count();
        }

    #ifndef NDEBUG
        // 15) Cleanup ImGui
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    #endif

        // 16) Cleanup OpenGL resources
        for (auto& cm : Render::g_chunkMeshes) {
            glDeleteVertexArrays(1, &cm.vao);
            glDeleteBuffers(1, &cm.vbo);
            glDeleteBuffers(1, &cm.ebo);
        }

        Log::Info("Enhanced voxel engine shutting down");
        Log::Info("Final statistics: %zu chunks loaded, %zu sections rendered total",
                 Game::WorldManager::GetLoadedChunkCount(), Render::g_chunkMeshes.size());

        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

} // namespace PlatformMain