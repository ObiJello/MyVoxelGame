// File: src/platform/PlatformMain.cpp
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "client/input/Input.hpp"
#include "common/core/Log.hpp"
#include "common/core/Config.hpp"
#include "common/core/ThreadAllocator.hpp"
#include "client/renderer/debug/DebugSystem.hpp"
// Include game headers
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockModel.hpp"
#include "client/input/PlayerController.hpp"
#include "common/physics/RayCast.hpp"
#include "common/physics/Physics.hpp"

// Include rendering headers
#include "client/renderer/core/Camera.hpp"
#include "client/renderer/core/Frustum.hpp"
#include "client/renderer/shader/Shader.hpp"
#include "client/renderer/mesh/BlockHighlight.hpp"
#include "client/renderer/debug/Crosshair.hpp"
#include "client/renderer/texture/AtlasBuilder.hpp"
#include "client/renderer/texture/TextureAnimator.hpp"
#include "common/core/Profiling.hpp"

// Include world system headers
#include "common/world/level/World.hpp"
#include "common/world/level/WorldGlobals.hpp"
#include "server/world/ChunkProvider.hpp"

// Include mesh system headers
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "client/renderer/mesh/ClientMeshManager.hpp"

// Include new Minecraft-style architecture
#include "client/network/NetworkClient.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkIOService.hpp"
#include "server/IntegratedServer.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "server/world/ServerWorkerPool.hpp"
#include "client/world/ClientWorkerPool.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <atomic>

