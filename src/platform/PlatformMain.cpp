// File: src/platform/PlatformMain.cpp (Enhanced with Performance Monitoring, Cursor Toggle, and Chunk Visualization)
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
#include <unordered_set>
#include <cmath>

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

    // Chunk visualization helper functions
    struct ChunkState {
        bool isGenerated = false;
        bool inFrustum = false;
    };

    // Helper function to check if a chunk is in the camera frustum
    bool IsChunkInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos) {
        // Calculate chunk bounds in world space
        float worldX = static_cast<float>(chunkPos.x * Game::Math::CHUNK_SIZE_X);
        float worldZ = static_cast<float>(chunkPos.z * Game::Math::CHUNK_SIZE_Z);

        // Create AABB for the entire chunk (from Y=0 to Y=256)
        AABB chunkAABB;
        chunkAABB.min = glm::vec3(worldX, 0.0f, worldZ);
        chunkAABB.max = glm::vec3(
            worldX + Game::Math::CHUNK_SIZE_X,
            Game::Math::CHUNK_TOTAL_HEIGHT,
            worldZ + Game::Math::CHUNK_SIZE_Z
        );

        return frustum.IsBoxVisible(chunkAABB);
    }

    // Draw chunk visualization in ImGui
    void DrawChunkVisualization(const Render::Camera& camera, const Frustum& frustum) {
        ImGui::Begin("Chunk Visualization");

        // Calculate camera chunk position
        int cameraChunkX = static_cast<int>(std::floor(camera.position.x / Game::Math::CHUNK_SIZE_X));
        int cameraChunkZ = static_cast<int>(std::floor(camera.position.z / Game::Math::CHUNK_SIZE_Z));

        // Get loaded chunks
        auto loadedChunks = Game::WorldManager::GetLoadedChunks();
        std::unordered_set<uint64_t> loadedChunkSet;

        // Create a set for quick lookup of loaded chunks
        for (const auto& chunk : loadedChunks) {
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(chunk.x)) << 32) |
                          static_cast<uint32_t>(chunk.z);
            loadedChunkSet.insert(key);
        }

        // Visualization parameters
        const int vizRadius = 12; // Show chunks in a 25x25 grid around player
        const float circleRadius = 8.0f;
        const float spacing = 20.0f;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();

        // Ensure minimum canvas size
        if (canvasSize.x < 500) canvasSize.x = 500;
        if (canvasSize.y < 500) canvasSize.y = 500;

        // Calculate center of visualization
        ImVec2 center = ImVec2(canvasPos.x + canvasSize.x * 0.5f, canvasPos.y + canvasSize.y * 0.5f);

        // Draw grid background
        ImU32 gridColor = IM_COL32(64, 64, 64, 128);
        for (int i = -vizRadius; i <= vizRadius; i++) {
            float x = center.x + i * spacing;
            drawList->AddLine(
                ImVec2(x, canvasPos.y),
                ImVec2(x, canvasPos.y + canvasSize.y),
                gridColor, 1.0f
            );

            float y = center.y + i * spacing;
            drawList->AddLine(
                ImVec2(canvasPos.x, y),
                ImVec2(canvasPos.x + canvasSize.x, y),
                gridColor, 1.0f
            );
        }

        // Draw chunks
        int totalChunks = 0;
        int generatedChunks = 0;
        int visibleChunks = 0;

        for (int dz = -vizRadius; dz <= vizRadius; dz++) {
            for (int dx = -vizRadius; dx <= vizRadius; dx++) {
                totalChunks++;

                Game::Math::ChunkPos chunkPos = {cameraChunkX + dx, cameraChunkZ + dz};

                // Check if chunk is loaded/generated
                uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(chunkPos.x)) << 32) |
                              static_cast<uint32_t>(chunkPos.z);
                bool isGenerated = loadedChunkSet.find(key) != loadedChunkSet.end();

                // Check if chunk is in frustum
                bool inFrustum = false;
                if (isGenerated) {
                    generatedChunks++;
                    inFrustum = IsChunkInFrustum(frustum, chunkPos);
                    if (inFrustum) {
                        visibleChunks++;
                    }
                }

                // Determine color based on state
                ImU32 chunkColor;
                if (!isGenerated) {
                    chunkColor = IM_COL32(255, 64, 64, 255);   // Red: not generated
                } else if (inFrustum) {
                    chunkColor = IM_COL32(64, 255, 64, 255);   // Green: generated and visible
                } else {
                    chunkColor = IM_COL32(255, 255, 64, 255);  // Yellow: generated but not visible
                }

                // Calculate position on screen
                ImVec2 chunkScreenPos = ImVec2(
                    center.x + dx * spacing,
                    center.y + dz * spacing
                );

                // Draw chunk circle
                drawList->AddCircleFilled(chunkScreenPos, circleRadius, chunkColor);

                // Draw chunk outline
                ImU32 outlineColor = IM_COL32(128, 128, 128, 255);
                drawList->AddCircle(chunkScreenPos, circleRadius, outlineColor, 0, 2.0f);

                // Draw chunk coordinates on hover
                if (ImGui::IsMouseHoveringRect(
                    ImVec2(chunkScreenPos.x - circleRadius, chunkScreenPos.y - circleRadius),
                    ImVec2(chunkScreenPos.x + circleRadius, chunkScreenPos.y + circleRadius))) {

                    ImGui::SetTooltip("Chunk (%d, %d)\nGenerated: %s\nIn Frustum: %s",
                                     chunkPos.x, chunkPos.z,
                                     isGenerated ? "Yes" : "No",
                                     inFrustum ? "Yes" : "No");
                }
            }
        }

        // Draw player position (camera chunk)
        ImVec2 playerPos = center;
        drawList->AddCircleFilled(playerPos, circleRadius + 2.0f, IM_COL32(255, 255, 255, 255));
        drawList->AddCircle(playerPos, circleRadius + 2.0f, IM_COL32(0, 0, 0, 255), 0, 3.0f);

        // Draw player direction indicator
        float playerYawRad = glm::radians(camera.yaw);
        ImVec2 directionEnd = ImVec2(
            playerPos.x + cos(playerYawRad) * (circleRadius + 8.0f),
            playerPos.y + sin(playerYawRad) * (circleRadius + 8.0f)
        );
        drawList->AddLine(playerPos, directionEnd, IM_COL32(0, 0, 0, 255), 3.0f);

        // Create invisible button for the entire canvas to capture input
        ImGui::InvisibleButton("chunk_viz_canvas", canvasSize);

        // Display statistics
        ImGui::Separator();
        ImGui::Text("Chunk Statistics:");
        ImGui::Text("Total chunks in view: %d", totalChunks);
        ImGui::Text("Generated chunks: %d", generatedChunks);
        ImGui::Text("Visible chunks: %d", visibleChunks);
        ImGui::Text("Camera chunk: (%d, %d)", cameraChunkX, cameraChunkZ);

        // Legend
        ImGui::Separator();
        ImGui::Text("Legend:");
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "● Red: Not generated");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.25f, 1.0f), "● Yellow: Generated, not visible");
        ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.25f, 1.0f), "● Green: Generated and visible");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "● White: Player position");

        ImGui::End();
    }

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

        // 14) Main loop
        Log::Info("Entering main render loop with enhanced inter-chunk culling");

        while (!glfwWindowShouldClose(window)) {
            frameStartTime = std::chrono::high_resolution_clock::now();

            // a) Poll events
            glfwPollEvents();

            // b) Update input states
            Input::UpdateKeyStates();

            // c) Update time
            Time::Tick();
            float dt = static_cast<float>(Time::Delta());
            metrics.AddFrameTimeSample(dt);

            // d) Handle input
            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            // e) Handle cursor toggle (Tab key)
            if (Input::IsKeyPressed(Input::Key::Tab)) {
                cursorEnabled = !cursorEnabled;

                if (cursorEnabled) {
                    // Enable cursor and disable camera mouse look
                    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                    camera.enableMouseLook = false;
                    Log::Info("Cursor enabled - camera mouse look disabled");
                } else {
                    // Disable cursor and enable camera mouse look
                    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                    camera.enableMouseLook = true;
                    Log::Info("Cursor disabled - camera mouse look enabled");
                }
            }

            // f) Update camera
            camera.Update(dt);

            // g) Update world (handles chunk loading/unloading with enhanced meshing)
            Game::WorldManager::Update(camera.position);

    #ifndef NDEBUG
            // h) Start ImGui frame
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
    #endif

            // i) Clear buffers
            glClearColor(0.5f, 0.7f, 1.0f, 1.0f); // Sky blue
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // j) Calculate frustum for chunk visibility testing
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
            Frustum frustum = Frustum::FromMatrix(proj * view);

            // k) Upload meshes with performance tracking
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

            // l) Render scene with performance tracking
            auto renderStartTime = std::chrono::high_resolution_clock::now();
            {
                // Use shader
                blockShader.Use();

                // Render visible chunks
                metrics.meshesRenderedThisFrame = 0;
                metrics.totalVerticesRendered = 0;
                metrics.totalIndicesRendered = 0;

                for (const auto& cm : Render::g_chunkMeshes) {
                    AABB box = cm.GetAABB();

                    if (!frustum.IsBoxVisible(box)) {
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
            // m) ImGui debug interface
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

                // Controls and status
                ImGui::Spacing();
                ImGui::Text("Controls & Status");
                ImGui::Separator();
                ImGui::Text("WASD: Move horizontally");
                ImGui::Text("Space/Shift: Move vertically");
                ImGui::Text("Mouse: Look around (when enabled)");
                ImGui::Text("Tab: Toggle cursor visibility");
                ImGui::Text("Escape: Exit");
                ImGui::Spacing();

                // Cursor status indicator
                if (cursorEnabled) {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Cursor: VISIBLE (Camera locked)");
                } else {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Cursor: HIDDEN (Camera active)");
                }
                ImGui::Text("Mouse Look: %s", camera.enableMouseLook ? "ENABLED" : "DISABLED");

                // Force remesh button for testing
                if (ImGui::Button("Force Remesh Current Chunk")) {
                    int chunkX = static_cast<int>(std::floor(camPos.x / Game::Math::CHUNK_SIZE_X));
                    int chunkZ = static_cast<int>(std::floor(camPos.z / Game::Math::CHUNK_SIZE_Z));
                    Game::WorldManager::ForceRemeshChunk({chunkX, chunkZ});
                    Log::Info("Force remesh requested for chunk (%d, %d)", chunkX, chunkZ);
                }

                ImGui::End();
            }

            // n) Draw chunk visualization
            DrawChunkVisualization(camera, frustum);

            // o) Render ImGui
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    #endif

            // p) Swap buffers
            glfwSwapBuffers(window);

            // q) Reset input deltas
            Input::ResetMouseDelta();
            Input::ResetScrollOffset();

            // r) Calculate total frame time
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