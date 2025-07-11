// File: src/platform/PlatformMain.cpp
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "Input.hpp"
#include "Log.hpp"
#include "Config.hpp"
#include "../render/debug/DebugSystem.hpp"

// Include game headers
#include "../engine/block/BlockRegistry.hpp"
#include "../engine/block/BlockModel.hpp"
#include "../game/PlayerController.hpp"
#include "../engine/physics/RayCast.hpp"
#include "../engine/physics/Physics.hpp"

// Include rendering headers
#include "../render/gfx/Camera.hpp"
#include "../render/gfx/Frustum.hpp"
#include "../render/gfx/Shader.hpp"
#include "../render/mesh/BlockHighlight.hpp"
#include "../render/debug/Crosshair.hpp"
#include "../render/atlas/AtlasBuilder.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <queue>
#include <unordered_map>
#include <memory>

#include "GameDirectory.hpp"
#include "JobSystem.hpp"
#include "engine/world/RegionFileCache.hpp"
#include "engine/world/SectionDataUnpacker.hpp"
#include "mesh/ChunkRenderer.hpp"
#include "mesh/MeshManager.hpp"
#include "engine/world/WorldGlobals.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#endif

namespace Game {
    // Global world reference for debug system
    World* g_world = nullptr;
}

namespace PlatformMain {

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

    void InitializeGameSystems() {
        Log::Info("Initializing game systems...");

        // Initialize block registries
        Game::BlockRegistry::Init();

        // Use platform-specific asset path function
        std::string modelsPath = GetAssetPath("assets/models/block");

        // Load block models
        if (!Game::BlockModelRegistry::LoadModels(modelsPath)) {
            Log::Warning("Failed to load block models from %s, using default models", modelsPath.c_str());
        }

        Log::Info("✓ Game systems initialized");
    }

    Shader InitializeShaders() {
        // Use platform-specific asset paths
        std::string vertPath = GetAssetPath("shaders/block.vert");
        std::string fragPath = GetAssetPath("shaders/block.frag");

        // Try to use shaders if available
        if (std::filesystem::exists(vertPath) && std::filesystem::exists(fragPath)) {
            Log::Info("Using block shaders");
            return Shader(vertPath, fragPath);
        }

        // **FIX**: Return a valid shader even if files don't exist
        Log::Warning("Shader files not found, creating basic fallback shader");
        return Shader(vertPath, fragPath); // This will create a basic shader even if files don't exist
    }

    bool InitializeTextureSystem() {
        // Initialize AtlasBuilder
        Render::g_atlasBuilder = std::make_unique<Render::AtlasBuilder>();
        std::string atlasJsonPath = GetAssetPath("assets/atlases/blocks.json");
        std::string texturesPath = GetAssetPath("assets/textures");

        if (!Render::g_atlasBuilder->BuildFromJSON(atlasJsonPath, texturesPath)) {
            Log::Warning("AtlasBuilder failed to build from JSON at %s",
                        atlasJsonPath.c_str());
            Render::g_atlasBuilder.reset();
            return false;
        }
        Log::Info("AtlasBuilder initialized successfully: %dx%d atlas with %zu textures",
                 Render::g_atlasBuilder->GetAtlasWidth(),
                 Render::g_atlasBuilder->GetAtlasHeight(),
                 Render::g_atlasBuilder->GetTextureCount());
        return true;
    }

    // [Keep all existing input and utility functions unchanged]
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

