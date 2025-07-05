// File: src/platform/PlatformMain.cpp (Fixed for macOS Bundle Asset Loading)
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "Input.hpp"
#include "Log.hpp"
#include "Config.hpp"
#include "../debug/DebugSystem.hpp"

// Include game headers
#include "../game/BlockRegistry.hpp"
#include "../game/EnhancedBlockRegistry.hpp"  // NEW
#include "../game/BlockModel.hpp"             // NEW
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
#include "../render/AtlasBuilder.hpp"         // NEW

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>  // NEW

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#endif

namespace PlatformMain {

    // NEW: Platform-specific function to get the correct asset path
    std::string GetAssetPath(const std::string& relativePath) {
#ifdef __APPLE__
        // On macOS, check if we're running from a bundle
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        if (mainBundle) {
            // Get the Resources directory from the bundle
            CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
            if (resourcesURL) {
                char path[PATH_MAX];
                if (CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8*)path, PATH_MAX)) {
                    CFRelease(resourcesURL);

                    std::string resourcesPath = std::string(path);
                    std::string fullPath = resourcesPath + "/" + relativePath;

                    // Check if the asset exists in the bundle Resources directory
                    if (std::filesystem::exists(fullPath)) {
                        Log::Info("Using bundle asset path: %s", fullPath.c_str());
                        return fullPath;
                    } else {
                        Log::Debug("Asset not found in bundle Resources: %s", fullPath.c_str());
                    }
                }
                CFRelease(resourcesURL);
            }
        }

        // Fall back to relative path from current directory
        Log::Debug("Falling back to relative asset path: %s", relativePath.c_str());
        return relativePath;
#else
        // On other platforms, use relative path directly
        return relativePath;
