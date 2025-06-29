// File: src/platform/PlatformMain.cpp (Complete with Physics Integration)
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "Input.hpp"
#include "Log.hpp"
#include "Config.hpp"

// Include game headers
#include "../game/BlockRegistry.hpp"
#include "../game/ChunkProvider.hpp"
#include "../game/WorldManager.hpp"
#include "../game/PlayerController.hpp"
#include "../game/WorldAccess.hpp"
#include "../game/RayCast.hpp"
#include "../game/Physics.hpp"  // Add physics header

// Include rendering headers
#include "../render/Camera.hpp"
#include "../render/ChunkRenderer.hpp"
#include "../render/Frustum.hpp"
#include "../render/Shader.hpp"
#include "../render/TextureAtlas.hpp"
#include "../render/BlockHighlight.hpp"

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

#ifndef NDEBUG
    // Chunk visualization helper functions
    struct ChunkState {
        bool isGenerated = false;
        bool inFrustum = false;
    };

    bool IsChunkInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos) {
        float worldX = static_cast<float>(chunkPos.x * Game::Math::CHUNK_SIZE_X);
        float worldZ = static_cast<float>(chunkPos.z * Game::Math::CHUNK_SIZE_Z);

        AABB chunkAABB;
        chunkAABB.min = glm::vec3(worldX, 0.0f, worldZ);
        chunkAABB.max = glm::vec3(
            worldX + Game::Math::CHUNK_SIZE_X,
            Game::Math::CHUNK_TOTAL_HEIGHT,
            worldZ + Game::Math::CHUNK_SIZE_Z
        );

        return frustum.IsBoxVisible(chunkAABB);
    }

    void DrawChunkVisualization(const Render::Camera& camera, const Frustum& frustum) {
        ImGui::Begin("Chunk Visualization");

        int cameraChunkX = static_cast<int>(std::floor(camera.position.x / Game::Math::CHUNK_SIZE_X));
        int cameraChunkZ = static_cast<int>(std::floor(camera.position.z / Game::Math::CHUNK_SIZE_Z));

        auto loadedChunks = Game::WorldManager::GetLoadedChunks();
        std::unordered_set<uint64_t> loadedChunkSet;

        for (const auto& chunk : loadedChunks) {
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(chunk.x)) << 32) |
                          static_cast<uint32_t>(chunk.z);
            loadedChunkSet.insert(key);
        }

        const int vizRadius = 12;
        const float circleRadius = 8.0f;
        const float spacing = 20.0f;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();

        if (canvasSize.x < 500) canvasSize.x = 500;
        if (canvasSize.y < 500) canvasSize.y = 500;

        ImVec2 center = ImVec2(canvasPos.x + canvasSize.x * 0.5f, canvasPos.y + canvasSize.y * 0.5f);

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

        int totalChunks = 0;
        int generatedChunks = 0;
        int visibleChunks = 0;

        for (int dz = -vizRadius; dz <= vizRadius; dz++) {
            for (int dx = -vizRadius; dx <= vizRadius; dx++) {
                totalChunks++;

                Game::Math::ChunkPos chunkPos = {cameraChunkX + dx, cameraChunkZ + dz};

                uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(chunkPos.x)) << 32) |
                              static_cast<uint32_t>(chunkPos.z);
                bool isGenerated = loadedChunkSet.find(key) != loadedChunkSet.end();

                bool inFrustum = false;
                if (isGenerated) {
                    generatedChunks++;
                    inFrustum = IsChunkInFrustum(frustum, chunkPos);
                    if (inFrustum) {
                        visibleChunks++;
                    }
                }

                ImU32 chunkColor;
                if (!isGenerated) {
                    chunkColor = IM_COL32(255, 64, 64, 255);
                } else if (inFrustum) {
                    chunkColor = IM_COL32(64, 255, 64, 255);
                } else {
                    chunkColor = IM_COL32(255, 255, 64, 255);
                }

                ImVec2 chunkScreenPos = ImVec2(
                    center.x + dx * spacing,
                    center.y + dz * spacing
                );

                drawList->AddCircleFilled(chunkScreenPos, circleRadius, chunkColor);

                ImU32 outlineColor = IM_COL32(128, 128, 128, 255);
                drawList->AddCircle(chunkScreenPos, circleRadius, outlineColor, 0, 2.0f);

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

        ImVec2 playerPos = center;
        drawList->AddCircleFilled(playerPos, circleRadius + 2.0f, IM_COL32(255, 255, 255, 255));
        drawList->AddCircle(playerPos, circleRadius + 2.0f, IM_COL32(0, 0, 0, 255), 0, 3.0f);

        float playerYawRad = glm::radians(camera.yaw);
        ImVec2 directionEnd = ImVec2(
            playerPos.x + cos(playerYawRad) * (circleRadius + 8.0f),
            playerPos.y + sin(playerYawRad) * (circleRadius + 8.0f)
        );
        drawList->AddLine(playerPos, directionEnd, IM_COL32(0, 0, 0, 255), 3.0f);

        ImGui::InvisibleButton("chunk_viz_canvas", canvasSize);

        ImGui::Separator();
        ImGui::Text("Chunk Statistics:");
        ImGui::Text("Total chunks in view: %d", totalChunks);
        ImGui::Text("Generated chunks: %d", generatedChunks);
        ImGui::Text("Visible chunks: %d", visibleChunks);
        ImGui::Text("Camera chunk: (%d, %d)", cameraChunkX, cameraChunkZ);

        ImGui::Separator();
        ImGui::Text("Legend:");
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "● Red: Not generated");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.25f, 1.0f), "● Yellow: Generated, not visible");
        ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.25f, 1.0f), "● Green: Generated and visible");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "● White: Player position");

        ImGui::End();
    }

    void DrawTextureAtlasDebug() {
        ImGui::Begin("Texture Atlas Debug");

        if (!Render::g_textureAtlas.IsLoaded()) {
            ImGui::Text("Texture atlas not loaded");
            ImGui::End();
            return;
        }

        ImGui::Text("Atlas Status: Loaded");
        ImGui::Text("Atlas Size: %dx%d pixels", Render::TextureAtlas::ATLAS_WIDTH, Render::TextureAtlas::ATLAS_HEIGHT);
        ImGui::Text("Tile Size: %dx%d pixels", Render::TextureAtlas::TILE_SIZE, Render::TextureAtlas::TILE_SIZE);
        ImGui::Text("Grid Layout: %dx%d tiles", Render::TextureAtlas::TILES_PER_ROW, Render::TextureAtlas::TILES_PER_COLUMN);
        ImGui::Text("Total Capacity: %d tiles", Render::TextureAtlas::MAX_TILES);
        ImGui::Text("Loaded textures: %zu", Render::g_textureAtlas.GetLoadedTextureCount());

        ImGui::Separator();

        ImGui::Text("Atlas Preview (Scroll to navigate):");
        GLuint atlasID = Render::g_textureAtlas.GetTextureID();

        static float zoomLevel = 1.0f;
        ImGui::SliderFloat("Zoom", &zoomLevel, 0.5f, 4.0f, "%.1fx");
        ImGui::SameLine();
        if (ImGui::Button("Reset Zoom")) {
            zoomLevel = 1.0f;
        }

        float baseDisplayWidth = 256.0f;
        float baseDisplayHeight = 1024.0f;
        float displayWidth = baseDisplayWidth * zoomLevel;
        float displayHeight = baseDisplayHeight * zoomLevel;

        ImVec2 scrollRegionSize = ImVec2(400, 500);
        ImGui::BeginChild("AtlasScrollRegion", scrollRegionSize, true, ImGuiWindowFlags_HorizontalScrollbar);

        ImTextureID textureID = (ImTextureID)(uintptr_t)atlasID;
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();

        ImGui::Image(textureID,
                    ImVec2(displayWidth, displayHeight),
                    ImVec2(0, 0), ImVec2(1, 1));

        if (ImGui::IsItemHovered()) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImVec2 imagePos = ImGui::GetItemRectMin();

            float tileDisplayWidth = displayWidth / Render::TextureAtlas::TILES_PER_ROW;
            float tileDisplayHeight = displayHeight / Render::TextureAtlas::TILES_PER_COLUMN;

            ImU32 gridColor = IM_COL32(255, 255, 255, 100);
            ImU32 majorGridColor = IM_COL32(255, 255, 0, 150);

            if (zoomLevel >= 0.8f) {
                for (int i = 0; i <= Render::TextureAtlas::TILES_PER_ROW; ++i) {
                    float offset = i * tileDisplayWidth;
                    ImU32 color = (i % 4 == 0) ? majorGridColor : gridColor;
                    drawList->AddLine(
                        ImVec2(imagePos.x + offset, imagePos.y),
                        ImVec2(imagePos.x + offset, imagePos.y + displayHeight),
                        color, (i % 4 == 0) ? 2.0f : 1.0f
                    );
                }

                int lineStep = (zoomLevel >= 2.0f) ? 1 : (zoomLevel >= 1.5f) ? 2 : 4;
                for (int i = 0; i <= Render::TextureAtlas::TILES_PER_COLUMN; i += lineStep) {
                    float offset = i * tileDisplayHeight;
                    ImU32 color = (i % 16 == 0) ? majorGridColor : gridColor;
                    float thickness = (i % 16 == 0) ? 2.0f : 1.0f;
                    drawList->AddLine(
                        ImVec2(imagePos.x, imagePos.y + offset),
                        ImVec2(imagePos.x + displayWidth, imagePos.y + offset),
                        color, thickness
                    );
                }
            }

            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 relativePos = ImVec2(mousePos.x - imagePos.x, mousePos.y - imagePos.y);

            if (relativePos.x >= 0 && relativePos.x < displayWidth &&
                relativePos.y >= 0 && relativePos.y < displayHeight) {

                int tileX = (int)(relativePos.x / tileDisplayWidth);
                int tileY = (int)(relativePos.y / tileDisplayHeight);

                tileX = std::max(0, std::min(tileX, Render::TextureAtlas::TILES_PER_ROW - 1));
                tileY = std::max(0, std::min(tileY, Render::TextureAtlas::TILES_PER_COLUMN - 1));

                int tileIndex = tileY * Render::TextureAtlas::TILES_PER_ROW + tileX;

                ImVec2 tileTopLeft = ImVec2(imagePos.x + tileX * tileDisplayWidth,
                                          imagePos.y + tileY * tileDisplayHeight);
                ImVec2 tileBottomRight = ImVec2(tileTopLeft.x + tileDisplayWidth,
                                              tileTopLeft.y + tileDisplayHeight);

                drawList->AddRect(tileTopLeft, tileBottomRight, IM_COL32(255, 0, 0, 200), 0.0f, 0, 3.0f);

                float uvPerTileX = 1.0f / static_cast<float>(Render::TextureAtlas::TILES_PER_ROW);
                float uvPerTileY = 1.0f / static_cast<float>(Render::TextureAtlas::TILES_PER_COLUMN);

                float uvMinX = static_cast<float>(tileX) * uvPerTileX;
                float uvMinY = static_cast<float>(tileY) * uvPerTileY;
                float uvMaxX = static_cast<float>(tileX + 1) * uvPerTileX;
                float uvMaxY = static_cast<float>(tileY + 1) * uvPerTileY;

                ImGui::SetTooltip("Tile (%d, %d) = Index %d\nUV: (%.4f, %.4f) to (%.4f, %.4f)",
                                 tileX, tileY, tileIndex,
                                 uvMinX, uvMinY, uvMaxX, uvMaxY);
            }
        }

        ImGui::EndChild();

        ImVec2 scrollPos = ImVec2(ImGui::GetScrollX(), ImGui::GetScrollY());
        ImGui::Text("Scroll: (%.0f, %.0f) | Zoom: %.1fx", scrollPos.x, scrollPos.y, zoomLevel);

        ImGui::End();
    }
