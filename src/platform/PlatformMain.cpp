// File: src/platform/PlatformMain.cpp
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "Input.hpp"
#include "Log.hpp"
#include "Config.hpp"
#include "../render/debug/DebugSystem.hpp"

// Include game headers
#include "../engine/block/BlockRegistry.hpp"
#include "../engine/block/BlockRegistry.hpp"
#include "../engine/block/BlockModel.hpp"
#include "../engine/world/ChunkProvider.hpp"
#include "../engine/world/WorldManager.hpp"
#include "../game/PlayerController.hpp"
#include "../game/WorldAccess.hpp"
#include "../engine/physics/RayCast.hpp"
#include "../engine/physics/Physics.hpp"
#include "../render/mesh/Mesher.hpp"

// Include rendering headers
#include "../render/gfx/Camera.hpp"
#include "../render/mesh/ChunkRenderer.hpp"
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

#include "JobSystem.hpp"
#include "engine/world/RegionFileCache.hpp"
#include "engine/world/SectionDataUnpacker.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#endif

namespace PlatformMain {

    // Platform-specific function to get the correct asset path
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

    // NEW: Enhanced mesh upload queue for layered rendering
    static std::queue<Game::LayeredMeshData*> g_layeredMeshUploadQueue;
    static std::queue<Game::MeshData*> g_legacyMeshUploadQueue;
    static std::mutex g_meshQueueMutex;

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

    // ENHANCED: Global function to enqueue layered mesh data from background threads
    void EnqueueLayeredMeshUpload(Game::LayeredMeshData* meshData) {
        if (!meshData) {
            Log::Warning("EnqueueLayeredMeshUpload called with null meshData");
            return;
        }

        std::lock_guard<std::mutex> lock(g_meshQueueMutex);
        g_layeredMeshUploadQueue.push(meshData);
        // Note: Queue now owns the meshData pointer
    }


    // ENHANCED: Upload mesh data from both queues with proper error handling
    void UploadMeshData(Debug::PerformanceMetrics& metrics) {
        auto uploadStartTime = std::chrono::high_resolution_clock::now();

        int uploadedThisFrame = 0;
        constexpr int MAX_UPLOADS_PER_FRAME = 5; // Limit to prevent frame drops

        try {
            std::lock_guard<std::mutex> lock(g_meshQueueMutex);

            // Process layered mesh uploads (prioritized)
            while (!g_layeredMeshUploadQueue.empty() && uploadedThisFrame < MAX_UPLOADS_PER_FRAME) {
                Game::LayeredMeshData* layeredMeshData = g_layeredMeshUploadQueue.front();
                g_layeredMeshUploadQueue.pop();

                if (layeredMeshData) {
                    try {
                        // Upload to GPU using the enhanced layered system
                        Render::UploadLayeredMesh(layeredMeshData);
                        uploadedThisFrame++;

                        Log::Debug("Uploaded layered mesh: chunk (%d,%d) section %d - "
                                  "Opaque: %zu, Cutout: %zu, Translucent: %zu vertices",
                                  layeredMeshData->chunkXZ.x, layeredMeshData->chunkXZ.z,
                                  layeredMeshData->sectionIndex,
                                  layeredMeshData->opaqueVertices.size(),
                                  layeredMeshData->cutoutVertices.size(),
                                  layeredMeshData->translucentVertices.size());
                    } catch (const std::exception& e) {
                        Log::Error("Failed to upload layered mesh data: %s", e.what());
                        delete layeredMeshData;
                    } catch (...) {
                        Log::Error("Failed to upload layered mesh data: unknown exception");
                        delete layeredMeshData;
                    }
                } else {
                    Log::Warning("Found null layeredMeshData in upload queue");
                }
            }

            // Process legacy mesh uploads (for backward compatibility)
            while (!g_legacyMeshUploadQueue.empty() && uploadedThisFrame < MAX_UPLOADS_PER_FRAME) {
                Game::MeshData* meshData = g_legacyMeshUploadQueue.front();
                g_legacyMeshUploadQueue.pop();

                if (meshData) {
                    try {
                        // Upload using legacy system (converts to layered internally)
                        Render::UploadMesh(meshData);
                        uploadedThisFrame++;
                    } catch (const std::exception& e) {
                        Log::Error("Failed to upload legacy mesh data: %s", e.what());
                        delete meshData;
                    } catch (...) {
                        Log::Error("Failed to upload legacy mesh data: unknown exception");
                        delete meshData;
                    }
                } else {
                    Log::Warning("Found null meshData in legacy upload queue");
                }
            }
        } catch (const std::exception& e) {
            Log::Error("Error in UploadMeshData: %s", e.what());
        } catch (...) {
            Log::Error("Unknown error in UploadMeshData");
        }

        metrics.meshesUploadedThisFrame = uploadedThisFrame;

        auto uploadEndTime = std::chrono::high_resolution_clock::now();
        metrics.meshUploadTime = std::chrono::duration<float, std::milli>(uploadEndTime - uploadStartTime).count();
    }