#include "platform/GameDirectory.hpp"
#include "common/core/JobSystem.hpp"

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

    Shader InitializeShaders() {
        // Use platform-specific asset paths
        std::string vertPath = GetAssetPath("shaders/block.vert");
        std::string fragPath = GetAssetPath("shaders/block.frag");

        // Try to use shaders if available
        if (std::filesystem::exists(vertPath) && std::filesystem::exists(fragPath)) {
            Log::Info("Using block shaders");
            return Shader(vertPath, fragPath);
        }

        // Return a valid shader even if files don't exist
        Log::Warning("Shader files not found, creating basic fallback shader");
        return Shader(vertPath, fragPath); // This will create a basic shader even if files don't exist
    }

    bool InitializeTextureSystem() {
        // Initialize TextureAnimator first
        Render::g_textureAnimator = std::make_unique<Render::TextureAnimator>();
        
        // Initialize AtlasBuilder
        Render::g_atlasBuilder = std::make_unique<Render::AtlasBuilder>();
        
        // Connect the TextureAnimator to the AtlasBuilder
        Render::g_atlasBuilder->SetTextureAnimator(Render::g_textureAnimator.get());
        
        std::string atlasJsonPath = GetAssetPath("assets/atlases/blocks.json");
        std::string texturesPath = GetAssetPath("assets/textures");

        if (!Render::g_atlasBuilder->BuildFromJSON(atlasJsonPath, texturesPath)) {
            Log::Warning("AtlasBuilder failed to build from JSON at %s",
                        atlasJsonPath.c_str());
            Render::g_atlasBuilder.reset();
            Render::g_textureAnimator.reset();
            return false;
        }
        Log::Info("AtlasBuilder initialized successfully: %dx%d atlas with %zu textures",
                 Render::g_atlasBuilder->GetAtlasWidth(),
                 Render::g_atlasBuilder->GetAtlasHeight(),
                 Render::g_atlasBuilder->GetTextureCount());
        return true;
    }

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


    void UpdateMeshSystemIntegration(Game::World& world) {
        // Get dirty sections from the world's chunk provider
        auto dirtySections = world.GetDirtySections();

        // TODO: Server-side mesh management has been refactored
        // Dirty sections should now be handled through the ClientChunkManager -> ClientMeshManager pipeline
        // This server-side mesh marking functionality is no longer available in ClientMeshManager

        // Clear the processed sections
        world.ClearDirtySections(dirtySections);
    }

    bool InitializeGameSystems(GLFWwindow* window) {
        Log::Info("Initializing game systems...");

        // Initialize block registries
        Game::BlockRegistry::Init();

        // Use platform-specific asset path function
        std::string modelsPath = GetAssetPath("assets/models/block");

        // Load block models
        if (!Game::BlockModelRegistry::LoadModels(modelsPath)) {
            Log::Warning("Failed to load block models from %s, using default models", modelsPath.c_str());
        }

        // Initialize texture systems
        if (!InitializeTextureSystem()) {
            Log::Error("Failed to initialize texture systems");
            glfwDestroyWindow(window);
            glfwTerminate();
            return false;
        }

        if (!Render::g_blockHighlight.Initialize()) {
            Log::Error("Failed to initialize block highlight system");
            glfwDestroyWindow(window);
            glfwTerminate();
            return false;
        }

        // Initialize crosshair with proper asset path
        std::string crosshairPath = GetAssetPath("assets/textures/gui/sprites/hud/crosshair.png");
        if (!Render::g_crosshair.Initialize(crosshairPath)) {
            Log::Warning("Failed to initialize crosshair system, continuing without crosshair");
        }

        // Compile shaders
        Shader blockShader = InitializeShaders();

        Log::Info("✓ Game systems initialized");
        return true;
    }

    int Run(int argc, char** argv) {
        // Initialize systems
        Log::Info("Starting Voxel Engine");

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

        // Log system info
        Log::Info("Vendor: %s", glGetString(GL_VENDOR));
        Log::Info("Renderer: %s", glGetString(GL_RENDERER));
        Log::Info("Version: %s", glGetString(GL_VERSION));

        // Setup OpenGL state
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glfwSwapInterval(1); // VSync

        // Initialize game directory system (creates obeycraft folder and loads options.txt)
        if (!Platform::InitializeGameDirectorySystem()) {
            Log::Error("Failed to initialize game directory system");
            return -1;
        }

        // Initialize input
        Input::Init(window);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        // Initialize game systems BEFORE any chunk loading
        if (!InitializeGameSystems(window)) {
            Log::Error("Failure to init");
            return 1;
        }

        // === MINECRAFT-STYLE ARCHITECTURE INITIALIZATION ===
        Log::Info("Initializing Minecraft Java Edition Architecture...");
        
        // 1. Initialize server-side systems (server creates and owns the world)
        Server::IntegratedServerConfig serverConfig;
        serverConfig.tickRate = 20;                     // 20 TPS like Minecraft
        serverConfig.enableAsyncChunkLoading = false;    // Use ServerWorkerPool
        
        // Automatically use local save directory if available (temporary feature)
        if (serverConfig.useLocalSaveDirectory && Platform::g_gameDirectory.HasDefaultSaveWorld()) {
            // Point to the saves/world folder (not /region, as MinecraftChunkLoader adds that)
            serverConfig.minecraftWorldPath = Platform::g_gameDirectory.GetSavesDirectory() + "/world";
            Log::Info("✓ Auto-detected Minecraft save at: %s", serverConfig.minecraftWorldPath.c_str());
        } else {
            Log::Info("No local Minecraft save found, will use procedural generation");
        }
        
        Server::InitializeIntegratedServer(serverConfig);
        Log::Info("✓ IntegratedServer initialized (20 TPS, world created on server)");
        
        // Get world reference for legacy systems (temporary)
        Game::World* world = Server::g_integratedServer->GetWorld();
        Game::g_world = world;
        
        // 3. Initialize worker pools with dynamic thread allocation
        Core::ThreadAllocation threadAlloc = Core::ThreadAllocator::GetOptimalAllocation();
        Threading::InitializeServerWorkerPool(threadAlloc.serverWorldWorkers);
        Threading::InitializeClientWorkerPool(threadAlloc.clientMeshWorkers);
        Log::Info("✓ Worker pools initialized - %s", threadAlloc.ToString().c_str());
        
        // 4. Initialize client-side systems
        Client::InitializeClientChunkManager();
        Render::InitializeClientMeshManager(Client::g_clientChunkManager.get());
        Log::Info("✓ Client systems initialized (chunk manager, mesh manager)");
        
        // 5. Initialize player controller (no longer needs world directly)
        Game::PlayerController playerController;
        playerController.SetWorld(world);  // Temporary until we fully remove world dependency
        
        // 6. Configure IntegratedServer with player controller
        Server::g_integratedServer->SetPlayerController(&playerController);
        
        // 7. Initialize rendering systems (keeping existing ones that still work)
        if (!Render::InitializeChunkRenderer()) {
            Log::Error("Failed to initialize chunk renderer");
            return -7;
        }
        
        // 8. Initialize debug system
        Debug::DebugSystem::Initialize(window);
        
        // 9. Start the IntegratedServer thread (20 TPS)
        Server::StartIntegratedServer();
        Log::Info("✓ IntegratedServer thread started (20 TPS)");
        
        // 10. Initialize Network I/O Service (dedicated I/O thread like Minecraft's Netty)
        Client::InitializeNetworkIOService();
        Log::Info("✓ Network I/O Service started (dedicated I/O thread)");
        
        // 11. Create NetworkClient and connect to localhost server
        auto networkClient = std::make_unique<Client::NetworkClient>(Client::g_networkIOService->GetIOContext());
        Client::g_networkClient = networkClient.get();  // Set global pointer for legacy systems
        
        // Use async connect with callback (Minecraft/Netty style)
        // Use shared_ptr to ensure atomics remain valid for async callbacks
        auto connected = std::make_shared<std::atomic<bool>>(false);
        auto connectionComplete = std::make_shared<std::atomic<bool>>(false);
        
        networkClient->SetOnConnected([connected, connectionComplete]() {
            Log::Info("✓ Connection established");
            *connected = true;
            *connectionComplete = true;
        });
        
        networkClient->SetOnError([connectionComplete](const std::string& error) {
            Log::Error("Connection failed: %s", error.c_str());
            *connectionComplete = true;
        });
        
        // Start async connection (all socket ops on I/O thread)
        Log::Info("Connecting to localhost:25565...");
        networkClient->ConnectAsync("127.0.0.1", 25565);
        
        // Wait for connection with timeout (using yield instead of sleep)
        auto startTime = std::chrono::steady_clock::now();
        while (!*connectionComplete) {
            if (std::chrono::steady_clock::now() - startTime > std::chrono::seconds(2)) {
                Log::Error("Connection timeout after 2 seconds");
                return -8;
            }
            std::this_thread::yield();
        }
        
        if (!*connected) {
            Log::Error("Failed to connect to server");
            return -8;
        }
        
        Log::Info("✓ NetworkClient connected to localhost:25565");
        Log::Info("✓ Handshake automatically sent to server");
        
        Log::Info("🎮 Minecraft Java Edition Architecture fully initialized!");
        Log::Info("   Server Thread: 20 TPS | Client Thread: Unlocked FPS | I/O Thread: Async | Workers: 6 threads");
        Log::Info("   Client connected via TCP to localhost:25565");
        Log::Info("=== ENTERING CLIENT RENDER LOOP (UNLOCKED FPS) ===");

        // === MINECRAFT-STYLE CLIENT RENDER LOOP (UNLOCKED FPS) ===
        
        // Initialize camera for client thread
        Render::Camera camera;
        camera.position = glm::vec3(0.0f, 67.0f, 0.0f);
        camera.physicsControlled = true;
        
        // Network tracking
        uint32_t playerMoveSequence = 0;
        
        // Performance tracking
        Debug::PerformanceMetrics metrics;
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        // CLIENT RENDER LOOP (like Minecraft's GameRenderer.render)
        while (!glfwWindowShouldClose(window)) {
            frameStartTime = std::chrono::high_resolution_clock::now();
            
            // 1. Drain incoming network packets on main thread (Minecraft-style)
            PROFILE_TIMER_START(network);
            // Note: I/O is handled on dedicated thread, we just drain the queue here
            if (networkClient) {
                networkClient->DrainIncomingPackets();
            }
            PROFILE_TIMER_END(network, metrics.networkProcessingTime);

            // 2. Process completed mesh build results from worker threads
            PROFILE_TIMER_START(meshresults);
            Render::ProcessClientMeshBuildResults();  // Drain MeshResultQueue
            PROFILE_TIMER_END(meshresults, metrics.meshResultProcessingTime);

            // 3. Poll events and handle input
            PROFILE_TIMER_START(input);
            glfwPollEvents();
            Input::UpdateKeyStates();

            // Handle escape key
            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            bool cursorEnabled = HandleCursorToggle(window, camera);
            HandlePlayerInput(playerController, camera);
            PROFILE_TIMER_END(input, metrics.inputHandlingTime);

            // Frame counter for debugging
            static uint64_t frameCounter = 0;
            frameCounter++;
            if (frameCounter % 60 == 0) {
                Log::Debug("MAIN LOOP: Frame %llu at time %.2f", frameCounter, Time::Now());
            }
            
            // 4. Update time and game logic
            PROFILE_TIMER_START(gamelogic);
            if (frameCounter % 60 == 0) Log::Debug("MAIN LOOP: Step 1 - Update time and logic");
            Time::Tick();
            float dt = static_cast<float>(Time::Delta());
            metrics.AddFrameTimeSample(dt);

            // Update camera and player
            camera.Update(dt);
            playerController.Update(dt, camera);
            PROFILE_TIMER_END(gamelogic, metrics.gameLogicTime);

            // 5. Set player position for mesh prioritization
            if (frameCounter % 60 == 0) Log::Debug("MAIN LOOP: Step 2 - Set player position");
            glm::vec3 playerPos = playerController.GetPhysics().position;
            Render::SetClientMeshPlayerPosition(playerPos);
            
            // Send player position to server via TCP (throttled to 50ms / 20Hz)
            // Static variables MUST be outside the if block to ensure they persist
            static auto lastPlayerSendTime = std::chrono::steady_clock::now();
            static bool playerSendInitialized = false;
            
            if (networkClient->IsConnected()) {
                auto now = std::chrono::steady_clock::now();
                
                // Initialize on first run
                if (!playerSendInitialized) {
                    lastPlayerSendTime = now;
                    playerSendInitialized = true;
                }
                
                auto timeSinceLastSend = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPlayerSendTime);
                
                // Only send every 50ms (20 times per second)
                if (timeSinceLastSend.count() >= 50) {
                    // Send player movement packet
                    Network::PlayerMoveC2SPacket movePacket;
                    movePacket.position = playerPos;
                    movePacket.rotation = glm::vec2(camera.yaw, camera.pitch);
                    movePacket.onGround = playerController.GetPhysics().isOnGround;
                    movePacket.sequenceNumber = ++playerMoveSequence;
                    movePacket.timestamp = std::chrono::steady_clock::now();
                    
                    networkClient->GetConnection()->SendPlayerMove(movePacket);
                    
                    // Update last send time
                    lastPlayerSendTime = now;
                }
            }

            // 6. Schedule mesh builds for LOADED chunks
            PROFILE_TIMER_START(meshsched);
            if (frameCounter % 60 == 0) Log::Debug("MAIN LOOP: Step 3 - Schedule mesh builds");
            Render::ScheduleClientMeshBuilds(playerPos);
            if (frameCounter % 60 == 0) Log::Debug("MAIN LOOP: Step 3 - Schedule mesh builds COMPLETE");
            PROFILE_TIMER_END(meshsched, metrics.meshSchedulingTime);

            // 7. Perform GPU uploads
            PROFILE_TIMER_START(gpuupload);
            if (frameCounter % 60 == 0) Log::Debug("MAIN LOOP: Step 4 - GPU uploads");
            Render::PerformClientGPUUploads();
            if (frameCounter % 60 == 0) Log::Debug("MAIN LOOP: Step 4 - GPU uploads COMPLETE");
            PROFILE_TIMER_END(gpuupload, metrics.gpuUploadTime);

            // 8. Update texture animations
            PROFILE_TIMER_START(texanim);
            if (Render::g_textureAnimator) {
                Render::g_textureAnimator->UpdateAnimations(dt);
            }
            PROFILE_TIMER_END(texanim, metrics.textureAnimationTime);

            // 9. Main rendering phase (frustum culling + GPU draw calls)
            PROFILE_TIMER_START(render);
            Debug::DebugSystem::BeginFrame();
            
            // Reset per-frame render stats
            metrics.ResetFrameMetrics();

            // Clear framebuffer
            glClearColor(0.5f, 0.7f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Calculate view-projection matrices
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            float aspect = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);

            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.01f, 800.0f);
            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 viewProj = proj * view;
            Frustum frustum = Frustum::FromMatrix(viewProj);

            // Main chunk rendering (includes frustum culling and all render passes)
            Render::RenderChunksAll(camera, frustum);
            
            // Get rendering statistics from ChunkRenderer
            if (auto* renderStats = Render::GetChunkRendererStats()) {
                metrics.meshesRenderedThisFrame = renderStats->sectionsRendered;
                metrics.totalVerticesRendered = renderStats->totalVerticesRendered;
                metrics.totalIndicesRendered = renderStats->totalIndicesRendered;
                metrics.opaqueMeshesRendered = renderStats->opaqueSections;
                metrics.cutoutMeshesRendered = renderStats->cutoutSections;
                metrics.translucentMeshesRendered = renderStats->translucentSections;
            }

            // Render UI overlay elements
            RenderBlockHighlight(playerController, viewProj);
            RenderCrosshair(window);
            PROFILE_TIMER_END(render, metrics.renderTime);

            // 10. Debug UI with new architecture statistics
            PROFILE_TIMER_START(debugui);
            int windowWidth, windowHeight;
            glfwGetWindowSize(window, &windowWidth, &windowHeight);

            Debug::DebugSystem::RenderDebugUI(
                camera, frustum, playerController, metrics, cursorEnabled,
                windowWidth, windowHeight, width, height
            );

            Debug::DebugSystem::EndFrame();
            PROFILE_TIMER_END(debugui, metrics.debugUITime);

            // 11. Swap buffers (includes VSync wait)
            PROFILE_TIMER_START(vsync);
            glfwSwapBuffers(window);
            PROFILE_TIMER_END(vsync, metrics.vsyncWaitTime);
            
            Input::ResetMouseDelta();
            Input::ResetScrollOffset();

            // Calculate total frame time and unaccounted time
            auto frameEndTime = std::chrono::high_resolution_clock::now();
            metrics.frameTime = std::chrono::duration<float, std::milli>(frameEndTime - frameStartTime).count();
            
            // Calculate unaccounted time (operations we didn't explicitly measure)
            float totalMeasured = metrics.networkProcessingTime + metrics.meshResultProcessingTime + 
                                 metrics.inputHandlingTime + metrics.gameLogicTime + 
                                 metrics.meshSchedulingTime + metrics.gpuUploadTime + 
                                 metrics.textureAnimationTime + metrics.renderTime + 
                                 metrics.debugUITime + metrics.vsyncWaitTime;
            metrics.otherTime = metrics.frameTime - totalMeasured;
        }

        // === MINECRAFT-STYLE SHUTDOWN SEQUENCE ===
        Log::Info("Shutting down...");

        // 1. Disconnect NetworkClient
        Log::Info("Disconnecting NetworkClient...");
        networkClient->Disconnect();
        networkClient.reset();
        Log::Info("✓ NetworkClient disconnected");
        
        // 2. Stop Network I/O Service (dedicated I/O thread)
        Log::Info("Stopping Network I/O Service...");
        Client::ShutdownNetworkIOService();
        Log::Info("✓ Network I/O Service stopped");

        // 3. Stop IntegratedServer thread (20 TPS)
        Log::Info("Stopping IntegratedServer thread...");
        Server::StopIntegratedServer();
        Log::Info("✓ IntegratedServer stopped");
        
        // 4. Stop worker pools (stops background threads)
        Log::Info("Stopping worker thread pools...");
        Threading::ShutdownServerWorkerPool();
        Threading::ShutdownClientWorkerPool();
        Log::Info("✓ Worker pools stopped");
        
        // 5. Note: Chunks are now saved by IntegratedServer during its shutdown
        
        // 6. Shutdown client systems
        Log::Info("Shutting down client systems...");
        Render::ShutdownClientMeshManager();
        Client::ShutdownClientChunkManager();
        Log::Info("✓ Client systems shutdown");
        
        // 7. Shutdown server systems
        Log::Info("Shutting down server systems...");
        Server::ShutdownIntegratedServer();
        Log::Info("✓ Server systems shutdown");
        
        // 8. Shutdown rendering systems
        Render::ShutdownChunkRenderer();
        
        // 9. Clear world reference (world is shutdown by IntegratedServer)
        Game::g_world = nullptr;  // Clear global reference
        
        // 10. Cleanup debug system
        Debug::DebugSystem::Shutdown();
        
        // 11. Stop legacy job system
        Log::Info("Stopping legacy job system...");
        try {
            JobSystem::g_ThreadPool.Stop();
            Log::Info("✓ Legacy job system stopped");
        } catch (const std::exception& e) {
            Log::Error("Exception stopping job system: %s", e.what());
        } catch (...) {
            Log::Error("Unknown exception stopping job system");
        }

        // Clean up OpenGL resources
        Log::Info("Cleaning up OpenGL resources...");
        try {
            if (glfwGetCurrentContext() == window) {
                // Clean up global rendering resources
                if (Render::g_atlasBuilder) {
                    Render::g_atlasBuilder.reset();
                }
                if (Render::g_textureAnimator) {
                    Render::g_textureAnimator.reset();
                }

                // Clear any remaining OpenGL errors
                while (glGetError() != GL_NO_ERROR) {
                    // Clear error queue
                }
            } else {
                Log::Warning("No valid OpenGL context during cleanup");
            }
        } catch (const std::exception& e) {
            Log::Error("Exception during OpenGL cleanup: %s", e.what());
        } catch (...) {
            Log::Error("Unknown exception during OpenGL cleanup");
        }

        Log::Info("🎮 Minecraft Java Edition Architecture shutdown complete!");
        Log::Info("   All threads stopped, all resources cleaned up");

        // 12. Final GLFW cleanup
        Log::Info("Final GLFW cleanup...");
        try {
            glfwDestroyWindow(window);
            glfwTerminate();
            Log::Info("✓ GLFW cleanup complete");
        } catch (...) {
            Log::Error("Exception during GLFW cleanup");
        }

        Log::Info("=== MINECRAFT JAVA EDITION ARCHITECTURE SHUTDOWN COMPLETE ===");
        return 0;
    }

} // namespace PlatformMain