    void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam) {
        Log::Error("[GL DEBUG] %s", message);
    }

    bool InitializeMinecraftSupport() {
        Log::Info("=== MINECRAFT WORLD SUPPORT INITIALIZATION ===");

        // Initialize the block state registry for converting Minecraft blocks
        Game::BlockStateRegistry::Initialize();
        Log::Info("✓ Block state registry initialized");

        // Check for common Minecraft world locations
        std::vector<std::string> commonPaths = {
            "world",                    // Current directory
            "../world",                 // Parent directory
            "saves/New World",          // Typical save name
            "saves/World",              // Another common name
            std::string(getenv("HOME") ? getenv("HOME") : "") + "/library/Application Support/minecraft/saves/test1",  // Default Minecraft location
        };

        bool foundWorld = false;
        for (const auto& path : commonPaths) {
            if (path.empty()) continue;

            if (Game::ChunkProvider::LoadMinecraftWorld(path)) {
                Log::Info("✓ Automatically loaded Minecraft world: %s", path.c_str());
                foundWorld = true;
                break;
            }
        }

        if (!foundWorld) {
            Log::Info("No Minecraft world auto-detected, will use procedural generation");
        }

        Log::Info("=== MINECRAFT WORLD SUPPORT READY ===");
        return true;
    }

    bool ProcessWorldArguments(int argc, char* argv[]) {
        for (int i = 1; i < argc - 1; ++i) {
            std::string arg = argv[i];

            if (arg == "--world" || arg == "-w") {
                std::string worldPath = argv[i + 1];
                Log::Info("Loading Minecraft world from command line: %s", worldPath.c_str());

                /*if (Game::ChunkProvider::LoadMinecraftWorld(worldPath)) {
                    Log::Info("✓ Successfully loaded world: %s", worldPath.c_str());
                    return true;
                } else {
                    Log::Error("✗ Failed to load world: %s", worldPath.c_str());
                    return false;
                }*/
            }
        }

        return false; // No world argument found
    }

    int Run(int argc, char** argv) {
        // Initialize systems
        Log::Init();
        Log::Info("Starting MyVoxelGame v0.1 with Enhanced Mesher System");

        // Initialize game directory system (creates obeycraft folder and loads options.txt)
        if (!Platform::InitializeGameDirectorySystem()) {
            Log::Error("Failed to initialize game directory system");
            return -1;
        }

        // Process world arguments and initialize Minecraft support FIRST
        ProcessWorldArguments(argc, argv);
        InitializeMinecraftSupport();

        // Initialize game systems BEFORE any chunk loading
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

        // Initialize texture systems
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

        // Initialize crosshair with proper asset path
        std::string crosshairPath = GetAssetPath("assets/textures/gui/sprites/hud/crosshair.png");
        if (!Render::g_crosshair.Initialize(crosshairPath)) {
            Log::Warning("Failed to initialize crosshair system, continuing without crosshair");
        }

        // Compile shaders
        Shader blockShader = InitializeShaders();

        // Setup OpenGL state
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glfwSwapInterval(1); // VSync

        // Initialize game systems
        Render::Camera camera;
        camera.position = glm::vec3(0.0f, 97.0f, 0.0f);
        camera.physicsControlled = true;

        // Create PlayerController
        Game::PlayerController playerController;

        // Initialize the mesh system
        Game::World world;
        world.Initialize();

        Game::g_world = &world;

        // Initialize mesh system with the world
        Render::MeshManagerConfig meshConfig;
        meshConfig.maxMeshesPerFrame = 3;        // Limit uploads per frame
        meshConfig.maxBuildTimeMs = 8.0f;        // Max time per frame for processing
        meshConfig.enableAsyncBuilding = true;   // Use background threads
        meshConfig.highPriorityRadius = 64.0f;   // High priority radius

        Render::InitializeMeshSystem(&world, meshConfig);

        // Initialize chunk renderer
        if (!Render::InitializeChunkRenderer()) {
            Log::Error("Failed to initialize chunk renderer");
            return -6;
        }

        // Set world reference for player controller
        playerController.SetWorld(&world);

        // Initialize debug system
        Debug::DebugSystem::Initialize(window);

        // Performance tracking
        Debug::PerformanceMetrics metrics;
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        Log::Info("Entering main render loop with Enhanced Mesher System");

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
            world.Update(dt);  // Make sure world is updated

            //Set player position for mesh system prioritization
            glm::vec3 playerPos = playerController.GetPhysics().position;
            Render::SetMeshSystemPlayerPosition(playerPos);

            // Update mesh system
            Render::UpdateMeshSystem(dt);

            // Get player chunk position for chunk loading
            int playerChunkX = static_cast<int>(std::floor(playerPos.x / Game::Math::CHUNK_SIZE_X));
            int playerChunkZ = static_cast<int>(std::floor(playerPos.z / Game::Math::CHUNK_SIZE_Z));

            // Update chunk loading around player
            world.UpdateLoadedChunks(playerChunkX, playerChunkZ, 8);

            // Start debug frame
            Debug::DebugSystem::BeginFrame();

            // Clear and setup rendering
            glClearColor(0.5f, 0.7f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Calculate matrices
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            float aspect = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);

            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.01f, 500.0f);
            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 viewProj = proj * view;
            Frustum frustum = Frustum::FromMatrix(viewProj);

            // **CRITICAL FIX 4**: Check if we have meshes to render
            if (Render::g_meshManager) {
                size_t activeSections = Render::g_meshManager->GetGPUDataManager().GetSectionCount();
                size_t pendingJobs = Render::g_meshManager->GetPendingJobCount();
                size_t completedResults = Render::g_meshManager->GetCompletedResultCount();

                // Log mesh stats occasionally
                static float meshLogTimer = 0.0f;
                meshLogTimer += dt;
                if (meshLogTimer >= 2.0f) { // Every 2 seconds
                    Log::Info("Mesh Status: %zu active sections, %zu pending jobs, %zu completed results",
                             activeSections, pendingJobs, completedResults);
                    meshLogTimer = 0.0f;
                }
            }

            // Render chunks using the new system
            Render::RenderChunksAll(camera, frustum);

            // Render UI elements
            RenderBlockHighlight(playerController, viewProj);

            // Render crosshair (must be last to appear on top)
            RenderCrosshair(window);

            // **NEW**: Update debug UI with mesh statistics
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

        // Shutdown mesh system first (stops background threads)
        Render::ShutdownMeshSystem();

        // Shutdown chunk renderer
        Render::ShutdownChunkRenderer();

        // Shutdown world
        world.Shutdown();

        Game::g_world = nullptr;

        // Clear region file cache
        World::RegionFileCache::Instance().Clear();

        // Cleanup
        Debug::DebugSystem::Shutdown();

        Log::Info("Stopping background job system...");
        try {
            JobSystem::g_ThreadPool.Stop();
            Log::Info("Job system stopped successfully");
        } catch (const std::exception& e) {
            Log::Error("Exception stopping job system: %s", e.what());
        } catch (...) {
            Log::Error("Unknown exception stopping job system");
        }

        // Clean up OpenGL resources with better error handling
        Log::Info("Cleaning up OpenGL resources...");
        try {
            // Make sure we have a valid OpenGL context
            if (glfwGetCurrentContext() == window) {

                // Clean up global rendering resources
                if (Render::g_atlasBuilder) {
                    Render::g_atlasBuilder.reset();
                }

                // Clear any remaining OpenGL errors
                while (glGetError() != GL_NO_ERROR) {
                    // Clear error queue
                }
            } else {
                Log::Warning("No valid OpenGL context during cleanup - skipping OpenGL resource cleanup");
            }
        } catch (const std::exception& e) {
            Log::Error("Exception during OpenGL cleanup: %s", e.what());
        } catch (...) {
            Log::Error("Unknown exception during OpenGL cleanup");
        }

        Log::Info("Enhanced Voxel Engine shutting down");

        // Destroy window and terminate GLFW with error handling
        try {
            glfwDestroyWindow(window);
            glfwTerminate();
        } catch (...) {
            Log::Error("Exception during GLFW cleanup");
        }

        return 0;
    }

} // namespace PlatformMain