    // ENHANCED: Rendering function with three-layer support
    void RenderScene(const Render::Camera& camera, const Shader& blockShader,
                    const glm::mat4& proj, const glm::mat4& view, const Frustum& frustum,
                    Debug::PerformanceMetrics& metrics) {

        // Use the enhanced layered rendering system
        Render::RenderLayeredScene(camera, blockShader, proj, view, frustum, metrics);
    }

    // Legacy function to enqueue mesh data from background threads
    void EnqueueMeshUpload(Game::MeshData* meshData) {
        if (!meshData) {
            Log::Warning("EnqueueMeshUpload called with null meshData");
            return;
        }

        std::lock_guard<std::mutex> lock(g_meshQueueMutex);
        g_legacyMeshUploadQueue.push(meshData);
        // Note: Queue now owns the meshData pointer
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

    // Initialize game systems with proper asset paths
    void InitializeGameSystems() {
        Log::Info("Initializing game systems...");

        // Initialize both registries for backward compatibility
        Game::BlockRegistry::Init();
        Game::BlockRegistry::Init();

        // Use platform-specific asset path function
        std::string modelsPath = GetAssetPath("assets/models/block");

        // Load block models
        if (!Game::BlockModelRegistry::LoadModels(modelsPath)) {
            Log::Warning("Failed to load block models from %s, using default models", modelsPath.c_str());
        }

        // Set up mesh upload callback for the mesher
        // This allows background meshing threads to queue mesh data for main thread upload
        Game::SetMeshUploadCallback([](Game::MeshData* data) {
            EnqueueMeshUpload(data);
        });

        // NEW: Layered mesh upload callback for enhanced rendering
        Game::SetLayeredMeshUploadCallback(EnqueueLayeredMeshUpload);

        Log::Info("Game systems initialized successfully");
    }

    // Initialize shaders with proper asset paths
    Shader InitializeShaders() {
        // Use platform-specific asset paths
        std::string vertPath = GetAssetPath("shaders/block.vert");
        std::string fragPath = GetAssetPath("shaders/block.frag");

        // Try to use shaders if available
        if (std::filesystem::exists(vertPath) && std::filesystem::exists(fragPath)) {
            Log::Info("Using block shaders");
            return Shader(vertPath, fragPath);
        }
    }

    // Initialize texture systems with proper asset paths
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

    // Callback for OpenGL debug messages
    void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam) {
        Log::Error("[GL DEBUG] %s", message);
    }

    // **NEW**: Initialize Minecraft world loading support
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
            std::string(getenv("HOME") ? getenv("HOME") : "") + "/.minecraft/saves/New World",  // Default Minecraft location
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

    // **NEW**: Command line argument processing for world loading
    bool ProcessWorldArguments(int argc, char* argv[]) {
        for (int i = 1; i < argc - 1; ++i) {
            std::string arg = argv[i];

            if (arg == "--world" || arg == "-w") {
                std::string worldPath = argv[i + 1];
                Log::Info("Loading Minecraft world from command line: %s", worldPath.c_str());

                if (Game::ChunkProvider::LoadMinecraftWorld(worldPath)) {
                    Log::Info("✓ Successfully loaded world: %s", worldPath.c_str());
                    return true;
                } else {
                    Log::Error("✗ Failed to load world: %s", worldPath.c_str());
                    return false;
                }
            }
        }

        return false; // No world argument found
    }