#endif // NDEBUG

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
        Log::Info("Starting MyVoxelGame v0.1 with Physics System");

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

        // 9) Initialize texture atlas
        if (!Render::g_textureAtlas.Initialize()) {
            Log::Error("Failed to initialize texture atlas");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -4;
        }

        // 10) Initialize block highlighting system
        if (!Render::g_blockHighlight.Initialize()) {
            Log::Error("Failed to initialize block highlight system");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -5;
        }

        // 11) Compile shaders
        Shader blockShader({
            "shaders/block.vert",
            "shaders/block.frag"
        });

        // 12) Enable OpenGL features
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);

        // 13) Initialize camera with physics
        Render::Camera camera;
        camera.position = glm::vec3(0.0f, 80.0f, 0.0f);
        camera.yaw = 0.0f;
        camera.pitch = 0.0f;
        camera.physicsControlled = true; // Enable physics control
        glfwSwapInterval(1); // Enable VSync

        // 14) Initialize player controller with physics
        Game::PlayerController playerController;
        Log::Info("Player controller initialized with physics system");

    #ifndef NDEBUG
        // 15) Setup ImGui
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");
    #endif

        // 16) Performance monitoring
        PerformanceMetrics metrics;
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        // Mouse cursor toggle state
        bool cursorEnabled = false;

        // 17) Main loop
        Log::Info("Entering main render loop with physics system");

        while (!glfwWindowShouldClose(window)) {
            frameStartTime = std::chrono::high_resolution_clock::now();

            // a) Poll events
            glfwPollEvents();

            // b) Update input states
            Input::UpdateKeyStates();

            // c) Handle player input with physics

            // Calculate movement input from camera
            glm::vec3 movementInput = camera.CalculateMovementInput();
            playerController.SetMovementInput(movementInput);

            // Handle jump input
            playerController.SetJumpPressed(camera.IsJumpPressed());

            // Handle sprint input
            playerController.SetSprintPressed(camera.IsSprintPressed());

            // Handle sneak input
            playerController.SetSneakPressed(camera.IsSneakPressed());

            // Mouse buttons for block interaction
            if (Input::IsMouseButtonDown(Input::Key::LeftMouse)) {
                playerController.OnBreakPressed();
            } else {
                playerController.OnBreakReleased();
            }

            if (Input::IsMouseButtonDown(Input::Key::RightMouse)) {
                playerController.OnPlacePressed();
            } else {
                playerController.OnPlaceReleased();
            }

            // Number keys for inventory slot selection
            if (Input::IsKeyPressed(Input::Key::Alpha1)) playerController.SelectSlot(0);
            if (Input::IsKeyPressed(Input::Key::Alpha2)) playerController.SelectSlot(1);
            if (Input::IsKeyPressed(Input::Key::Alpha3)) playerController.SelectSlot(2);
            if (Input::IsKeyPressed(Input::Key::Alpha4)) playerController.SelectSlot(3);
            if (Input::IsKeyPressed(Input::Key::Alpha5)) playerController.SelectSlot(4);
            if (Input::IsKeyPressed(Input::Key::Alpha6)) playerController.SelectSlot(5);
            if (Input::IsKeyPressed(Input::Key::Alpha7)) playerController.SelectSlot(6);
            if (Input::IsKeyPressed(Input::Key::Alpha8)) playerController.SelectSlot(7);
            if (Input::IsKeyPressed(Input::Key::Alpha9)) playerController.SelectSlot(8);

            // Mouse wheel for inventory scrolling
            auto [scrollX, scrollY] = Input::GetScrollOffset();
            if (scrollY > 0) {
                playerController.SelectPreviousSlot();
            } else if (scrollY < 0) {
                playerController.SelectNextSlot();
            }

            // Debug key for noclip (N key)
            if (Input::IsKeyPressed(Input::Key::N)) {
                playerController.ToggleNoclip();
            }

            // d) Update time
            Time::Tick();
            float dt = static_cast<float>(Time::Delta());
            metrics.AddFrameTimeSample(dt);

            // e) Handle input
            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            // f) Handle cursor toggle (Tab key)
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

            // g) Update camera (orientation only, position set by physics)
            camera.Update(dt);

            // h) Update player controller with physics
            playerController.Update(dt, camera);

            // i) Update world (handles chunk loading/unloading with enhanced meshing)
            Game::WorldManager::Update(camera.position);

    #ifndef NDEBUG
            // j) Start ImGui frame
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
    #endif

            // k) Clear buffers
            glClearColor(0.5f, 0.7f, 1.0f, 1.0f); // Sky blue
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // l) Calculate frustum for chunk visibility testing
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
            glm::mat4 viewProj = proj * view;
            Frustum frustum = Frustum::FromMatrix(viewProj);

            // m) Upload meshes with performance tracking
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

                    Render::UploadMesh(meshPtr);
                    metrics.meshesUploadedThisFrame++;
                }
            }
            auto uploadEndTime = std::chrono::high_resolution_clock::now();
            metrics.meshUploadTime = std::chrono::duration<float, std::milli>(uploadEndTime - uploadStartTime).count();

            // n) Render scene with performance tracking and texture atlas
            auto renderStartTime = std::chrono::high_resolution_clock::now();
            {
                // Use shader and bind texture atlas
                blockShader.Use();

                // Bind the texture atlas to texture unit 0
                Render::g_textureAtlas.Bind(GL_TEXTURE0);

                // Set the texture atlas uniform
                blockShader.SetMat4("uMVP", glm::mat4(1.0f)); // Will be set per mesh
                glUniform1i(blockShader.GetUniformLocation("uTextureAtlas"), 0); // Texture unit 0

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

            // o) Render block highlight
            {
                const auto& hit = playerController.GetCurrentHit();
                if (Render::BlockHighlight::IsValidHighlight(hit)) {
                    Render::g_blockHighlight.Render(hit->blockPos, viewProj);
                }
            }

    #ifndef NDEBUG
            // p) ImGui debug interface
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

                // Texture Atlas information
                ImGui::Text("Texture Atlas");
                ImGui::Separator();
                ImGui::Text("Status: %s", Render::g_textureAtlas.IsLoaded() ? "Loaded" : "Not Loaded");
                ImGui::Text("Loaded Textures: %zu", Render::g_textureAtlas.GetLoadedTextureCount());
                ImGui::Text("Atlas Size: %dx%d", Render::TextureAtlas::ATLAS_WIDTH, Render::TextureAtlas::ATLAS_HEIGHT);
                ImGui::Text("Grid: %dx%d (%d tiles)", Render::TextureAtlas::TILES_PER_ROW, Render::TextureAtlas::TILES_PER_COLUMN, Render::TextureAtlas::MAX_TILES);
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

                // Physics information (NEW)
                ImGui::Spacing();
                ImGui::Text("Player Physics");
                ImGui::Separator();
                const auto& playerPhysics = playerController.GetPhysics();
                ImGui::Text("Position: (%.2f, %.2f, %.2f)",
                           playerPhysics.position.x, playerPhysics.position.y, playerPhysics.position.z);
                ImGui::Text("Velocity: (%.2f, %.2f, %.2f)",
                           playerPhysics.velocity.x, playerPhysics.velocity.y, playerPhysics.velocity.z);
                ImGui::Text("Speed: %.2f blocks/s", glm::length(playerPhysics.velocity));
                ImGui::Text("On Ground: %s", playerPhysics.isOnGround ? "YES" : "NO");
                ImGui::Text("In Water: %s", playerPhysics.isInWater ? "YES" : "NO");
                ImGui::Text("Sneaking: %s", playerPhysics.isSneaking ? "YES" : "NO");
                ImGui::Text("Sprinting: %s", playerPhysics.isSprinting ? "YES" : "NO");
                ImGui::Text("Noclip: %s", playerPhysics.noclip ? "ENABLED" : "DISABLED");

                // Eye height
                float currentEyeHeight = playerPhysics.isSneaking ?
                    Game::PlayerPhysics::EYE_HEIGHT_SNEAKING :
                    Game::PlayerPhysics::EYE_HEIGHT_STANDING;
                ImGui::Text("Eye Height: %.2f blocks", currentEyeHeight);

                // Player interaction info
                ImGui::Spacing();
                ImGui::Text("Player Interaction");
                ImGui::Separator();
                const auto& inventory = playerController.GetInventory();
                Game::BlockID selectedBlock = inventory.GetSelectedBlock();
                ImGui::Text("Selected Block: %s (Slot %d)",
                           selectedBlock == Game::BlockID::Air ? "None" :
                           Game::BlockRegistry::Get(selectedBlock).name.c_str(),
                           inventory.GetSelectedSlot());

                ImGui::Text("Inventory:");
                for (int i = 0; i < Game::Inventory::HOTBAR_SIZE; ++i) {
                    const auto& slot = inventory.GetSlot(i);
                    if (!slot.IsEmpty()) {
                        ImGui::Text("  [%d] %s x%d", i,
                                   Game::BlockRegistry::Get(slot.blockId).name.c_str(),
                                   slot.count);
                    }
                }

                const auto& hit = playerController.GetCurrentHit();
                if (hit.has_value()) {
                    ImGui::Text("Looking at: %s at (%d, %d, %d)",
                               Game::BlockRegistry::Get(hit->blockId).name.c_str(),
                               hit->blockPos.x, hit->blockPos.y, hit->blockPos.z);
                    ImGui::Text("Distance: %.2f", hit->distance);

                    // Block highlight status
                    bool isHighlighted = Render::BlockHighlight::IsValidHighlight(hit);
                    ImGui::Text("Block Highlighted: %s", isHighlighted ? "YES" : "NO");
                    if (!isHighlighted && hit->distance > Game::PlayerController::INTERACTION_RANGE) {
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "  (Out of reach: %.2f > %.2f)",
                                         hit->distance, Game::PlayerController::INTERACTION_RANGE);
                    }

                    if (playerController.IsBreaking()) {
                        ImGui::Text("Breaking Progress: %.0f%%",
                                   playerController.GetBreakProgress() * 100.0f);
                        ImGui::ProgressBar(playerController.GetBreakProgress(), ImVec2(-1, 0));
                    }
                } else {
                    ImGui::Text("Looking at: Nothing");
                    ImGui::Text("Block Highlighted: NO");
                }

                // Player statistics (enhanced)
                ImGui::Spacing();
                ImGui::Text("Player Statistics");
                ImGui::Separator();
                const auto& playerStats = playerController.GetStats();
                ImGui::Text("Blocks Placed: %d", playerStats.blocksPlaced);
                ImGui::Text("Blocks Broken: %d", playerStats.blocksBroken);
                ImGui::Text("Distance Traveled: %.1f blocks", playerStats.totalDistanceTraveled);
                ImGui::Text("Play Time: %.1f seconds", playerStats.totalPlayTime);
                if (playerStats.totalPlayTime > 0.0f) {
                    float avgSpeed = playerStats.totalDistanceTraveled / playerStats.totalPlayTime;
                    ImGui::Text("Average Speed: %.2f blocks/s", avgSpeed);
                }

                // Performance graph
                ImGui::PlotLines("Frame Time (ms)", metrics.frameTimes, PerformanceMetrics::SAMPLE_COUNT,
                               metrics.sampleIndex, nullptr, 0.0f, 50.0f, ImVec2(0, 80));

                // Enhanced controls list
                ImGui::Spacing();
                ImGui::Text("Controls & Status");
                ImGui::Separator();
                ImGui::Text("WASD: Move horizontally");
                ImGui::Text("Space: Jump");
                ImGui::Text("Shift: Sneak");
                ImGui::Text("Ctrl: Sprint");
                ImGui::Text("Mouse: Look around (when enabled)");
                ImGui::Text("Left Click: Break blocks");
                ImGui::Text("Right Click: Place blocks");
                ImGui::Text("1-9: Select inventory slot");
                ImGui::Text("Mouse Wheel: Scroll inventory");
                ImGui::Text("Tab: Toggle cursor visibility");
                ImGui::Text("N: Toggle noclip (debug)");
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

            // q) Draw chunk visualization
            DrawChunkVisualization(camera, frustum);

            // r) Draw texture atlas debug window
            DrawTextureAtlasDebug();

            // s) Render ImGui
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    #endif

            // t) Swap buffers
            glfwSwapBuffers(window);

            // u) Reset input deltas
            Input::ResetMouseDelta();
            Input::ResetScrollOffset();

            // v) Calculate total frame time
            auto frameEndTime = std::chrono::high_resolution_clock::now();
            metrics.frameTime = std::chrono::duration<float, std::milli>(frameEndTime - frameStartTime).count();
        }

    #ifndef NDEBUG
        // 18) Cleanup ImGui
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    #endif

        // 19) Cleanup OpenGL resources
        for (auto& cm : Render::g_chunkMeshes) {
            glDeleteVertexArrays(1, &cm.vao);
            glDeleteBuffers(1, &cm.vbo);
            glDeleteBuffers(1, &cm.ebo);
        }

        Log::Info("Enhanced voxel engine with physics system shutting down");
        Log::Info("Final statistics: %zu chunks loaded, %zu sections rendered total",
                 Game::WorldManager::GetLoadedChunkCount(), Render::g_chunkMeshes.size());

        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

} // namespace PlatformMain