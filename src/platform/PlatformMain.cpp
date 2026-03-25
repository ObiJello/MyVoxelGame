// File: src/platform/PlatformMain.cpp
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "client/input/Input.hpp"
#include "common/core/Log.hpp"
#include <sentry.h>
#include "common/core/Config.hpp"
#include "common/core/ThreadAllocator.hpp"
#include "client/renderer/debug/DebugSystem.hpp"
// Include game headers
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockModel.hpp"
#include "client/input/PlayerController.hpp"
#include "client/entity/Player.hpp"
#include "common/physics/RayCast.hpp"
#include "common/physics/Physics.hpp"

// Include rendering headers
#include "client/renderer/core/Camera.hpp"
#include "client/renderer/core/Frustum.hpp"
#include "client/renderer/shader/Shader.hpp"
#include "client/renderer/mesh/BlockHighlight.hpp"
#include "client/renderer/debug/Crosshair.hpp"
#include "client/renderer/gui/GuiAtlas.hpp"
#include "client/renderer/gui/GuiRenderState.hpp"
#include "client/renderer/gui/GuiRenderer.hpp"
#include "client/renderer/gui/GuiGraphics.hpp"
#include "client/renderer/gui/FontRenderer.hpp"
#include "client/renderer/gui/HudRenderer.hpp"
#include "client/renderer/gui/ChatComponent.hpp"
#include "client/renderer/gui/ChatScreen.hpp"
#include <functional>

// Declared in ClientConnection.cpp
extern void SetChatMessageCallback(std::function<void(const std::string&)> callback);
extern void SetChatBubbleCallback(std::function<void(uint32_t, const std::string&)> callback);
#include "client/renderer/texture/AtlasBuilder.hpp"
#include "client/renderer/texture/TextureAnimator.hpp"
#include "common/core/Profiling.hpp"
#include "common/core/Profiling_Tracy.hpp"

// Include world system headers
#include "common/world/level/World.hpp"
#include "common/world/level/WorldGlobals.hpp"
#include "server/world/ChunkProvider.hpp"

// Include mesh system headers
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "client/renderer/mesh/ClientMeshManager.hpp"

// Include new Minecraft-style architecture
#include "client/network/NetworkClient.hpp"
#include "server/session/PlayerSession.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkIOService.hpp"
#include "server/IntegratedServer.hpp"
#include "server/network/NetworkServer.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "server/world/ServerWorkerPool.hpp"
#include "client/world/ClientWorkerPool.hpp"

// Multiplayer player visibility
#include "client/entity/RemotePlayerManager.hpp"
#include "client/renderer/entity/PlayerRenderer.hpp"

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
#include "client/renderer/backend/RenderBackend.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <objc/objc.h>
#include <objc/message.h>
#include <unistd.h>
#endif

namespace Game {
    // Global world reference for debug system
    World* g_world = nullptr;
}