    // **NEW**: Enhanced debug UI for Minecraft world support
    void DrawMinecraftWorldDebug() {
        if (!ImGui::Begin("Minecraft World Support")) {
            ImGui::End();
            return;
        }

        auto stats = Game::ChunkProvider::GetWorldStats();

        ImGui::Text("=== MINECRAFT WORLD STATUS ===");
        ImGui::Separator();

        if (stats.hasMinecraftWorld) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Minecraft World Loaded");
            ImGui::Text("World Path: %s", stats.worldPath.c_str());

            // Check if region directory exists
            std::string regionPath = stats.worldPath + "/region";
            if (std::filesystem::exists(regionPath)) {
                // Count region files
                int regionCount = 0;
                try {
                    for (const auto& entry : std::filesystem::directory_iterator(regionPath)) {
                        if (entry.path().extension() == ".mca") {
                            regionCount++;
                        }
                    }
                } catch (...) {
                    regionCount = -1;
                }

                if (regionCount >= 0) {
                    ImGui::Text("Region Files: %d", regionCount);
                } else {
                    ImGui::Text("Region Files: Error counting");
                }
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "⚠ No Minecraft World");
            ImGui::Text("Using procedural generation");
        }

        ImGui::Spacing();
        ImGui::Text("Loaded Chunks: %zu", stats.loadedChunks);

        // World loading interface
        ImGui::Separator();
        ImGui::Text("=== LOAD MINECRAFT WORLD ===");

