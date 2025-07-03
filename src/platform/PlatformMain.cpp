// File: src/platform/PlatformMain.cpp (Simplified Main Loop)
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "Input.hpp"
#include "Log.hpp"
#include "Config.hpp"
#include "../debug/DebugSystem.hpp"

// Include game headers
#include "../game/BlockRegistry.hpp"
#include "../game/ChunkProvider.hpp"
#include "../game/WorldManager.hpp"
#include "../game/PlayerController.hpp"
#include "../game/WorldAccess.hpp"
#include "../game/RayCast.hpp"
#include "../game/Physics.hpp"

// Include rendering headers
#include "../render/Camera.hpp"
#include "../render/ChunkRenderer.hpp"
#include "../render/Frustum.hpp"
#include "../render/Shader.hpp"
#include "../render/TextureAtlas.hpp"
#include "../render/BlockHighlight.hpp"
#include "../render/Crosshair.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <chrono>

namespace PlatformMain {

    // Input handling
    void HandlePlayerInput(Game::PlayerController& playerController, Render::Camera& camera) {
        // Movement input
        glm::vec3 movementInput = camera.CalculateMovementInput();
        playerController.SetMovementInput(movementInput);

        // Action inputs
        playerController.SetJumpPressed(camera.IsJumpPressed());
        playerController.SetSprintPressed(camera.IsSprintPressed());
        playerController.SetSneakPressed(camera.IsSneakPressed());

        // Block interaction
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

        // Inventory selection
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

        // Debug noclip toggle
        if (Input::IsKeyPressed(Input::Key::N)) {
            playerController.ToggleNoclip();
        }
    }

    bool HandleCursorToggle(GLFWwindow* window, Render::Camera& camera) {
        static bool cursorEnabled = false;

        if (Input::IsKeyPressed(Input::Key::Tab)) {
            cursorEnabled = !cursorEnabled;

            if (cursorEnabled) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                camera.enableMouseLook = false;
                Log::Info("Cursor enabled - camera mouse look disabled");
            } else {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                camera.enableMouseLook = true;
                Log::Info("Cursor disabled - camera mouse look enabled");
            }
        }