// GUI system globals
static Render::GuiAtlas g_guiAtlas;
static Render::FontRenderer g_fontRenderer;
static Render::GuiRenderer g_guiRenderer;
static Render::HudRenderer g_hudRenderer;
static Render::ChatComponent g_chatComponent;
static Render::ChatScreen g_chatScreen;

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

    void RenderBlockHighlight(const Game::ClientPlayer& player, const glm::mat4& proj, const glm::mat4& view) {
        const auto& hit = player.lastBlockHit;
        if (Render::BlockHighlight::IsValidHighlight(hit)) {
            Render::g_blockHighlight.Render(hit->blockPos, proj, view);
        }
    }

    void RenderCrosshair(GLFWwindow* window) {
        int windowWidth, windowHeight, framebufferWidth, framebufferHeight;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

        Render::g_crosshair.Render(windowWidth, windowHeight, framebufferWidth, framebufferHeight);
    }

    void RenderHUD(GLFWwindow* window, const Game::Inventory& inventory, float deltaTime,
                   const glm::mat4& proj = glm::mat4(1.0f), const glm::mat4& view = glm::mat4(1.0f)) {
        int windowWidth, windowHeight, framebufferWidth, framebufferHeight;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

        if (framebufferWidth <= 0 || framebufferHeight <= 0) return;

        // Calculate GUI scale (MC: auto scale based on window size)
        float scaleX = static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth);
        float guiScale = std::max(1.0f, std::floor(scaleX * 2.0f)); // 2x default, higher on Retina

        int guiWidth = static_cast<int>(static_cast<float>(framebufferWidth) / guiScale);
        int guiHeight = static_cast<int>(static_cast<float>(framebufferHeight) / guiScale);

        Render::GuiRenderState renderState;
        Render::GuiGraphics graphics(guiWidth, guiHeight, &g_guiAtlas, &renderState, &g_fontRenderer);
        g_hudRenderer.Render(graphics, inventory, deltaTime);

        // Chat: update timer and render messages + input field
        g_chatComponent.Update(deltaTime);
        g_chatComponent.Render(graphics, g_chatComponent.GetGameTime(), g_chatScreen.IsOpen());
        g_chatScreen.Render(graphics);

        // Chat bubbles above remote players (rendered in GUI space with text)
        if (Client::g_remotePlayerManager) {
            glm::mat4 vp = proj * view;
            for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                if (rp.chatBubbleTimer <= 0.0f || rp.chatBubbleText.empty()) continue;

                // Project player head position to screen
                glm::vec4 worldPos(rp.position.x, rp.position.y + 2.4f, rp.position.z, 1.0f);
                glm::vec4 clip = vp * worldPos;
                if (clip.w <= 0.0f) continue;

                // NDC to GUI-scaled coords
                float ndcX = clip.x / clip.w;
                float ndcY = clip.y / clip.w;
                float sx = (ndcX * 0.5f + 0.5f) * guiWidth;
                float sy = (1.0f - (ndcY * 0.5f + 0.5f)) * guiHeight;

                // Measure text and build bubble
                const std::string& text = rp.chatBubbleText;
                int textW = g_fontRenderer.GetStringWidth(text);
                int padding = 4;
                int bubbleW = textW + padding * 2;
                int bubbleH = Render::FontRenderer::LINE_HEIGHT + padding * 2;
                int bx = static_cast<int>(sx) - bubbleW / 2;
                int by = static_cast<int>(sy) - bubbleH - 6;

                // Black outline (2px border)
                graphics.Fill(bx - 2, by - 2, bx + bubbleW + 2, by + bubbleH + 2, 0xFF000000);
                // White fill
                graphics.Fill(bx, by, bx + bubbleW, by + bubbleH, 0xFFFFFFFF);

                // Triangle pointer (black outline + white fill)
                int tx = static_cast<int>(sx);
                int ty = by + bubbleH;
                graphics.Fill(tx - 4, ty, tx + 4, ty + 2, 0xFF000000);
                graphics.Fill(tx - 3, ty + 2, tx + 3, ty + 4, 0xFF000000);
                graphics.Fill(tx - 2, ty + 4, tx + 2, ty + 6, 0xFF000000);
                graphics.Fill(tx - 3, ty, tx + 3, ty + 2, 0xFFFFFFFF);
                graphics.Fill(tx - 2, ty + 2, tx + 2, ty + 4, 0xFFFFFFFF);

                // Text (black, centered in bubble)
                graphics.DrawString(text, bx + padding, by + padding, 0xFF000000, false);
            }
        }

        g_guiRenderer.Render(renderState, windowWidth, windowHeight,
                            framebufferWidth, framebufferHeight, guiScale, &g_fontRenderer);
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

    void HandlePlayerInput(Game::ClientPlayer& player, Game::ClientPlayerController& controller, Render::Camera& camera) {
        // Movement input
        glm::vec3 movementInput = camera.CalculateMovementInput();
        player.SetMovementInput(movementInput);

        // Action inputs
        player.SetJumpPressed(camera.IsJumpPressed());
        player.SetJumpHeld(camera.IsJumpPressed());
        player.SetSprintPressed(camera.IsSprintPressed());
        player.SetSneakPressed(camera.IsSneakPressed());

        // Block interaction
        static bool leftMouseWasDown = false;
        bool leftMouseDown = Input::IsMouseButtonDown(Input::Key::LeftMouse);
        if (leftMouseDown != leftMouseWasDown) {
            controller.OnLMB(leftMouseDown);
            leftMouseWasDown = leftMouseDown;
        }

        static bool rightMouseWasDown = false;
        bool rightMouseDown = Input::IsMouseButtonDown(Input::Key::RightMouse);
        if (rightMouseDown != rightMouseWasDown) {
            controller.OnRMB(rightMouseDown);
            rightMouseWasDown = rightMouseDown;
        }

        // Inventory selection
        if (Input::IsKeyPressed(Input::Key::Alpha1)) controller.OnHotbarChanged(0);
        if (Input::IsKeyPressed(Input::Key::Alpha2)) controller.OnHotbarChanged(1);
        if (Input::IsKeyPressed(Input::Key::Alpha3)) controller.OnHotbarChanged(2);
        if (Input::IsKeyPressed(Input::Key::Alpha4)) controller.OnHotbarChanged(3);
        if (Input::IsKeyPressed(Input::Key::Alpha5)) controller.OnHotbarChanged(4);
        if (Input::IsKeyPressed(Input::Key::Alpha6)) controller.OnHotbarChanged(5);
        if (Input::IsKeyPressed(Input::Key::Alpha7)) controller.OnHotbarChanged(6);
        if (Input::IsKeyPressed(Input::Key::Alpha8)) controller.OnHotbarChanged(7);
        if (Input::IsKeyPressed(Input::Key::Alpha9)) controller.OnHotbarChanged(8);

        // Pick block (P key) — set selected slot to the block being looked at
        if (Input::IsKeyPressed(Input::Key::P)) {
            if (player.lastBlockHit.has_value()) {
                Game::BlockID pickedBlock = player.lastBlockHit->blockId;
                if (pickedBlock != Game::BlockID::Air) {
                    int slot = player.inventory.GetSelectedSlot();
                    player.inventory.SetSlot(slot, pickedBlock, 64);
                    // Sync with server (sends slot + block type)
                    controller.OnHotbarChanged(slot);
                }
            }
        }

        // Mouse wheel for inventory scrolling
        auto [scrollX, scrollY] = Input::GetScrollOffset();
        if (scrollY > 0) {
            controller.OnHotbarChanged((player.GetSelectedSlot() - 1 + 9) % 9);
        } else if (scrollY < 0) {
            controller.OnHotbarChanged((player.GetSelectedSlot() + 1) % 9);
        }

        // Debug noclip toggle
        if (Input::IsKeyPressed(Input::Key::N)) {
            player.ToggleNoclip();
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

    // Fullscreen toggle state
    static bool s_isFullscreen = false;
    static int s_windowedX = 0, s_windowedY = 0;
    static int s_windowedWidth = Config::WindowWidth, s_windowedHeight = Config::WindowHeight;

    void ToggleFullscreen(GLFWwindow* window) {
        if (s_isFullscreen) {
            glfwSetWindowMonitor(window, nullptr,
                s_windowedX, s_windowedY,
                s_windowedWidth, s_windowedHeight, 0);
            s_isFullscreen = false;
            Log::Info("Switched to windowed mode (%dx%d)", s_windowedWidth, s_windowedHeight);
        } else {
            glfwGetWindowPos(window, &s_windowedX, &s_windowedY);
            glfwGetWindowSize(window, &s_windowedWidth, &s_windowedHeight);

            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(window, monitor,
                0, 0, mode->width, mode->height, mode->refreshRate);
            s_isFullscreen = true;
            Log::Info("Switched to fullscreen (%dx%d @ %dHz)", mode->width, mode->height, mode->refreshRate);
        }

        Platform::g_gameSettings.SetFullscreen(s_isFullscreen);
        Platform::g_gameSettings.Save();
    }

    void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam) {
        // Classify severity
        const char* severityStr = "UNKNOWN";
        switch (severity) {
            case GL_DEBUG_SEVERITY_HIGH:         severityStr = "HIGH"; break;
            case GL_DEBUG_SEVERITY_MEDIUM:       severityStr = "MEDIUM"; break;
            case GL_DEBUG_SEVERITY_LOW:          severityStr = "LOW"; break;
            case GL_DEBUG_SEVERITY_NOTIFICATION: severityStr = "NOTIFICATION"; break;
        }

        // Classify source
        const char* sourceStr = "UNKNOWN";
        switch (source) {
            case GL_DEBUG_SOURCE_API:             sourceStr = "API"; break;
            case GL_DEBUG_SOURCE_WINDOW_SYSTEM:   sourceStr = "WINDOW"; break;
            case GL_DEBUG_SOURCE_SHADER_COMPILER: sourceStr = "SHADER"; break;
            case GL_DEBUG_SOURCE_THIRD_PARTY:     sourceStr = "3RD_PARTY"; break;
            case GL_DEBUG_SOURCE_APPLICATION:     sourceStr = "APP"; break;
            case GL_DEBUG_SOURCE_OTHER:           sourceStr = "OTHER"; break;
        }

        // Route to appropriate log level
        if (severity == GL_DEBUG_SEVERITY_HIGH) {
            Log::Error("[GL %s/%s] %s", sourceStr, severityStr, message);
        } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
            Log::Warning("[GL %s/%s] %s", sourceStr, severityStr, message);
        } else {
            Log::Debug("[GL %s/%s] %s", sourceStr, severityStr, message);
        }
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

        // Initialize GUI rendering system
        std::string guiSpritesDir = GetAssetPath("assets/textures/gui/sprites");
        if (!g_guiAtlas.Initialize(guiSpritesDir)) {
            Log::Warning("Failed to initialize GUI atlas, continuing without HUD");
        }
        std::string fontPath = GetAssetPath("assets/textures/font/ascii.png");
        if (!g_fontRenderer.Initialize(fontPath)) {
            Log::Warning("Failed to initialize font renderer");
        }
        if (!g_guiRenderer.Initialize()) {
            Log::Warning("Failed to initialize GUI renderer");
        }

        // Set up chat message callback
        SetChatMessageCallback([](const std::string& msg) {
            g_chatComponent.AddMessage(msg);
        });
        SetChatBubbleCallback([](uint32_t senderId, const std::string& msg) {
            if (Client::g_remotePlayerManager) {
                Client::g_remotePlayerManager->SetChatBubble(senderId, msg);
            }
        });

        // Compile shaders
        Shader blockShader = InitializeShaders();

        Log::Info("✓ Game systems initialized");
        return true;
    }

    int Run(int argc, char** argv) {
        // Parse command-line arguments
        bool useVulkan = false;
        bool crashTest = false;
        bool isRemoteClient = false;
        std::string remoteServerAddress;
        uint16_t remoteServerPort = 25565;
        std::string playerName = "Player1";
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--vulkan") {
                useVulkan = true;
                Log::Info("Vulkan backend requested via --vulkan flag");
            }
            if (arg == "--crash-test") {
                crashTest = true;
            }
            if (arg == "--server" && i + 1 < argc) {
                isRemoteClient = true;
                std::string hostPort = argv[++i];
                // Parse host:port
                auto colonPos = hostPort.rfind(':');
                if (colonPos != std::string::npos) {
                    remoteServerAddress = hostPort.substr(0, colonPos);
                    remoteServerPort = static_cast<uint16_t>(std::stoi(hostPort.substr(colonPos + 1)));
                } else {
                    remoteServerAddress = hostPort;
                    // Use default port 25565
                }
                Log::Info("Remote server mode: connecting to %s:%d", remoteServerAddress.c_str(), remoteServerPort);
            }
            if (arg == "--name" && i + 1 < argc) {
                playerName = argv[++i];
                Log::Info("Player name set to: %s", playerName.c_str());
            }
        }

        // Initialize crash reporting (must be first — catches crashes during all other init)
        sentry_options_t *sentryOptions = sentry_options_new();
        sentry_options_set_dsn(sentryOptions, "https://685865d2f16184d804534ac7e262e818@o4511006654791680.ingest.us.sentry.io/4511006665539584");
        sentry_options_set_database_path(sentryOptions, ".sentry-native");
        sentry_options_set_release(sentryOptions, "myvoxelgame@" GAME_VERSION);
        sentry_options_set_debug(sentryOptions, 0);