        static char worldPathBuffer[512] = "";
        ImGui::InputText("World Path", worldPathBuffer, sizeof(worldPathBuffer));

        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            // In a real implementation, you'd open a file dialog here
            ImGui::OpenPopup("World Browser");
        }

        if (ImGui::Button("Load World")) {
            std::string path = worldPathBuffer;
            if (!path.empty()) {
                if (Game::ChunkProvider::LoadMinecraftWorld(path)) {
                    Log::Info("Successfully loaded world from UI: %s", path.c_str());
                } else {
                    Log::Error("Failed to load world from UI: %s", path.c_str());
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear World")) {
            Game::ChunkProvider::ClearAllChunks();
            Game::ChunkProvider::SetMinecraftWorldPath("");
            Log::Info("Cleared world and switched to procedural generation");
        }

        // Quick load buttons for common locations
        ImGui::Spacing();
        ImGui::Text("Quick Load:");

        std::vector<std::pair<std::string, std::string>> quickPaths = {
            {"Current Dir", "./world"},
            {"Parent Dir", "../world"},
            {"Test World", "./saves/TestWorld"}
        };

        for (const auto& [name, path] : quickPaths) {
            if (ImGui::Button(name.c_str())) {
                if (Game::ChunkProvider::LoadMinecraftWorld(path)) {
                    strncpy(worldPathBuffer, path.c_str(), sizeof(worldPathBuffer) - 1);
                    worldPathBuffer[sizeof(worldPathBuffer) - 1] = '\0';
                }
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();

        // Chunk loading statistics
        ImGui::Separator();
        ImGui::Text("=== CHUNK STATISTICS ===");

        // Test specific chunk loading
        static int testChunkX = 0, testChunkZ = 0;
        ImGui::InputInt("Test Chunk X", &testChunkX);
        ImGui::InputInt("Test Chunk Z", &testChunkZ);

        if (ImGui::Button("Test Chunk Availability")) {
            Game::Math::ChunkPos testPos{testChunkX, testChunkZ};
            bool available = Game::ChunkProvider::IsMinecraftChunkAvailable(testPos);

            if (available) {
                Log::Info("Chunk (%d, %d) is available in Minecraft world", testChunkX, testChunkZ);
            } else {
                Log::Info("Chunk (%d, %d) not found in Minecraft world (will be generated)", testChunkX, testChunkZ);
            }
        }

        // Region file cache statistics
        ImGui::Spacing();
        size_t cacheSize = World::RegionFileCache::Instance().GetCacheSize();
        ImGui::Text("Region File Cache: %zu files", cacheSize);

        if (ImGui::Button("Clear Region Cache")) {
            World::RegionFileCache::Instance().Clear();
            Log::Info("Cleared region file cache");
        }

        ImGui::End();
    }

    // debug UI for layered rendering
    void DrawLayeredRenderingDebug() {
        if (!ImGui::Begin("Layered Rendering Debug")) {
            ImGui::End();
            return;
        }

        ImGui::Text("=== LAYERED RENDERING SYSTEM ===");
        ImGui::Separator();

        // Validate layered rendering
        bool isValid = Render::ValidateLayeredRendering();
        if (isValid) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Layered Rendering: ACTIVE");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "✗ Layered Rendering: FAILED");
        }

        // Get statistics
        auto stats = Render::GetLastRenderStats();

        ImGui::Spacing();
        ImGui::Text("Render Layer Statistics:");
        ImGui::Text("  Total Visible Chunks: %d", stats.totalVisibleChunks);
        ImGui::Text("  Opaque Draw Calls: %d (%zu vertices)", stats.opaqueDrawCalls, stats.opaqueVertices);
        ImGui::Text("  Cutout Draw Calls: %d (%zu vertices)", stats.cutoutDrawCalls, stats.cutoutVertices);
        ImGui::Text("  Translucent Draw Calls: %d (%zu vertices)", stats.translucentDrawCalls, stats.translucentVertices);

        if (stats.translucentDrawCalls > 0) {
            ImGui::Text("  Translucent Sort Time: %.2fms", stats.sortTime);
        }

        ImGui::Separator();
        ImGui::Text("Queue Status:");

        int layeredQueueSize = 0;
        int legacyQueueSize = 0;
        {
            std::lock_guard<std::mutex> lock(g_meshQueueMutex);
            layeredQueueSize = static_cast<int>(g_layeredMeshUploadQueue.size());
            legacyQueueSize = static_cast<int>(g_legacyMeshUploadQueue.size());
        }

        ImGui::Text("  Layered Mesh Upload Queue: %d", layeredQueueSize);
        ImGui::Text("  Legacy Mesh Upload Queue: %d", legacyQueueSize);

        ImGui::Spacing();
        ImGui::Text("Layer Performance:");
        int totalDrawCalls = stats.opaqueDrawCalls + stats.cutoutDrawCalls + stats.translucentDrawCalls;
        size_t totalVertices = stats.opaqueVertices + stats.cutoutVertices + stats.translucentVertices;

        if (totalDrawCalls > 0) {
            float opaquePercent = (stats.opaqueDrawCalls * 100.0f) / totalDrawCalls;
            float cutoutPercent = (stats.cutoutDrawCalls * 100.0f) / totalDrawCalls;
            float translucentPercent = (stats.translucentDrawCalls * 100.0f) / totalDrawCalls;

            ImGui::Text("  Draw Call Distribution:");
            ImGui::Text("    Opaque: %.1f%%", opaquePercent);
            ImGui::Text("    Cutout: %.1f%%", cutoutPercent);
            ImGui::Text("    Translucent: %.1f%%", translucentPercent);
        }

        ImGui::Separator();
        ImGui::Text("Fluid Rendering:");
        if (stats.translucentVertices > 0) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "✓ Fluids detected in translucent layer");
            ImGui::Text("  Estimated fluid faces: %zu", stats.translucentVertices / 4);
        } else {
            ImGui::Text("  No fluid geometry detected");
        }

        ImGui::Spacing();
        if (ImGui::Button("Regenerate All Chunks (Layered)")) {
            Render::RegenerateAllChunksLayered();
            Log::Info("Regenerated all chunks with layered meshing");
        }

        if (ImGui::Button("Validate Layered Rendering")) {
            bool valid = Render::ValidateLayeredRendering();
            Log::Info("Layered rendering validation: %s", valid ? "PASSED" : "FAILED");
        }

        ImGui::End();
    }

    int Run(int argc, char** argv) {
        // Initialize systems
        Log::Init();
        Log::Info("Starting MyVoxelGame v0.1");

        // **NEW**: Process command line arguments for world loading
        ProcessWorldArguments(argc, argv);

        // Initialize game systems
        InitializeGameSystems();

        // **NEW**: Initialize Minecraft world support
        InitializeMinecraftSupport();

        // Initialize game systems
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
        camera.position = glm::vec3(0.0f, 80.0f, 0.0f);
        camera.physicsControlled = true;

        Game::PlayerController playerController;

        // Initialize debug system
        Debug::DebugSystem::Initialize(window);

        // Performance tracking
        Debug::PerformanceMetrics metrics;
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        Log::Info("Entering main render loop");

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

            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.01f, 500.0f);
            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 viewProj = proj * view;
            Frustum frustum = Frustum::FromMatrix(viewProj);

            // Upload mesh data from background threads (both types)
            UploadMeshData(metrics);

            // Render scene with layered rendering
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

            // **NEW**: Render Minecraft world debug UI
            DrawMinecraftWorldDebug();

            // ENHANCED: Render layered rendering debug UI
            DrawLayeredRenderingDebug();

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

        // **ENHANCED**: Cleanup with Minecraft world support
        Log::Info("Cleaning up Minecraft world support...");

        // Clear region file cache
        World::RegionFileCache::Instance().Clear();

        // Clear all loaded chunks
        Game::ChunkProvider::ClearAllChunks();

        // Cleanup
        Debug::DebugSystem::Shutdown();

        // **NEW**: Stop all background threads BEFORE cleaning up OpenGL resources
        Log::Info("Stopping background job system...");
        // Add a way to stop the thread pool gracefully
        // (You may need to add a Stop() method to JobSystem::ThreadPool)

        // Clean up both mesh upload queues with error handling
        {
            std::lock_guard<std::mutex> lock(g_meshQueueMutex);
            int layeredCleanedUp = 0;
            int legacyCleanedUp = 0;

            while (!g_layeredMeshUploadQueue.empty()) {
                Game::LayeredMeshData* layeredMeshData = g_layeredMeshUploadQueue.front();
                g_layeredMeshUploadQueue.pop();
                if (layeredMeshData) {
                    delete layeredMeshData;
                    layeredCleanedUp++;
                }
            }

            while (!g_legacyMeshUploadQueue.empty()) {
                Game::MeshData* meshData = g_legacyMeshUploadQueue.front();
                g_legacyMeshUploadQueue.pop();
                if (meshData) {
                    delete meshData;
                    legacyCleanedUp++;
                }
            }

            if (layeredCleanedUp > 0 || legacyCleanedUp > 0) {
                Log::Info("Cleaned up %d layered and %d legacy mesh uploads during shutdown",
                         layeredCleanedUp, legacyCleanedUp);
            }
        }

        Log::Info("Stopping job system...");
        try {
            JobSystem::g_ThreadPool.Stop();
            Log::Info("Job system stopped successfully");
        } catch (const std::exception& e) {
            Log::Error("Exception stopping job system: %s", e.what());
        } catch (...) {
            Log::Error("Unknown exception stopping job system");
        }

        // **FIXED**: Clean up OpenGL resources with better error handling
        Log::Info("Cleaning up OpenGL resources...");
        try {
            // Make sure we have a valid OpenGL context
            if (glfwGetCurrentContext() == window) {
                // Clean up chunk meshes
                size_t meshCount = Render::g_chunkMeshes.size();
                for (auto& cm : Render::g_chunkMeshes) {
                    cm.Cleanup(); // Use the proper cleanup method
                }
                Render::g_chunkMeshes.clear();
                Render::g_chunkMeshes.clear();
                Log::Info("Cleaned up %zu chunk meshes", meshCount);

                // **NEW**: Clean up global rendering resources
                if (Render::g_atlasBuilder) {
                    Render::g_atlasBuilder.reset();
                }

                // Clear texture atlas
                // Note: TextureAtlas destructor should handle OpenGL cleanup

                // **NEW**: Explicitly clear any remaining OpenGL errors
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

        Log::Info("Voxel engine shutting down");
        Log::Info("Final statistics: %zu chunks loaded, %zu sections rendered",
                 Game::WorldManager::GetLoadedChunkCount(), Render::g_chunkMeshes.size());

        // **FIXED**: Destroy window and terminate GLFW with error handling
        try {
            glfwDestroyWindow(window);
            glfwTerminate();
        } catch (...) {
            Log::Error("Exception during GLFW cleanup");
        }

        return 0;
    }

} // namespace PlatformMain