        return cursorEnabled;
    }

    void UploadMeshData(Debug::PerformanceMetrics& metrics) {
        auto uploadStartTime = std::chrono::high_resolution_clock::now();

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

        auto uploadEndTime = std::chrono::high_resolution_clock::now();
        metrics.meshUploadTime = std::chrono::duration<float, std::milli>(uploadEndTime - uploadStartTime).count();
    }

    void RenderScene(const Render::Camera& camera, const Shader& blockShader,
                    const glm::mat4& proj, const glm::mat4& view, const Frustum& frustum,
                    Debug::PerformanceMetrics& metrics) {

        auto renderStartTime = std::chrono::high_resolution_clock::now();

        // Use shader and bind texture atlas
        blockShader.Use();
        Render::g_textureAtlas.Bind(GL_TEXTURE0);
        glUniform1i(blockShader.GetUniformLocation("uTextureAtlas"), 0);

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
            metrics.totalVerticesRendered += cm.indexCount;
            metrics.totalIndicesRendered += cm.indexCount;
        }

        auto renderEndTime = std::chrono::high_resolution_clock::now();
        metrics.renderTime = std::chrono::duration<float, std::milli>(renderEndTime - renderStartTime).count();
    }

    void RenderBlockHighlight(const Game::PlayerController& playerController, const glm::mat4& viewProj) {
        const auto& hit = playerController.GetCurrentHit();
        if (Render::BlockHighlight::IsValidHighlight(hit)) {
            Render::g_blockHighlight.Render(hit->blockPos, viewProj);
        }
    }

    void RenderCrosshair(GLFWwindow* window) {
        int windowWidth, windowHeight, framebufferWidth, framebufferHeight;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

        Render::g_crosshair.Render(windowWidth, windowHeight, framebufferWidth, framebufferHeight);
    }

    // Callback for OpenGL debug messages
    void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam) {
        Log::Error("[GL DEBUG] %s", message);
    }

    int Run(int argc, char** argv) {
        // Initialize systems
        Log::Init();
        Log::Info("Starting MyVoxelGame v0.1 with Physics System and Crosshair");

        Game::BlockRegistry::Init();

        // Initialize GLFW
        if (!glfwInit()) {
            Log::Error("Failed to initialize GLFW");
            return -1;
        }

        // Setup OpenGL context
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Config::OpenGLMajor);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Config::OpenGLMinor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    #endif
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);

        // Create window
        GLFWwindow* window = glfwCreateWindow(
            Config::WindowWidth, Config::WindowHeight, Config::WindowTitle,
            nullptr, nullptr
        );
        if (!window) {
            Log::Error("Failed to create GLFW window");
            glfwTerminate();
            return -2;
        }

        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            Log::Error("Failed to initialize GLAD");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -3;
        }

        // Initialize input
        Input::Init(window);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        // Log system info
        Log::Info("Vendor: %s", glGetString(GL_VENDOR));
        Log::Info("Renderer: %s", glGetString(GL_RENDERER));
        Log::Info("Version: %s", glGetString(GL_VERSION));

        // Setup debug output
    #ifndef NDEBUG
        if (GLAD_GL_KHR_debug) {
            glEnable(GL_DEBUG_OUTPUT);
            glDebugMessageCallback(glDebugOutput, nullptr);
            Log::Info("KHR_debug enabled");
        }
    #endif

        // Initialize rendering systems
        if (!Render::g_textureAtlas.Initialize()) {
            Log::Error("Failed to initialize texture atlas");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -4;
        }

        if (!Render::g_blockHighlight.Initialize()) {
            Log::Error("Failed to initialize block highlight system");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -5;
        }

        if (!Render::g_crosshair.Initialize()) {
            Log::Warning("Failed to initialize crosshair system, continuing without crosshair");
        }

        // Compile shaders
        Shader blockShader({"shaders/block.vert", "shaders/block.frag"});

        // Setup OpenGL state
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glfwSwapInterval(1); // VSync

        // Initialize game systems
        Render::Camera camera;
        camera.position = glm::vec3(0.0f, 80.0f, 0.0f);
        camera.physicsControlled = true;

        Game::PlayerController playerController;

        // Initialize debug system
        Debug::DebugSystem::Initialize(window);

        // Performance tracking
        Debug::PerformanceMetrics metrics;
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        Log::Info("Entering main render loop");

        // MAIN LOOP - Much cleaner now!
        while (!glfwWindowShouldClose(window)) {
            frameStartTime = std::chrono::high_resolution_clock::now();

            // Poll events and update input
            glfwPollEvents();
            Input::UpdateKeyStates();

            // Handle input
            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            bool cursorEnabled = HandleCursorToggle(window, camera);
            HandlePlayerInput(playerController, camera);

            // Update time and metrics
            Time::Tick();
            float dt = static_cast<float>(Time::Delta());
            metrics.AddFrameTimeSample(dt);

            // Update game systems
            camera.Update(dt);
            playerController.Update(dt, camera);
            Game::WorldManager::Update(camera.position);

            // Start debug frame
            Debug::DebugSystem::BeginFrame();

            // Clear and setup rendering
            glClearColor(0.5f, 0.7f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Calculate matrices
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            float aspect = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);

            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.1f, 1000.0f);
            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 viewProj = proj * view;
            Frustum frustum = Frustum::FromMatrix(viewProj);

            // Upload mesh data
            UploadMeshData(metrics);

            // Render scene
            RenderScene(camera, blockShader, proj, view, frustum, metrics);

            // Render UI elements
            RenderBlockHighlight(playerController, viewProj);

            // Render debug UI
            int windowWidth, windowHeight;
            glfwGetWindowSize(window, &windowWidth, &windowHeight);
            Debug::DebugSystem::RenderDebugUI(
                camera, frustum, playerController, metrics, cursorEnabled,
                windowWidth, windowHeight, width, height
            );

            // Finish debug frame
            Debug::DebugSystem::EndFrame();

            // Render crosshair (must be last to appear on top)
            RenderCrosshair(window);

            // Swap buffers and reset input
            glfwSwapBuffers(window);
            Input::ResetMouseDelta();
            Input::ResetScrollOffset();

            // Calculate frame time
            auto frameEndTime = std::chrono::high_resolution_clock::now();
            metrics.frameTime = std::chrono::duration<float, std::milli>(frameEndTime - frameStartTime).count();
        }

        // Cleanup
        Debug::DebugSystem::Shutdown();

        for (auto& cm : Render::g_chunkMeshes) {
            glDeleteVertexArrays(1, &cm.vao);
            glDeleteBuffers(1, &cm.vbo);
            glDeleteBuffers(1, &cm.ebo);
        }

        Log::Info("Voxel engine shutting down");
        Log::Info("Final statistics: %zu chunks loaded, %zu sections rendered",
                 Game::WorldManager::GetLoadedChunkCount(), Render::g_chunkMeshes.size());

        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

} // namespace PlatformMain