#ifdef __APPLE__
        // On macOS, crashpad_handler is bundled next to the executable in the .app
        {
            std::string exeDir = std::string(argv[0]);
            exeDir = exeDir.substr(0, exeDir.find_last_of('/'));
            std::string handlerPath = exeDir + "/crashpad_handler";
            sentry_options_set_handler_path(sentryOptions, handlerPath.c_str());
        }
#endif
        int sentryResult = sentry_init(sentryOptions);
        if (sentryResult == 0) {
            Log::Info("Sentry crash reporting initialized");
            // Ensure sentry_close() runs on ALL exit paths (early returns, crashes, etc.)
            std::atexit([]() { sentry_close(); });
        } else {
            Log::Error("Sentry initialization failed (error %d)", sentryResult);
        }

        // Intentional crash for testing Sentry (run with --crash-test)
        if (crashTest) {
            Log::Info("Crash test requested — crashing in 3 seconds...");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            volatile int* p = nullptr;
            *p = 42;  // SIGSEGV
        }

        // Initialize systems
        Log::Info("Starting Voxel Engine");

#if defined(__APPLE__) && defined(HAS_VULKAN)
        // Point the Vulkan loader to the bundled MoltenVK ICD manifest.
        // This makes Vulkan work without any system-wide Vulkan/MoltenVK installation.
        if (useVulkan) {
            CFBundleRef mainBundle = CFBundleGetMainBundle();
            if (mainBundle) {
                CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
                if (resourcesURL) {
                    char resourcesPath[PATH_MAX];
                    if (CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8*)resourcesPath, PATH_MAX)) {
                        std::string icdPath = std::string(resourcesPath) + "/vulkan/icd.d/MoltenVK_icd.json";
                        setenv("VK_ICD_FILENAMES", icdPath.c_str(), 1);
                        setenv("VK_DRIVER_FILES", icdPath.c_str(), 1);
                        Log::Info("Set bundled MoltenVK ICD path: %s", icdPath.c_str());
                    }
                    CFRelease(resourcesURL);
                }
            }
        }