#endif
    }

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

        auto uploadEndTime = std::chrono::high_resolution_clock::now();
        metrics.meshUploadTime = std::chrono::duration<float, std::milli>(uploadEndTime - uploadStartTime).count();
    }

    // UPDATED: Enhanced rendering with biome tinting support
    void RenderScene(const Render::Camera& camera, const Shader& blockShader,
                    const glm::mat4& proj, const glm::mat4& view, const Frustum& frustum,
                    Debug::PerformanceMetrics& metrics) {

        auto renderStartTime = std::chrono::high_resolution_clock::now();

        // Use shader
        blockShader.Use();

        // Bind textures based on available systems
        if (Render::g_atlasBuilder && Render::g_atlasBuilder->GetAtlasTextureID() != 0) {
            // Use AtlasBuilder system
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, Render::g_atlasBuilder->GetAtlasTextureID());
            glUniform1i(blockShader.GetUniformLocation("uTextureAtlas"), 0);

            // Bind biome colormaps if available
            GLuint grassColormap = Render::g_atlasBuilder->GetGrassColormapID();
            GLuint foliageColormap = Render::g_atlasBuilder->GetFoliageColormapID();

            if (grassColormap != 0) {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, grassColormap);
                glUniform1i(blockShader.GetUniformLocation("uGrassColormap"), 1);
            }

            if (foliageColormap != 0) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, foliageColormap);
                glUniform1i(blockShader.GetUniformLocation("uFoliageColormap"), 2);
            }

            // Set biome parameters
            glUniform1f(blockShader.GetUniformLocation("uBiomeTemperature"), 0.7f); // Temperate
            glUniform1f(blockShader.GetUniformLocation("uBiomeHumidity"), 0.6f);    // Moderate
            glUniform1i(blockShader.GetUniformLocation("uEnableBiomeTinting"), 1);  // Enable

        } else {
            // Fall back to legacy texture atlas
            Render::g_textureAtlas.Bind(GL_TEXTURE0);
            glUniform1i(blockShader.GetUniformLocation("uTextureAtlas"), 0);

            // Disable biome tinting for legacy system
            glUniform1i(blockShader.GetUniformLocation("uEnableBiomeTinting"), 0);
        }

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

    // UPDATED: Initialize enhanced game systems with proper asset paths
    void InitializeGameSystems() {
        Log::Info("Initializing game systems...");

        // Initialize both registries for backward compatibility
        Game::BlockRegistry::Init();
        Game::EnhancedBlockRegistry::Init();

        // FIXED: Use platform-specific asset path function
        std::string modelsPath = GetAssetPath("assets/models/block");

        // Load block models
        if (!Game::BlockModelRegistry::LoadModels(modelsPath)) {
            Log::Warning("Failed to load block models from %s, using default models", modelsPath.c_str());
        }

        Log::Info("Game systems initialized successfully");
    }

    // UPDATED: Initialize enhanced shaders with proper asset paths
    Shader InitializeShaders() {
        // FIXED: Use platform-specific asset paths
        std::string enhancedVertPath = GetAssetPath("shaders/enhanced_block.vert");
        std::string enhancedFragPath = GetAssetPath("shaders/enhanced_block.frag");
        std::string standardVertPath = GetAssetPath("shaders/block.vert");
        std::string standardFragPath = GetAssetPath("shaders/block.frag");

        // Try to use enhanced shaders if available
        if (std::filesystem::exists(enhancedVertPath) && std::filesystem::exists(enhancedFragPath)) {
            Log::Info("Using enhanced block shaders with biome tinting support");
            return Shader(enhancedVertPath, enhancedFragPath);
        } else {
            Log::Info("Enhanced shaders not found, using standard block shaders");
            return Shader(standardVertPath, standardFragPath);
        }
    }

    // UPDATED: Initialize enhanced texture systems with proper asset paths
    bool InitializeTextureSystem() {
        // Initialize legacy texture atlas first
        std::string atlasPath = GetAssetPath("assets/textures/atlas.png");
        if (!Render::g_textureAtlas.Initialize(atlasPath)) {
            Log::Error("Failed to initialize legacy texture atlas");
            return false;
        }

        // Initialize AtlasBuilder
        Render::g_atlasBuilder = std::make_unique<Render::AtlasBuilder>();
        std::string atlasJsonPath = GetAssetPath("assets/atlases/blocks.json");
        std::string texturesPath = GetAssetPath("assets/textures");

        if (!Render::g_atlasBuilder->BuildFromJSON(atlasJsonPath, texturesPath)) {
            Log::Warning("AtlasBuilder failed to build from JSON at %s, falling back to legacy system",
                        atlasJsonPath.c_str());
            Render::g_atlasBuilder.reset();
        } else {
            Log::Info("AtlasBuilder initialized successfully: %dx%d atlas with %zu textures",
                     Render::g_atlasBuilder->GetAtlasWidth(),
                     Render::g_atlasBuilder->GetAtlasHeight(),
                     Render::g_atlasBuilder->GetTextureCount());
        }

        return true;
    }

    // Callback for OpenGL debug messages
    void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam) {
        Log::Error("[GL DEBUG] %s", message);
    }

    int Run(int argc, char** argv) {
        // Initialize systems
        Log::Init();
        Log::Info("Starting MyVoxelGame v0.1 with Enhanced Model System");

        // UPDATED: Initialize enhanced game systems
        InitializeGameSystems();

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

        // UPDATED: Initialize enhanced texture systems
        if (!InitializeTextureSystem()) {
            Log::Error("Failed to initialize texture systems");
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

        // UPDATED: Initialize crosshair with proper asset path
        std::string crosshairPath = GetAssetPath("assets/textures/gui/sprites/hud/crosshair.png");
        if (!Render::g_crosshair.Initialize(crosshairPath)) {
            Log::Warning("Failed to initialize crosshair system, continuing without crosshair");
        }

        // UPDATED: Compile enhanced shaders
        Shader blockShader = InitializeShaders();

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

        Log::Info("Entering main render loop with enhanced model system");

        // MAIN LOOP
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

            // Render crosshair (must be last to appear on top)
            RenderCrosshair(window);

            // Render debug UI
            int windowWidth, windowHeight;
            glfwGetWindowSize(window, &windowWidth, &windowHeight);
            Debug::DebugSystem::RenderDebugUI(
                camera, frustum, playerController, metrics, cursorEnabled,
                windowWidth, windowHeight, width, height
            );

            // Finish debug frame
            Debug::DebugSystem::EndFrame();

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