#endif

        // Initialize GLFW
        if (!glfwInit()) {
            Log::Error("Failed to initialize GLFW");
            return -1;
        }

        // Setup graphics API context based on backend choice
        if (useVulkan) {
#ifdef HAS_VULKAN
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan manages its own context
    #ifdef __APPLE__
            glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    #endif
            Log::Info("Window configured for Vulkan (no OpenGL context)");
#else
            Log::Error("Vulkan backend not available (compiled without HAS_VULKAN). Falling back to OpenGL.");
            useVulkan = false;
            // Fall through to OpenGL setup below
#endif
        }

        if (!useVulkan) {
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Config::OpenGLMajor);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Config::OpenGLMinor);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef __APPLE__
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
            glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    #endif
            glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
        }

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

        if (!useVulkan) {
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

            // Register OpenGL debug callback
        #ifndef NDEBUG
            {
                GLint contextFlags = 0;
                glGetIntegerv(GL_CONTEXT_FLAGS, &contextFlags);
                if (contextFlags & GL_CONTEXT_FLAG_DEBUG_BIT) {
                    glEnable(GL_DEBUG_OUTPUT);
                    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
                    glDebugMessageCallback(glDebugOutput, nullptr);
                    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
                    Log::Info("OpenGL debug output enabled");
                } else {
                    Log::Warning("OpenGL debug context not available");
                }
            }
        #endif

            // Setup OpenGL state
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
            glfwSwapInterval(Platform::g_gameSettings.GetVSync() ? 1 : 0); // VSync from settings
        } else {
            Log::Info("Vulkan mode: skipping OpenGL initialization");
        }

        // Initialize render backend abstraction
        {
            Render::BackendType backendType = useVulkan ? Render::BackendType::Vulkan : Render::BackendType::OpenGL;
            Render::g_renderBackend = Render::CreateRenderBackend(backendType);
            if (!Render::g_renderBackend) {
                Log::Error("Failed to create render backend");
                if (useVulkan) {
                    Log::Error("Vulkan backend creation failed. Try running without --vulkan");
                }
                glfwDestroyWindow(window);
                glfwTerminate();
                return -3;
            }
            if (!Render::g_renderBackend->Initialize(window)) {
                Log::Error("Failed to initialize render backend: %s", Render::g_renderBackend->GetName());
                glfwDestroyWindow(window);
                glfwTerminate();
                return -3;
            }
            Log::Info("Render backend initialized: %s", Render::g_renderBackend->GetName());
        }

        // Initialize game directory system (creates obeycraft folder and loads options.txt)
        if (!Platform::InitializeGameDirectorySystem()) {
            Log::Error("Failed to initialize game directory system");
            return -1;
        }

        // Initialize input
        Input::Init(window);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        // Apply fullscreen setting from saved preferences
        if (Platform::g_gameSettings.GetFullscreen()) {
            ToggleFullscreen(window);
        }

        // Initialize game systems BEFORE any chunk loading
        if (!InitializeGameSystems(window)) {
            Log::Error("Failure to init");
            return 1;
        }

        // Initialize remote player tracking and renderer
        Client::g_remotePlayerManager = std::make_unique<Client::RemotePlayerManager>();
        Render::PlayerRenderer playerRenderer;
        if (!playerRenderer.Initialize()) {
            Log::Warning("Failed to initialize player renderer, remote players won't be visible");
        }

        // === MINECRAFT-STYLE ARCHITECTURE INITIALIZATION ===
        Log::Info("Initializing Minecraft Java Edition Architecture...");

        // ClientBlockAccess for remote clients (physics/raycast backed by client chunk cache)
        std::unique_ptr<Client::ClientBlockAccess> clientBlockAccess;
        Game::World* world = nullptr;

        if (!isRemoteClient) {
            // 1. Initialize server-side systems (server creates and owns the world)
            Server::IntegratedServerConfig serverConfig;
            serverConfig.tickRate = 20;                     // 20 TPS like Minecraft
            serverConfig.enableAsyncChunkLoading = true;     // Async via ServerWorkerPool (non-blocking)

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
            world = Server::g_integratedServer->GetWorld();
            Game::g_world = world;

            // 3. Initialize worker pools with dynamic thread allocation
            Core::ThreadAllocation threadAlloc = Core::ThreadAllocator::GetOptimalAllocation();
            Threading::InitializeServerWorkerPool(threadAlloc.serverWorldWorkers);
            Threading::InitializeClientWorkerPool(threadAlloc.clientMeshWorkers);
            Log::Info("✓ Worker pools initialized - %s", threadAlloc.ToString().c_str());
        } else {
            // Remote client: no server, no server worker pool. Only client worker pool for mesh building.
            Log::Info("Remote client mode - skipping integrated server");
            Core::ThreadAllocation threadAlloc = Core::ThreadAllocator::GetOptimalAllocation();
            Threading::InitializeClientWorkerPool(threadAlloc.clientMeshWorkers);
            Log::Info("✓ Client worker pool initialized (%d threads)", threadAlloc.clientMeshWorkers);

            // Create ClientBlockAccess for physics and raycast
            clientBlockAccess = std::make_unique<Client::ClientBlockAccess>();
        }

        // 4. Initialize client-side systems (always needed)
        Client::InitializeClientChunkManager();
        Render::InitializeClientMeshManager(Client::g_clientChunkManager.get());
        Log::Info("✓ Client systems initialized (chunk manager, mesh manager)");

        // 5. Initialize player and controller
        Game::ClientPlayer player;
        Game::ClientPlayerController playerController;
        playerController.SetPlayer(&player);
        if (!isRemoteClient) {
            playerController.SetWorld(world);
        } else {
            // Remote client: no World for block placement (server handles it)
            playerController.SetWorld(nullptr);
        }

        // 6. Configure IntegratedServer with player (host only)
        if (!isRemoteClient && Server::g_integratedServer) {
            Server::g_integratedServer->SetPlayer(&player);
        }

        // 7. Initialize rendering systems (keeping existing ones that still work)
        if (!Render::InitializeChunkRenderer()) {
            Log::Error("Failed to initialize chunk renderer");
            return -7;
        }

        // 8. Initialize debug system
        Debug::DebugSystem::Initialize(window);

        // 9. Start the IntegratedServer thread (host only)
        if (!isRemoteClient) {
            Server::StartIntegratedServer();
            Log::Info("✓ IntegratedServer thread started (20 TPS)");
        }

        // Set up global block access for raycast system
        Game::IBlockAccess* blockAccessForPhysics = nullptr;
        if (!isRemoteClient) {
            blockAccessForPhysics = world;
            // Note: World::Initialize already calls SetGlobalBlockAccess(this)
        } else {
            blockAccessForPhysics = clientBlockAccess.get();
            Game::SetGlobalBlockAccess(clientBlockAccess.get());
        }
        
        // 10. Initialize Network I/O Service (dedicated I/O thread like Minecraft's Netty)
        Client::InitializeNetworkIOService();
        Log::Info("✓ Network I/O Service started (dedicated I/O thread)");
        
        // 11. Create NetworkClient and connect to server
        auto networkClient = std::make_unique<Client::NetworkClient>(Client::g_networkIOService->GetIOContext());
        Client::g_networkClient = networkClient.get();  // Set global pointer for legacy systems

        // Set player name for handshake
        networkClient->SetPlayerName(playerName);

        // Wire up player reference for server-authoritative hotbar sync
        if (auto handler = networkClient->GetPacketHandler()) {
            handler->SetPlayer(&player);
        }

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

        networkClient->SetOnDisconnected([window](const std::string& reason) {
            Log::Info("Disconnected from server: %s", reason.c_str());
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        });

        // Determine connection target
        std::string connectHost;
        uint16_t serverPort;
        if (isRemoteClient) {
            connectHost = remoteServerAddress;
            serverPort = remoteServerPort;
        } else {
            connectHost = "127.0.0.1";
            serverPort = Server::g_integratedServer->GetNetworkServer()->GetPort();
        }

        // Start async connection (all socket ops on I/O thread)
        Log::Info("Connecting to %s:%d...", connectHost.c_str(), serverPort);
        networkClient->ConnectAsync(connectHost, serverPort);

        // Wait for connection with timeout (using yield instead of sleep)
        auto startTime = std::chrono::steady_clock::now();
        int timeoutSeconds = isRemoteClient ? 10 : 2;
        while (!*connectionComplete) {
            if (std::chrono::steady_clock::now() - startTime > std::chrono::seconds(timeoutSeconds)) {
                Log::Error("Connection timeout after %d seconds", timeoutSeconds);
                return -8;
            }
            std::this_thread::yield();
        }

        if (!*connected) {
            Log::Error("Failed to connect to server");
            return -8;
        }

        Log::Info("✓ NetworkClient connected to %s:%d", connectHost.c_str(), serverPort);
        Log::Info("✓ Handshake automatically sent to server");
        
        // Wire up NetworkClient to PlayerController for packet sending
        playerController.SetNetworkClient(networkClient.get());
        Log::Info("✓ PlayerController connected to NetworkClient");
        
        Log::Info("🎮 Minecraft Java Edition Architecture fully initialized!");
        if (isRemoteClient) {
            Log::Info("   Remote client mode: connected to %s:%d", connectHost.c_str(), serverPort);
        } else {
            Log::Info("   Server Thread: 20 TPS | Client Thread: Unlocked FPS | I/O Thread: Async");
            Log::Info("   Client connected via TCP to localhost:%d", serverPort);
        }
        Log::Info("=== ENTERING CLIENT RENDER LOOP (UNLOCKED FPS) ===");

        // === MINECRAFT-STYLE MAIN LOOP ===
        // Matches Minecraft.java: processQueuedPackets() → tick() (20 TPS) → render() (uncapped)
        // Packets and game logic run at fixed 20 TPS. Rendering is decoupled at uncapped FPS.

        // Initialize camera for client thread
        Render::Camera camera;
        camera.position = glm::vec3(0.0f, 67.0f, 0.0f);
        camera.physicsControlled = true;

        // Network tracking
        uint32_t playerMoveSequence = 0;

        // Performance tracking
        Debug::PerformanceMetrics metrics;
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        // Client tick timing (20 TPS, matching Minecraft and server)
        static constexpr auto CLIENT_TICK_INTERVAL = std::chrono::milliseconds(50);
        static constexpr int MAX_TICKS_PER_FRAME = 10;
        auto nextClientTick = std::chrono::steady_clock::now();

        while (!glfwWindowShouldClose(window)) {
            frameStartTime = std::chrono::high_resolution_clock::now();

            // === PER-FRAME: Poll events and handle input (must be every frame for responsiveness) ===
            bool cursorEnabled;
            { PROFILE_ZONE_N("Input");
            PROFILE_TIMER_START(input);
            glfwPollEvents();
            Input::UpdateKeyStates();

            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            if (Input::IsKeyPressed(Input::Key::F11)) {
                ToggleFullscreen(window);
            }

            cursorEnabled = HandleCursorToggle(window, camera);

            // Chat system: open on T or /, route input when open
            if (g_chatScreen.IsOpen()) {
                // Route character input to chat
                while (Input::HasCharInput()) {
                    g_chatScreen.OnCharInput(Input::PopCharInput());
                }
                // Route key input (Enter, Escape, Backspace, arrows)
                if (Input::IsKeyPressed(Input::Key::Escape)) {
                    g_chatScreen.OnKeyDown(GLFW_KEY_ESCAPE);
                }
                // Check raw GLFW keys for Enter/Backspace (need glfwGetKey for repeat)
                static bool enterHeld = false, backspaceHeld = false;
                bool enterDown = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS;
                bool backspaceDown = glfwGetKey(window, GLFW_KEY_BACKSPACE) == GLFW_PRESS;
                if (enterDown && !enterHeld) g_chatScreen.OnKeyDown(GLFW_KEY_ENTER);
                if (backspaceDown && !backspaceHeld) g_chatScreen.OnKeyDown(GLFW_KEY_BACKSPACE);
                enterHeld = enterDown;
                backspaceHeld = backspaceDown;
                // Up/down for history
                static bool upHeld = false, downHeld = false;
                bool upDown = glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS;
                bool downDown = glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;
                if (upDown && !upHeld) g_chatScreen.OnKeyDown(GLFW_KEY_UP);
                if (downDown && !downHeld) g_chatScreen.OnKeyDown(GLFW_KEY_DOWN);
                upHeld = upDown;
                downHeld = downDown;

                g_chatScreen.Update(1.0f / 60.0f); // Approximate frame dt for cursor blink

                // Handle submitted message
                std::string submitted = g_chatScreen.ConsumeSubmittedMessage();
                if (!submitted.empty() && networkClient && networkClient->IsConnected()) {
                    auto conn = networkClient->GetConnection();
                    if (conn) {
                        conn->SendChatMessage(submitted);
                    }
                }

                // Drain char queue to prevent stale input
                // Skip player input — chat has focus
            } else {
                // Drain char queue when chat is closed (prevent buildup)
                while (Input::HasCharInput()) Input::PopCharInput();

                // Open chat on T or /
                if (Input::IsKeyPressed(Input::Key::T)) {
                    g_chatScreen.Open(false);
                } else if (Input::IsKeyPressed(Input::Key::Slash)) {
                    g_chatScreen.Open(true);
                }

                HandlePlayerInput(player, playerController, camera);
            }
            PROFILE_TIMER_END(input, metrics.inputHandlingTime);
            }

            // Frame counter for debugging
            static uint64_t frameCounter = 0;
            frameCounter++;

            // === CLIENT TICK (20 TPS, matches Minecraft.java runTick()) ===
            // Minecraft pattern: process ALL queued packets, then run game tick.
            // Multiple ticks per frame if behind (catch-up, capped at 10).
            {
            auto now = std::chrono::steady_clock::now();
            int ticksThisFrame = 0;
            while (now >= nextClientTick && ticksThisFrame < MAX_TICKS_PER_FRAME) {
                PROFILE_ZONE_N("ClientTick");

                // 1. Drain ALL queued packets (Minecraft: packetProcessor.processQueuedPackets())
                { PROFILE_ZONE_N("Network");
                PROFILE_TIMER_START(network);
                if (networkClient) {
                    networkClient->DrainIncomingPackets();
                }
                PROFILE_TIMER_END(network, metrics.networkProcessingTime);
                }

                // 2. Schedule mesh builds (fixed-rate, not per-frame)
                { PROFILE_ZONE_N("MeshSchedule");
                PROFILE_TIMER_START(meshsched);
                glm::vec3 tickPlayerPos = player.physics.position;
                Render::ScheduleClientMeshBuilds(tickPlayerPos);
                PROFILE_TIMER_END(meshsched, metrics.meshSchedulingTime);
                }

                // 3. Interpolate remote player positions (Minecraft's InterpolationHandler)
                if (Client::g_remotePlayerManager) {
                    Client::g_remotePlayerManager->Tick();
                }

                // 4. Send player position to server (one packet per tick = 20 Hz)
                if (networkClient->IsConnected()) {
                    glm::vec3 playerPos = player.physics.position;
                    Network::PlayerMoveC2SPacket movePacket;
                    movePacket.position = playerPos;
                    movePacket.rotation = glm::vec2(camera.yaw, camera.pitch);
                    movePacket.onGround = player.physics.isOnGround;
                    movePacket.isCrouching = Input::IsKeyDown(Input::Key::LeftShift);
                    movePacket.sequenceNumber = ++playerMoveSequence;
                    movePacket.timestamp = std::chrono::steady_clock::now();
                    networkClient->GetConnection()->SendPlayerMove(movePacket);
                }

                nextClientTick += CLIENT_TICK_INTERVAL;
                ticksThisFrame++;
            }
            }

            // === PER-FRAME: Game logic (variable dt for smooth rendering) ===
            float dt;
            { PROFILE_ZONE_N("GameLogic");
            PROFILE_TIMER_START(gamelogic);
            Time::Tick();
            dt = static_cast<float>(Time::Delta());
            metrics.AddFrameTimeSample(dt * 1000.0f);

            player.UpdatePhysics(dt, blockAccessForPhysics);
            camera.position = player.GetEyePosition();
            camera.Update(dt);
            player.UpdateRaycast(camera);
            playerController.Tick(dt);
            player.UpdateVisual(dt);
            player.UpdateStatistics(dt);
            PROFILE_TIMER_END(gamelogic, metrics.gameLogicTime);
            }

            // === PER-FRAME: Set player position for mesh prioritization ===
            glm::vec3 playerPos = player.physics.position;
            Render::SetClientMeshPlayerPosition(playerPos);

            // 7. Perform GPU uploads
            { PROFILE_ZONE_N("GPUUpload");
            PROFILE_TIMER_START(gpuupload);
            Render::PerformClientGPUUploads();
            PROFILE_TIMER_END(gpuupload, metrics.gpuUploadTime);
            }

            // Capture per-frame mesh stats BEFORE they get reset
            int lastFrameMeshUploads = 0;
            size_t lastFrameMeshPending = 0;
            size_t lastFrameMeshActive = 0;
            if (Render::g_clientMeshManager) {
                const auto& meshStats = Render::g_clientMeshManager->GetStats();
                lastFrameMeshUploads = meshStats.meshUploadsThisFrame;
            }
            if (Threading::g_clientWorkerPool) {
                lastFrameMeshPending = Threading::g_clientWorkerPool->GetPendingJobCount();
                lastFrameMeshActive = Threading::g_clientWorkerPool->GetActiveJobCount();
            }

            // 8. Update texture animations
            { PROFILE_ZONE_N("TexAnimation");
            PROFILE_TIMER_START(texanim);
            if (Render::g_textureAnimator) {
                Render::g_textureAnimator->UpdateAnimations(dt);
            }
            PROFILE_TIMER_END(texanim, metrics.textureAnimationTime);
            }

            // 9. Main rendering phase (frustum culling + GPU draw calls)
            int width, height;
            Frustum frustum;
            { PROFILE_ZONE_N("Render");
            PROFILE_TIMER_START(render);

            // Begin render backend frame (acquires swapchain image for Vulkan)
            if (Render::g_renderBackend) {
                Render::g_renderBackend->BeginFrame();
            }

            Debug::DebugSystem::BeginFrame();

            // Reset per-frame render stats
            metrics.ResetFrameMetrics();

            // Clear framebuffer via render backend
            if (Render::g_renderBackend) {
                // Sky color RGB(120, 167, 255)
                Render::g_renderBackend->SetClearColor(120.0f/255.0f, 167.0f/255.0f, 1.0f, 1.0f);
                Render::g_renderBackend->Clear(true, true);
            } else {
                glClearColor(120.0f/255.0f, 167.0f/255.0f, 1.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }

            // Calculate view-projection matrices
            glfwGetFramebufferSize(window, &width, &height);
            if (Render::g_renderBackend) {
                Render::g_renderBackend->SetViewport(0, 0, width, height);
            }
            float aspect = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);

            int effectiveRenderDist = Platform::g_gameSettings.GetRenderDistance();
            if (Client::g_networkClient && Client::g_networkClient->GetServerViewDistance() > 0) {
                effectiveRenderDist = std::min(effectiveRenderDist, Client::g_networkClient->GetServerViewDistance());
            }
            float farPlane = static_cast<float>(effectiveRenderDist) * 16.0f * 4.0f;
            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.05f, farPlane);
            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 viewProj = proj * view;
            frustum = Frustum::FromMatrix(viewProj);

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

                // GPU timer query results (1-frame latency from GL_TIME_ELAPSED)
                metrics.gpuOpaqueTimeMs = renderStats->gpuOpaqueTimeMs;
                metrics.gpuCutoutTimeMs = renderStats->gpuCutoutTimeMs;
                metrics.gpuTranslucentTimeMs = renderStats->gpuTranslucentTimeMs;
                metrics.gpuTotalTimeMs = renderStats->gpuTotalTimeMs;

                // Occlusion culling stats
                metrics.occlusionVisited = renderStats->sectionsAvailable;
                metrics.occlusionOccluded = renderStats->sectionsSkipped;
            }

            // Render remote players (stick figures) before UI overlays
            if (Client::g_remotePlayerManager) {
                playerRenderer.Render(proj, view, camera.position, *Client::g_remotePlayerManager);
                Client::g_remotePlayerManager->UpdateBubbles(dt);
            }

            // Render UI overlay elements
            RenderBlockHighlight(player, proj, view);
            RenderHUD(window, player.inventory, dt, proj, view);
            RenderCrosshair(window);
            PROFILE_TIMER_END(render, metrics.renderTime);
            }

            // 10. Debug UI with new architecture statistics
            { PROFILE_ZONE_N("DebugUI");
            PROFILE_TIMER_START(debugui);
            int windowWidth, windowHeight;
            glfwGetWindowSize(window, &windowWidth, &windowHeight);

            // Snapshot cross-thread metrics for debug panels
            {
                Debug::ServerMetricsSnapshot srvSnap;
                if (Server::g_integratedServer) {
                    srvSnap.serverRunning = Server::g_integratedServer->IsRunning();
                    if (auto* ns = Server::g_integratedServer->GetNetworkServer())
                        srvSnap.serverPort = ns->GetPort();
                    if (auto* w = Server::g_integratedServer->GetWorld())
                        srvSnap.worldSeed = w->GetGenerationSeed();
                    const auto& ss = Server::g_integratedServer->GetStats();
                    srvSnap.ticksProcessed = ss.ticksProcessed.load(std::memory_order_relaxed);
                    srvSnap.chunksLoaded = ss.chunksLoaded.load(std::memory_order_relaxed);
                    srvSnap.chunksSent = ss.chunksSent.load(std::memory_order_relaxed);
                    srvSnap.blockChangesProcessed = ss.blockChangesProcessed.load(std::memory_order_relaxed);
                    srvSnap.packetsReceived = ss.packetsReceived.load(std::memory_order_relaxed);
                    srvSnap.packetsSent = ss.packetsSent.load(std::memory_order_relaxed);
                    srvSnap.averageTickTime = ss.averageTickTime.load(std::memory_order_relaxed);
                    srvSnap.averageTPS = ss.averageTPS.load(std::memory_order_relaxed);
                }
                if (Threading::g_serverWorkerPool) {
                    srvSnap.serverWorkerCount = Threading::g_serverWorkerPool->GetWorkerCount();
                    srvSnap.serverPendingJobs = Threading::g_serverWorkerPool->GetPendingJobCount();
                    srvSnap.serverActiveJobs = Threading::g_serverWorkerPool->GetActiveJobCount();
                    const auto& sw = Threading::g_serverWorkerPool->GetStats();
                    srvSnap.serverChunksGenerated = sw.chunksGenerated.load(std::memory_order_relaxed);
                    srvSnap.serverChunksLoaded = sw.chunksLoaded.load(std::memory_order_relaxed);
                    srvSnap.serverChunksSaved = sw.chunksSaved.load(std::memory_order_relaxed);
                    srvSnap.serverJobsSubmitted = sw.jobsSubmitted.load(std::memory_order_relaxed);
                    srvSnap.serverJobsCompleted = sw.jobsCompleted.load(std::memory_order_relaxed);
                    srvSnap.serverJobsCancelled = sw.jobsCancelled.load(std::memory_order_relaxed);
                    srvSnap.serverJobsFailed = sw.jobsFailed.load(std::memory_order_relaxed);
                }

                // Chunk loading metrics (still on IntegratedServer)
                if (Server::g_integratedServer) {
                    srvSnap.chunksPendingLoad = Server::g_integratedServer->GetPendingChunkLoadCount();
                }

                // ChunkProvider loaded count
                auto* srvWorld = Server::g_integratedServer ? Server::g_integratedServer->GetWorld() : nullptr;
                if (srvWorld && srvWorld->GetChunkProvider()) {
                    srvSnap.chunkProviderLoaded = srvWorld->GetChunkProvider()->GetLoadedChunkCount();
                }

                // Player session metrics (chunk sender now lives on session)
                auto session = Server::g_integratedServer ? Server::g_integratedServer->GetPlayerSession() : nullptr;
                if (session) {
                    auto sessionStats = session->GetStats();
                    srvSnap.sessionWatchSetSize = sessionStats.chunksInWatch;
                    srvSnap.sessionSentChunks = sessionStats.chunksSent;
                    srvSnap.sessionViewDistance = session->GetViewDistance();
                    srvSnap.chunkSenderPending = session->GetPendingChunksToSendCount();
                    srvSnap.chunkSendRate = session->GetDesiredChunksPerTick();
                    srvSnap.chunkSenderUnacked = session->GetUnackedBatches();
                }

                Debug::DebugSystem::SetServerSnapshot(srvSnap);

                Debug::NetworkMetricsSnapshot netSnap;
                if (networkClient) {
                    netSnap.connected = networkClient->IsConnected();
                    auto conn = networkClient->GetConnection();
                    if (conn) {
                        const auto& cs = conn->GetStats();
                        netSnap.bytesSent = cs.bytesSent.load(std::memory_order_relaxed);
                        netSnap.bytesReceived = cs.bytesReceived.load(std::memory_order_relaxed);
                        netSnap.packetsSent = cs.packetsSent.load(std::memory_order_relaxed);
                        netSnap.packetsReceived = cs.packetsReceived.load(std::memory_order_relaxed);
                        netSnap.incomingQueueSize = conn->GetIncomingQueueSize();
                        netSnap.droppedPacketCount = conn->GetDroppedPacketCount();
                        auto elapsed = std::chrono::steady_clock::now() - cs.connectedTime;
                        netSnap.connectionUptimeSec = std::chrono::duration<float>(elapsed).count();
                    }
                }
                Debug::DebugSystem::SetNetworkSnapshot(netSnap);

                // Chunk Pipeline snapshot
                Debug::ChunkPipelineSnapshot pipeSnap;

                // View Distance
                pipeSnap.viewDistance = Platform::g_gameSettings.GetRenderDistance();
                if (Client::g_networkClient)
                    pipeSnap.serverViewDistance = Client::g_networkClient->GetServerViewDistance();

                // Session data
                auto pipeSession = Server::g_integratedServer ? Server::g_integratedServer->GetPlayerSession() : nullptr;
                if (pipeSession) {
                    pipeSnap.watchSetSize = pipeSession->GetStats().chunksInWatch;
                    pipeSnap.sessionPendingLoads = pipeSession->GetPendingChunkLoadsCount();
                    pipeSnap.readyToSend = pipeSession->GetPendingChunksToSendCount();
                    pipeSnap.sentToClient = pipeSession->GetSentChunkCount();
                    pipeSnap.sendRate = pipeSession->GetDesiredChunksPerTick();
                    pipeSnap.batchQuota = pipeSession->GetBatchQuota();
                    pipeSnap.unackedBatches = pipeSession->GetUnackedBatches();
                    pipeSnap.maxUnackedBatches = pipeSession->GetMaxUnackedBatches();
                    pipeSnap.viewDistance = pipeSession->GetViewDistance();
                }

                // Server worker pool
                if (Threading::g_serverWorkerPool) {
                    pipeSnap.workerThreads = Threading::g_serverWorkerPool->GetWorkerCount();
                    pipeSnap.workerPendingJobs = Threading::g_serverWorkerPool->GetPendingJobCount();
                    pipeSnap.workerActiveJobs = Threading::g_serverWorkerPool->GetActiveJobCount();
                    const auto& swStats = Threading::g_serverWorkerPool->GetStats();
                    pipeSnap.chunksGenerated = swStats.chunksGenerated.load(std::memory_order_relaxed);
                    pipeSnap.chunksLoadedFromDisk = swStats.chunksLoaded.load(std::memory_order_relaxed);
                    pipeSnap.jobsFailed = swStats.jobsFailed.load(std::memory_order_relaxed);
                }
                if (Server::g_integratedServer)
                    pipeSnap.serverPendingLoads = Server::g_integratedServer->GetPendingChunkLoadCount();

                // Provider cache
                auto* pipeWorld = Server::g_integratedServer ? Server::g_integratedServer->GetWorld() : nullptr;
                if (pipeWorld && pipeWorld->GetChunkProvider()) {
                    pipeSnap.providerLoaded = pipeWorld->GetChunkProvider()->GetLoadedChunkCount();
                    auto cacheStats = pipeWorld->GetChunkProvider()->GetCacheStats();
                    pipeSnap.providerMaxSize = cacheStats.maxSize;
                    pipeSnap.providerEvictions = cacheStats.totalEvictions;
                }

                // Client receive
                if (networkClient) {
                    if (auto handler = networkClient->GetPacketHandler()) {
                        auto hs = handler->getStats();
                        pipeSnap.clientChunksReceived = hs.chunksReceived;
                        pipeSnap.clientChunksUnloaded = hs.chunksUnloaded;
                        pipeSnap.clientDesiredRate = handler->GetDesiredChunksPerTick();
                        pipeSnap.clientAvgNanosPerChunk = handler->GetAvgNanosPerChunk();
                    }
                }

                // Client mesh
                if (Client::g_clientChunkManager)
                    pipeSnap.clientChunkCount = Client::g_clientChunkManager->GetLoadedChunkCount();
                if (Render::g_clientMeshManager) {
                    const auto& meshStats = Render::g_clientMeshManager->GetStats();
                    pipeSnap.meshBuildsCompleted = meshStats.meshBuildsCompleted.load(std::memory_order_relaxed);
                    pipeSnap.gpuActiveSections = Render::g_clientMeshManager->GetActiveSectionCount();
                }
                // Use values captured BEFORE reset (see step 7 above)
                pipeSnap.meshPendingJobs = lastFrameMeshPending;
                pipeSnap.meshActiveJobs = lastFrameMeshActive;
                pipeSnap.meshUploadsThisFrame = lastFrameMeshUploads;

                // Rendering (from ChunkRenderer stats and metrics)
                if (auto* renderStats = Render::GetChunkRendererStats()) {
                    pipeSnap.sectionsRendered = renderStats->sectionsRendered;
                    pipeSnap.sectionsCulled = renderStats->sectionsSkipped;
                    pipeSnap.totalDrawCalls = renderStats->totalDrawCalls;
                    pipeSnap.renderTimeMs = renderStats->renderTimeMs;
                }

                Debug::DebugSystem::SetChunkPipelineSnapshot(pipeSnap);
            }

            Debug::DebugSystem::RenderDebugUI(
                camera, frustum, player, playerController, metrics, cursorEnabled,
                windowWidth, windowHeight, width, height
            );

            Debug::DebugSystem::EndFrame();
            PROFILE_TIMER_END(debugui, metrics.debugUITime);
            }

            // Handle render distance change from debug UI
            if (Debug::DebugSystem::ConsumeRenderDistanceChanged()) {
                int newDist = Platform::g_gameSettings.GetRenderDistance();
                Log::Info("Render distance changed to %d", newDist);

                // Resend client settings to server (Minecraft-style broadcastOptions)
                if (networkClient) {
                    auto conn = networkClient->GetConnection();
                    if (conn) {
                        conn->SendClientSettings(
                            newDist,
                            Platform::g_gameSettings.GetVSync(),
                            Platform::g_gameSettings.GetMouseSensitivity()
                        );
                    }
                }

                // Persist to disk
                Platform::g_gameSettings.Save();
            }

            // 11. Swap buffers (includes VSync wait)
            { PROFILE_ZONE_N("VSync");
            PROFILE_TIMER_START(vsync);
            if (Render::g_renderBackend) {
                Render::g_renderBackend->EndFrame(window);
            } else {
                glfwSwapBuffers(window);
            }
            PROFILE_TIMER_END(vsync, metrics.vsyncWaitTime);
            }
            
            Input::ResetMouseDelta();
            Input::ResetScrollOffset();

            // Query thermal state (macOS only, ~once per second to avoid overhead)
#ifdef __APPLE__
            static int thermalPollCounter = 0;
            static bool thermalFirstLog = true;
            if (++thermalPollCounter >= 60) {
                thermalPollCounter = 0;
                id processInfo = ((id(*)(id, SEL))objc_msgSend)(
                    (id)objc_getClass("NSProcessInfo"), sel_registerName("processInfo"));
                if (processInfo) {
                    int prevState = metrics.thermalState;
                    metrics.thermalState = static_cast<int>(
                        ((long(*)(id, SEL))objc_msgSend)(processInfo, sel_registerName("thermalState")));
                    if (thermalFirstLog || metrics.thermalState != prevState) {
                        const char* names[] = {"Nominal", "Fair", "Serious", "Critical"};
                        Log::Info("Thermal state: %s (%d)", names[std::clamp(metrics.thermalState, 0, 3)], metrics.thermalState);
                        thermalFirstLog = false;
                    }
                }
            }
#endif

            // Calculate total frame time and unaccounted time
            auto frameEndTime = std::chrono::high_resolution_clock::now();
            metrics.frameTime = std::chrono::duration<float, std::milli>(frameEndTime - frameStartTime).count();
            
            // Calculate unaccounted time (operations we didn't explicitly measure)
            float totalMeasured = metrics.networkProcessingTime + metrics.meshResultProcessingTime + 
                                 metrics.inputHandlingTime + metrics.gameLogicTime + 
                                 metrics.meshSchedulingTime + metrics.gpuUploadTime + 
                                 metrics.textureAnimationTime + metrics.renderTime + 
                                 metrics.debugUITime + metrics.vsyncWaitTime;
            metrics.otherTime = std::max(0.0f, metrics.frameTime - totalMeasured);

            // Spike detection: record frames that exceed 2x the target budget
            if (metrics.frameTime > metrics.targetFrameTimeMs * 2.0f) {
                Debug::PerformanceMetrics::FrameSpike spike;
                spike.totalMs = metrics.frameTime;
                spike.renderMs = metrics.renderTime;
                spike.meshSchedMs = metrics.meshSchedulingTime;
                spike.gpuUploadMs = metrics.gpuUploadTime;
                spike.gpuTimeMs = metrics.gpuTotalTimeMs;
                if (auto* rs = Render::GetChunkRendererStats()) {
                    spike.drawCalls = rs->totalDrawCalls;
                    spike.sectionsRendered = rs->sectionsRendered;
                }
                spike.secondsAgo = 0.0f;
                metrics.RecordSpike(spike);
            }

            // Age existing spikes
            float frameSec = metrics.frameTime / 1000.0f;
            for (int i = 0; i < metrics.spikeCount; i++) {
                metrics.recentSpikes[i].secondsAgo += frameSec;
            }

            PROFILE_FRAME_MARK;
        }

        // === MINECRAFT-STYLE SHUTDOWN SEQUENCE ===
        Log::Info("Shutting down...");

        // Clear global block access for raycast if remote client
        if (isRemoteClient) {
            Game::SetGlobalBlockAccess(nullptr);
        }

        // 1. Disconnect NetworkClient
        Log::Info("Disconnecting NetworkClient...");
        networkClient->Disconnect();
        networkClient.reset();
        Log::Info("✓ NetworkClient disconnected");

        // 2. Stop Network I/O Service (dedicated I/O thread)
        Log::Info("Stopping Network I/O Service...");
        Client::ShutdownNetworkIOService();
        Log::Info("✓ Network I/O Service stopped");

        // 3. Stop IntegratedServer thread (host only)
        if (!isRemoteClient) {
            Log::Info("Stopping IntegratedServer thread...");
            Server::StopIntegratedServer();
            Log::Info("✓ IntegratedServer stopped");
        }

        // 4. Stop worker pools (stops background threads)
        Log::Info("Stopping worker thread pools...");
        if (!isRemoteClient) {
            Threading::ShutdownServerWorkerPool();
        }
        Threading::ShutdownClientWorkerPool();
        Log::Info("✓ Worker pools stopped");

        // 5. Note: Chunks are now saved by IntegratedServer during its shutdown

        // 6. Shutdown client systems
        Log::Info("Shutting down client systems...");
        Render::ShutdownClientMeshManager();
        Client::ShutdownClientChunkManager();
        Log::Info("✓ Client systems shutdown");

        // 7. Shutdown server systems (host only)
        if (!isRemoteClient) {
            Log::Info("Shutting down server systems...");
            Server::ShutdownIntegratedServer();
            Log::Info("✓ Server systems shutdown");
        }

        // Clean up ClientBlockAccess (remote client only)
        clientBlockAccess.reset();
        
        // 8. Shutdown rendering systems
        Render::ShutdownChunkRenderer();

        // 8a. Destroy resources that depend on the render backend BEFORE destroying it
        playerRenderer.Shutdown();
        Client::g_remotePlayerManager.reset();
        g_hudRenderer = Render::HudRenderer();
        g_guiRenderer.Shutdown();
        g_fontRenderer.Shutdown();
        g_guiAtlas.Shutdown();
        Render::g_crosshair.Shutdown();
        Render::g_blockHighlight.Shutdown();
        if (Render::g_atlasBuilder) {
            Render::g_atlasBuilder.reset();
        }
        if (Render::g_textureAnimator) {
            Render::g_textureAnimator.reset();
        }

        // 8b. Cleanup debug system (before render backend, since ImGui shutdown needs the backend)
        Debug::DebugSystem::Shutdown();

        // 8c. Shutdown render backend (now safe — all dependent resources are gone)
        if (Render::g_renderBackend) {
            Render::g_renderBackend->Shutdown();
            Render::g_renderBackend.reset();
            Log::Info("Render backend shutdown");
        }

        // 9. Clear world reference (world is shutdown by IntegratedServer)
        Game::g_world = nullptr;  // Clear global reference
        
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

        // Clean up remaining OpenGL resources
        Log::Info("Cleaning up rendering resources...");
        try {
            // Clear any remaining OpenGL errors (only if GL context exists)
            if (!useVulkan && glfwGetCurrentContext() == window) {
                while (glGetError() != GL_NO_ERROR) {}
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