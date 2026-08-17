// File: src/platform/PlatformMain.cpp
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "client/input/Input.hpp"
#include "client/input/KeyMapping.hpp"
#include "common/core/Log.hpp"
#include <sentry.h>
#include "common/core/Config.hpp"
#include "common/core/Features.hpp"
#include "common/core/ThreadAllocator.hpp"
#include "common/core/ThreadPriority.hpp"
#include "client/renderer/debug/DebugSystem.hpp"
// Include game headers
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockModel.hpp"
#include "common/world/block/BlockStateModels.hpp"
#include "common/world/biome/Biomes.hpp"
#include "client/input/PlayerController.hpp"
#include "client/entity/Player.hpp"
#include "common/entity/GeneratedItemList.hpp"   // for Game::Items::Compass etc.
#include "client/renderer/gui/InventoryScreen.hpp"
#include "client/renderer/gui/items/ChestItemRenderer.hpp"
#include "client/renderer/gui/items/BedItemRenderer.hpp"
#include "client/renderer/gui/items/ShulkerBoxItemRenderer.hpp"
#include "client/renderer/gui/items/BannerItemRenderer.hpp"
#include "client/renderer/gui/items/HeadItemRenderer.hpp"
#include "client/renderer/gui/items/ShieldItemRenderer.hpp"
#include "common/physics/RayCast.hpp"
#include "common/physics/Physics.hpp"

// Include rendering headers
#include "client/renderer/core/Camera.hpp"
#include "client/renderer/core/Frustum.hpp"
#include "client/renderer/shader/Shader.hpp"
#include "client/renderer/mesh/BlockHighlight.hpp"
#include "client/renderer/mesh/BlockBreakOverlay.hpp"
#include "client/renderer/blockentity/BlockEntityRenderDispatcher.hpp"
#include "client/renderer/blockentity/BlockEntityRenderers.hpp"
#include "client/renderer/debug/Crosshair.hpp"
#if ENABLE_PORTAL_GUN
#include "client/renderer/portal/PortalRenderer.hpp"
#include "client/renderer/portal/PortalParticleSystem.hpp"
#include "client/renderer/viewmodel/PortalGunViewmodel.hpp"
#include "client/renderer/viewmodel/HeldItemRenderer.hpp"
#include "client/renderer/portal/PortalCrosshair.hpp"
#include "common/entity/Item.hpp"
#include "common/world/crafting/RecipeManager.hpp"
#include "common/world/loot/LootTables.hpp"
#include "client/portal/ClientPortalManager.hpp"
#endif
#include "client/renderer/gui/GuiAtlas.hpp"
#include "client/renderer/gui/GuiRenderState.hpp"
#include "client/renderer/gui/GuiRenderer.hpp"
#include "client/renderer/gui/GuiGraphics.hpp"
#include "client/renderer/gui/FontRenderer.hpp"
#include "client/renderer/gui/HudRenderer.hpp"
#include "common/entity/GeneratedItemAttributes.hpp"
#include "client/renderer/gui/ChatComponent.hpp"
#include <cctype>
#include "common/network/packets/game/ChatMessageS2CPacket.hpp"
#include "client/renderer/gui/ChatScreen.hpp"
#include "client/renderer/gui/screens/Screen.hpp"
#include "client/renderer/gui/screens/TitleScreen.hpp"
#include "client/renderer/gui/screens/PauseScreen.hpp"
#include "client/renderer/gui/screens/DeathScreen.hpp"
#include "client/renderer/gui/screens/PanoramaRenderer.hpp"
#include "client/renderer/gui/screens/WorldSelectScreens.hpp"
#include "client/renderer/environment/EnvironmentState.hpp"
#include "client/renderer/environment/SkyRenderer.hpp"
#include "client/renderer/environment/CloudRenderer.hpp"
#include "client/renderer/gui/screens/OptionsScreens.hpp"
#include "client/network/FriendsClient.hpp"
#include "client/network/UPnPPortMapper.hpp"
#include "common/core/FriendsServiceConfig.hpp"
#include <algorithm> // std::clamp (FOV modifier)
#include <cstdlib>   // getenv (temp autoplay diagnostic)
#include <functional>
#include <sstream>
#include <unordered_set>


// Declared in ClientConnection.cpp
extern void SetChatMessageCallback(std::function<void(const Network::ChatMessageS2CPacket&)> callback);
extern void SetChatBubbleCallback(std::function<void(uint32_t, const std::string&)> callback);
extern void SetTimeUpdateCallback(std::function<void(uint64_t, uint64_t, bool)> callback);
extern void SetTeleportCallback(std::function<void(double, double, double, float, float,
                                                    double, double, double)> callback);
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
#include "client/renderer/mesh/MeshUploadPermits.hpp"

// Include new Minecraft-style architecture
#include "client/network/NetworkClient.hpp"
#include "server/session/PlayerSession.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkIOService.hpp"
#include "server/IntegratedServer.hpp"
#include "server/network/NetworkServer.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "client/world/LevelLoadTracker.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "server/world/ServerWorkerPool.hpp"
#include "client/world/ClientWorkerPool.hpp"

// Multiplayer player visibility
#include "client/entity/RemotePlayerManager.hpp"
#include "client/renderer/entity/PlayerRenderer.hpp"
#include "client/entity/ItemEntityManager.hpp"
#include "client/renderer/entity/ItemEntityRenderer.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/ClientTickRateManager.hpp"
#include "server/entity/MobManager.hpp"
#include "common/world/spawn/NaturalSpawner.hpp"   // kMagicNumber
#include "client/renderer/entity/MobRenderer.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

// Pointing-hand cursor for clickable chat components (MC swaps the cursor over
// a click event). Kept at file scope with an explicit reset so closing chat
// while hovering a link can't leave the hand cursor stuck on.
static void SetChatPointerCursor(GLFWwindow* window, bool wantHand) {
    static GLFWcursor* handCursor = glfwCreateStandardCursor(GLFW_POINTING_HAND_CURSOR);
    static bool active = false;
    if (wantHand == active) return;
    glfwSetCursor(window, wantHand ? handCursor : nullptr);
    active = wantHand;
}

#include <chrono>
#include <filesystem>
#include <memory>
#include <atomic>
#include <optional>
#include <thread>   // frame limiter sleep_until

#include "platform/GameDirectory.hpp"
#include "platform/CrashHandler.hpp"
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

// Shared "E held" state for cross-branch edge detection (game branch opens inventory,
// inventory branch closes inventory — both must observe the same held-state to avoid
// double-firing on the frame the inventory opens). The local `extern bool s_eKeyHeld;`
// declarations inside Run() resolve to this via the enclosing namespace.
bool s_eKeyHeld = false;

// Same cross-branch pattern for ESC: the game branch opens the pause menu,
// the pause branch closes it — one shared held-state so the opening press
// doesn't immediately re-edge in the other branch and close the menu.
bool s_escKeyHeld = false;

// ImGui/debug-system init happens once per process (not per session of the
// outer title↔world loop) and must be visible to both the session init and
// the title-screen quit path so shutdown only runs when init actually did.
static bool s_debugSystemInitialized = false;

// Router port mapping for hosted worlds (UPnP). Created on the first hosted
// session and kept for the process: the mapping is the same port every time,
// so re-mapping per session would be pointless churn. Removed once at exit.
static std::unique_ptr<Client::UPnPPortMapper> g_portMapper;


    std::string GetAssetPath(const std::string& relativePath) {
#ifdef __APPLE__
        // On macOS, check if we're running from a bundle
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        if (mainBundle) {
            // Get the Resources directory from the bundle
            CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
            if (resourcesURL) {
                char path[PATH_MAX];
                bool gotPath = CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8*)path, PATH_MAX);
                CFRelease(resourcesURL);  // single release; CF double-release is fatal (SIGTRAP)
                if (gotPath) {
                    std::string fullPath = std::string(path) + "/" + relativePath;
                    if (std::filesystem::exists(fullPath)) {
                        return fullPath;
                    }
                    Log::Debug("Asset not found in bundle Resources: %s", fullPath.c_str());
                }
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

    void RenderBlockHighlight(const Game::ClientPlayer& player, const glm::mat4& proj,
                              const glm::mat4& view, bool entityPicked) {
        // MC LevelRenderer.renderHitOutline only runs for
        // `hitResult.getType() == BLOCK`. With a mob under the crosshair the
        // hit result IS the entity, so no outline is drawn — which is also the
        // player's only cue that the click will hit the mob and not the wall.
        if (entityPicked) return;

        const auto& hit = player.lastBlockHit;
        if (Render::BlockHighlight::IsValidHighlight(hit)) {
            // Use the block's actual model-shape bounds so partial blocks (leaf
            // litter, slabs, fences, …) outline their real geometry instead of
            // the enclosing full cube.
            // World-aware so a paired chest outlines the half that is actually
            // there — otherwise the selection box stops a pixel short of the
            // seam that the raycast now accepts.
            const auto shape = Client::g_clientBlockAccess
                ? Game::BlockRegistry::GetBlockShapeAt(*Client::g_clientBlockAccess,
                                                       hit->blockPos, hit->blockId,
                                                       hit->stateIndex)
                : Game::BlockRegistry::GetBlockShape(hit->blockId, hit->stateIndex);
            Render::g_blockHighlight.Render(hit->blockPos, proj, view, shape.min, shape.max);
        }
    }

    void RenderBlockBreakOverlay(const Game::ClientPlayerController& pc,
                                  const glm::mat4& proj, const glm::mat4& view) {
        const int stage = pc.GetDestroyStage();
        if (stage < 0) {
            Render::g_blockBreakOverlay.Clear();
            return;
        }
        const glm::ivec3 bp = pc.GetBreakingPos();
        // Size the crack overlay to the block's actual shape so partial
        // blocks (leaf litter, slabs, …) don't get a full-cube crack
        // floating above / around their geometry.
        const auto stateIdx = Game::Raycast::GetBlockStateAt(bp.x, bp.y, bp.z);
        const auto shape = Client::g_clientBlockAccess
            ? Game::BlockRegistry::GetBlockShapeAt(*Client::g_clientBlockAccess, bp,
                                                   pc.GetBreakingBlockId(), stateIdx)
            : Game::BlockRegistry::GetBlockShape(pc.GetBreakingBlockId(), stateIdx);
        Render::g_blockBreakOverlay.SetTarget(bp, stage, shape.min, shape.max);
        Render::g_blockBreakOverlay.Render(proj, view);
    }

    void RenderCrosshair(GLFWwindow* window) {
        int windowWidth, windowHeight, framebufferWidth, framebufferHeight;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

        Render::g_crosshair.Render(windowWidth, windowHeight, framebufferWidth, framebufferHeight);
    }

    // GUI scale in FRAMEBUFFER pixels per GUI pixel, honoring the "GUI Scale"
    // option (0 = Auto). Byte-for-byte MC Window.calculateScale() semantics:
    // everything counts in raw FRAMEBUFFER pixels (so on a retina display
    // "4" means 4 device px per GUI px, exactly like MC's 4). The scale
    // grows to the largest value that still leaves at least a 320×240 GUI
    // space; an explicit setting caps that growth and still steps down
    // automatically when the window is too small for it. This is what makes
    // the UI hold a constant on-screen size as the window shrinks (taking up
    // a growing fraction of it) instead of shrinking with the window.
    float ComputeGuiScale(int framebufferWidth, int framebufferHeight, int /*windowWidth*/) {
        const int setting = Platform::g_gameSettings.GetGuiScale();
        const int cap = setting >= 1 ? setting : 0x7FFFFFFF; // 0 → Auto (uncapped)

        int scale = 1;
        while (scale != cap &&
               scale < framebufferWidth && scale < framebufferHeight &&
               framebufferWidth / (scale + 1) >= 320 &&
               framebufferHeight / (scale + 1) >= 240) {
            ++scale;
        }
        return static_cast<float>(scale);
    }

    void RenderHUD(GLFWwindow* window, const Game::Inventory& inventory, float deltaTime,
                   const glm::mat4& proj = glm::mat4(1.0f), const glm::mat4& view = glm::mat4(1.0f)) {
        int windowWidth, windowHeight, framebufferWidth, framebufferHeight;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

        if (framebufferWidth <= 0 || framebufferHeight <= 0) return;

        float guiScale = ComputeGuiScale(framebufferWidth, framebufferHeight, windowWidth);
        int guiWidth = static_cast<int>(static_cast<float>(framebufferWidth) / guiScale);
        int guiHeight = static_cast<int>(static_cast<float>(framebufferHeight) / guiScale);

        Render::GuiRenderState renderState;
        Render::GuiGraphics graphics(guiWidth, guiHeight, &g_guiAtlas, &renderState, &g_fontRenderer);
        // "HudRender" covers far more than the HUD — hotbar, chat, the whole
        // inventory screen, the pause/options stack and nametags all render
        // through this one GuiGraphics. Split so a wide zone names a culprit
        // instead of just a phase.
        { PROFILE_ZONE_N("Hud");
        g_hudRenderer.Render(graphics, inventory, deltaTime);
        }

        // Chat: update timer and render messages + input field
        { PROFILE_ZONE_N("Chat");
        g_chatComponent.Update(deltaTime);
        g_chatComponent.Render(graphics, g_chatComponent.GetGameTime(), g_chatScreen.IsOpen());
        g_chatScreen.Render(graphics);
        }

        // Prime suspect when this zone is wide: the creative search tab draws
        // an icon per registered item, and every icon is its own sub-draw.
        { PROFILE_ZONE_N("InventoryScreen");
        Render::GetInventoryScreen().Render(graphics);
        }

        // ── Pause menu / options overlay (ESC) — drawn above everything ────
        {
            auto& screens = Render::GetScreenManager();
            if (!screens.Empty()) {
                PROFILE_ZONE_N("ScreenStack");
                screens.Update(guiWidth, guiHeight);
                auto [smx, smy] = Input::GetMousePosition();
                const int sgx = static_cast<int>(
                    smx * (static_cast<double>(framebufferWidth) / windowWidth) / guiScale);
                const int sgy = static_cast<int>(
                    smy * (static_cast<double>(framebufferHeight) / windowHeight) / guiScale);
                graphics.NextStratum();
                screens.Render(graphics, sgx, sgy, 0.0f);
            }
        }

        // ── Nametags above remote players ─────────────────────────────────────
        // Matches MC's NameTagFeatureRenderer (line 45): poseStack.scale(0.025F, -0.025F, 0.025F)
        // — the tag is a 3D billboard whose on-screen pixel size shrinks with distance.
        // To replicate that without a 3D text pipeline, we project the head position to GUI
        // space and apply a scale = (0.025 * guiHeight * proj[1][1]) / (2 * depth) to GuiGraphics
        // so the rendered text occupies the same screen area as MC's billboard would.
        if (Client::g_remotePlayerManager) {
            glm::mat4 nameVp = proj * view;
            glm::mat4 invView = glm::inverse(view);
            glm::vec3 cameraPos = glm::vec3(invView[3]);

            // proj[1][1] = 1 / tan(vfov/2) — vertical focal length in NDC units per world unit
            const float projY = proj[1][1];

            for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                if (rp.name.empty()) continue;
                // Hide nametag entirely when the player is shifting/sneaking
                if (rp.isCrouching) continue;

                // MC default render distance for nametags is 64 blocks
                float dx = rp.position.x - cameraPos.x;
                float dz = rp.position.z - cameraPos.z;
                if (dx * dx + dz * dz > 64.0f * 64.0f) continue;

                // MC NameTagFeatureRenderer line 43: translate(x, nameTagAttachment.y + 0.5, z)
                // where nameTagAttachment is at the top of the player's bbox (~1.8 high).
                // Our remote player position is at feet, so feet + 1.8 + 0.5 = feet + 2.3.
                glm::vec4 worldPos(rp.position.x, rp.position.y + 2.3f, rp.position.z, 1.0f);
                glm::vec4 clip = nameVp * worldPos;
                if (clip.w <= 0.0f) continue;

                float ndcX = clip.x / clip.w;
                float ndcY = clip.y / clip.w;
                float sx = (ndcX * 0.5f + 0.5f) * guiWidth;
                float sy = (1.0f - (ndcY * 0.5f + 0.5f)) * guiHeight;

                // Perspective scale (MC's 0.025 world-units-per-font-pixel becomes this many GUI
                // pixels at our viewport): factor of guiHeight maps NDC's 2.0-unit Y range to
                // pixels, divide by 2 for the half-range, multiply by projY/depth for projection.
                float scale = (0.025f * static_cast<float>(guiHeight) * projY) / (2.0f * clip.w);

                int textW = g_fontRenderer.GetStringWidth(rp.name);
                const int lineH = Render::FontRenderer::LINE_HEIGHT;

                // MC NameTagFeatureRenderer adds two passes (lines 49-54):
                //   Visible (in front of geometry):  solid white text, NO background  (line 53)
                //   Occluded (behind blocks):        50% white + 25% black bg          (line 51)
                // We approximate the depth test by raycasting from the camera to the tag's
                // world position — a hit means the player is behind something.
                glm::vec3 tagWorld(worldPos.x, worldPos.y, worldPos.z);
                glm::vec3 ray = tagWorld - cameraPos;
                float rayLen = glm::length(ray);
                bool occluded = false;
                if (rayLen > 0.001f) {
                    auto hit = Game::Raycast::CastRay(cameraPos, ray / rayLen, rayLen);
                    occluded = hit.has_value();
                }

                graphics.PushMatrix();
                graphics.Translate(sx, sy);
                graphics.Scale(scale, scale);
                int tagX = -textW / 2;
                int tagY = 0;

                // Background is always drawn (25% alpha black, MC: 0x40000000).
                graphics.Fill(tagX - 1, tagY - 1, tagX + textW + 1, tagY + lineH, 0x40000000);
                if (occluded) {
                    // See-through (MC line 51): -2130706433 = 0x80FFFFFF, 50% white reads as grey.
                    graphics.DrawString(rp.name, tagX, tagY, 0x80FFFFFF, true);
                } else {
                    // Normal (MC line 53): -1 = 0xFFFFFFFF solid white.
                    graphics.DrawString(rp.name, tagX, tagY, 0xFFFFFFFF, true);
                }
                graphics.PopMatrix();
            }
        }

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

        // THE zone to watch. Everything above only queues commands into
        // renderState; this is the single point where the GUI actually hits
        // the GL driver. So a hotbar/inventory/chat that "submits" cheaply can
        // still cost tens of ms here — the cost is paid at the flush, not at
        // the call site that caused it. If the sub-zones above are all small
        // and HudRender is still wide, the answer is in here.
        { PROFILE_ZONE_N("GuiFlush");
        g_guiRenderer.Render(renderState, windowWidth, windowHeight,
                            framebufferWidth, framebufferHeight, guiScale, &g_fontRenderer);
        }
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

    void HandlePlayerInput(Game::ClientPlayer& player, Game::ClientPlayerController& controller, Render::Camera& camera, bool cursorVisible) {
        // When the cursor is visible (Tab-toggle, inventory or chat
        // open) the player is interacting with UI, not the world —
        // drop world-space movement & action input so WASD held when
        // opening an overlay doesn't keep walking, and clicks don't
        // shoot portals / break blocks behind the cursor.
        if (cursorVisible) {
            player.SetMovementInput(glm::vec3(0.0f));
            player.SetJumpPressed(false);
            player.SetJumpHeld(false);
            player.SetSprintPressed(false);
            player.SetSneakPressed(false);
        } else {
            glm::vec3 movementInput = camera.CalculateMovementInput();
            player.SetMovementInput(movementInput);
            player.SetJumpPressed(camera.IsJumpPressed());
            player.SetJumpHeld(camera.IsJumpPressed());
            player.SetSprintPressed(camera.IsSprintPressed());
            player.SetSneakPressed(camera.IsSneakPressed());
        }

        // Block interaction — MC Minecraft.handleKeybinds (Minecraft.java:1979-1999).
        //
        //   while (keyAttack.consumeClick()) startAttack();
        //   while (keyUse.consumeClick())    startUseItem();
        //   continueAttack(screen == null && keyAttack.isDown() && mouseGrabbed);
        //
        // Discrete presses come from the input EVENT queue, held state from
        // isDown. Nothing here diffs a polled level against last frame, which
        // is what used to turn a click that dismissed a screen into a fresh
        // in-world press — Input's callbacks simply never record a press that
        // belonged to the UI.
        if (cursorVisible) {
            // A screen is up: drop anything queued and make sure an in-progress
            // break/use is torn down. MC does the same via KeyMapping.releaseAll
            // on setScreen plus missTime.
            while (Input::ConsumeClick(*Input::Binds::Attack)) {}
            while (Input::ConsumeClick(*Input::Binds::Use))    {}
            controller.ContinueAttack(false);
            controller.StopUseItem();
        } else {
            while (Input::ConsumeClick(*Input::Binds::Attack)) {
                controller.StartAttack();
            }
            while (Input::ConsumeClick(*Input::Binds::Use)) {
                controller.StartUseItem();
            }
            controller.ContinueAttack(Input::IsDown(*Input::Binds::Attack));
            if (!Input::IsDown(*Input::Binds::Use)) controller.StopUseItem();
        }

        // Inventory selection
        // MC handleKeybinds:1897 — `while (keyHotbarSlots[i].consumeClick())`.
        for (int i = 0; i < 9; ++i) {
            while (Input::ConsumeClick(*Input::Binds::Hotbar[i])) controller.OnHotbarChanged(i);
        }

        // Pick block (P key) — server-authoritative. The previous flow only
        // mutated the client's local inventory, so the server's view stayed
        // empty and every subsequent placement / inventory-click silently
        // failed (with the predictive HUD count drifting down to 0). The
        // controller now both predicts the local change AND sends an
        // InventoryClickC2S {CREATIVE_FILL_SLOT} so the server matches.
        if (Input::ConsumeClick(*Input::Binds::PickItem)) {
            if (player.lastBlockHit.has_value()) {
                controller.OnPickBlock(player.lastBlockHit->blockId);
            }
        }

        // Swap main/off hand (F) — MC's SWAP_ITEM_WITH_OFFHAND player action.
        // Gated on !cursorVisible so typing "f"/"q" into chat or the
        // inventory search box doesn't fire world actions.
        if (!cursorVisible && Input::ConsumeClick(*Input::Binds::SwapOffhand)) {
            controller.SendPlayerAction(Network::PlayerAction::SWAP_ITEM_WITH_OFFHAND);
        }

        // Drop held item (Q) — MC's DROP_ITEM player action. No item-entity
        // system yet, so the server just shrinks the stack.
        if (!cursorVisible && Input::ConsumeClick(*Input::Binds::Drop)) {
            controller.SendPlayerAction(Network::PlayerAction::DROP_ITEM);
        }

        // Mouse wheel for inventory scrolling
        auto [scrollX, scrollY] = Input::GetScrollOffset();
        if (scrollY > 0) {
            controller.OnHotbarChanged((player.GetSelectedSlot() - 1 + 9) % 9);
        } else if (scrollY < 0) {
            controller.OnHotbarChanged((player.GetSelectedSlot() + 1) % 9);
        }

        // Debug noclip toggle
        if (Input::ConsumeClick(*Input::Binds::Noclip)) {
            player.ToggleNoclip();
        }

        // F5 — cycle camera perspective (MC: first person → third person
        // back → third person front). Works with the cursor visible too,
        // same as MC.
        if (Input::ConsumeClick(*Input::Binds::TogglePerspective)) {
            camera.CyclePerspective();
        }
    }

    // Cursor visibility is the OR of two independent sources:
    //   - manual: toggled by Tab (user preference)
    //   - overlay: forced visible by UI screens that need pointer input (chat, future menus)
    // The effective state is applied to GLFW only on transition so opening chat while the
    // cursor is already up via Tab doesn't fight the manual toggle (and vice versa).
    // Change cursor capture. ALWAYS go through this rather than calling
    // glfwSetInputMode directly: GLFW reports cursor positions in a different
    // coordinate space either side of the switch, so the first delta measured
    // across it is meaningless and, applied to mouse-look, snaps the view to
    // an arbitrary direction. Pairing the two calls here is what stops a new
    // transition site from silently reintroducing that.
    //
    // MC pairs them the same way — MouseHandler.grabMouse sets ignoreFirstMove
    // right next to grabOrReleaseMouse (:404-407).
    void SetCursorCaptured(GLFWwindow* window, bool captured) {
        glfwSetInputMode(window, GLFW_CURSOR,
                         captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        Input::ResetMouseTracking();
    }

    bool HandleCursorToggle(GLFWwindow* window, Render::Camera& camera, bool overlayWantsCursor) {
        static bool s_manualCursorVisible = false;
        static bool s_lastEffective = false;
        static bool s_initialized = false;

        // Ignore Tab while an overlay holds the cursor — otherwise pressing Tab inside chat
        // would flip the manual state and leave the cursor visible after chat closes.
        if (!overlayWantsCursor && Input::ConsumeClick(*Input::Binds::ToggleCursor)) {
            s_manualCursorVisible = !s_manualCursorVisible;
            Log::Info(s_manualCursorVisible
                      ? "Manual cursor enabled (Tab)"
                      : "Manual cursor disabled (Tab)");
        }

        bool effective = s_manualCursorVisible || overlayWantsCursor;

        if (!s_initialized || effective != s_lastEffective) {
            if (effective) {
                SetCursorCaptured(window, false);
                camera.enableMouseLook = false;
            } else {
                SetCursorCaptured(window, true);
                camera.enableMouseLook = true;
            }
            s_lastEffective = effective;
            s_initialized = true;
        }

        return effective;
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
        Input::SaveKeyBindings();
        Platform::g_gameSettings.Save();
    }

    // ════════════════════════════════════════════════════════════════════════
    // TITLE SCREEN PHASE
    // ════════════════════════════════════════════════════════════════════════
    // Self-contained menu loop that runs after the render/GUI systems are up
    // but BEFORE any server/world/network initialization — mirroring how MC
    // sits on TitleScreen until the player commits to a world. Returns the
    // player's choice; Run() then continues into the normal boot path
    // (integrated server for Singleplayer, remote connect for Multiplayer)
    // or shuts down for Quit.
    Render::TitleAction RunTitleScreenPhase(GLFWwindow* window) {
        using Render::TitleAction;

        if (!Render::g_panoramaRenderer.Initialize(Platform::g_gameSettings.GetString(
                "panoramaSet", Render::PanoramaRenderer::kDefaultSet))) {
            Log::Warning("Panorama init failed — title background will be a gradient");
        }

        auto& screens = Render::GetScreenManager();
        screens.SetVersionString(std::string("MyVoxelGame ") + GAME_VERSION);
        // Back in menu-land: opaque menu backgrounds again (a quit-to-title
        // session left this set to the in-world transparent mode).
        screens.SetInWorld(false);
        screens.Set(std::make_unique<Render::TitleScreen>(/*fadeIn=*/true));

        // Presence: browsing menus.
        if (Client::g_friendsClient) {
            Client::g_friendsClient->SetPresence(
                Client::FriendPresence::State::Menu, "", 0);
        }

        SetCursorCaptured(window, false);

        bool lmbHeld = false;

        double lastTime  = glfwGetTime();
        double tickAccum = 0.0;

        while (!glfwWindowShouldClose(window)) {
            // Title/menu frames get their own Tracy frame set + zone so menu
            // time is attributable in captures instead of appearing as
            // unaccounted main-thread time.
            PROFILE_ZONE_N("TitleFrame");
            // The title screen IS a screen: every key belongs to the UI, so
            // gameplay bindings must record nothing and key presses must queue
            // for Screen::KeyPressed. Set before polling so this frame's events
            // are judged correctly.
            Input::SetUiActive(true);
            glfwPollEvents();
            Input::UpdateKeyStates();

            const double now = glfwGetTime();
            const float  dt  = static_cast<float>(now - lastTime);
            lastTime = now;

            int winW = 0, winH = 0, fbW = 0, fbH = 0;
            glfwGetWindowSize(window, &winW, &winH);
            glfwGetFramebufferSize(window, &fbW, &fbH);
            if (fbW <= 0 || fbH <= 0 || winW <= 0 || winH <= 0) continue; // minimized

            const float guiScale = ComputeGuiScale(fbW, fbH, winW);
            const int guiW = static_cast<int>(static_cast<float>(fbW) / guiScale);
            const int guiH = static_cast<int>(static_cast<float>(fbH) / guiScale);

            screens.Update(guiW, guiH);

            // ── Input (window coords → GUI coords) ─────────────────────────
            auto [mx, my] = Input::GetMousePosition();
            const double gx = mx * (static_cast<double>(fbW) / winW) / guiScale;
            const double gy = my * (static_cast<double>(fbH) / winH) / guiScale;

            const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !lmbHeld)      screens.MouseClicked(gx, gy, GLFW_MOUSE_BUTTON_LEFT);
            else if (!lmb && lmbHeld) screens.MouseReleased(gx, gy, GLFW_MOUSE_BUTTON_LEFT);
            else if (lmb)             screens.MouseDragged(gx, gy);
            lmbHeld = lmb;

            auto [scrollX, scrollY] = Input::GetScrollOffset();
            if (scrollY != 0.0) screens.MouseScrolled(gx, gy, scrollY);
            Input::ResetScrollOffset();

            while (Input::HasCharInput()) screens.CharTyped(Input::PopCharInput());

            // EVERY key press reaches the screen, straight from the GLFW
            // callback (MC hands keys to screen.keyPressed the same way). This
            // used to be a polled whitelist of navigation keys, so a letter
            // never reached Screen::KeyPressed — the Key Binds screen could
            // capture a mouse button but not a keyboard key.
            {
                int uiKey = 0, uiMods = 0;
                while (Input::PopUiKeyPress(uiKey, uiMods)) {
                    const bool consumed = screens.KeyPressed(uiKey, uiMods);
                    // Fullscreen still works from the menus (MC handles its
                    // fullscreen bind in KeyboardHandler.keyPress), but only if
                    // the screen didn't want the key — otherwise the Key Binds
                    // screen could never capture whatever fullscreen is bound to.
                    if (!consumed &&
                        Input::Binds::Fullscreen->key == Input::BoundKey::Keyboard(uiKey)) {
                        ToggleFullscreen(window);
                    }
                }
            }


            // ── One-shot option applications from the options screens ──────
            const uint32_t applied = screens.ConsumeAppliedSettings();
            if (applied & Render::ScreenManager::APPLY_VSYNC) {
                if (Render::g_renderBackend)
                    Render::g_renderBackend->SetVSync(Platform::g_gameSettings.GetVSync());
            }
            if (applied & Render::ScreenManager::APPLY_FULLSCREEN) {
                if (Platform::g_gameSettings.GetFullscreen() != s_isFullscreen)
                    ToggleFullscreen(window);
            }
            if (applied & Render::ScreenManager::APPLY_RAW_MOUSE) {
                if (glfwRawMouseMotionSupported()) {
                    glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
                        Platform::g_gameSettings.GetBool("rawMouseInput", false)
                            ? GLFW_TRUE : GLFW_FALSE);
                }
            }
            // APPLY_RENDER_DISTANCE / APPLY_MAX_FPS need no immediate action
            // here — the game loop reads both settings when it starts.

            // ── 20Hz screen ticks (caret blink etc.) ───────────────────────
            tickAccum += dt;
            while (tickAccum >= 0.05) { screens.Tick(); tickAccum -= 0.05; }

            // ── Render: skybox pass, then GUI pass ─────────────────────────
            if (Render::g_renderBackend) {
                Render::g_renderBackend->BeginFrame();
                Render::g_renderBackend->SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                Render::g_renderBackend->Clear(true, true, true);
                Render::g_renderBackend->SetViewport(0, 0, fbW, fbH);
            }

            const float panoramaSpeed =
                Platform::g_gameSettings.GetFloat("panoramaScrollSpeed", 1.0f);
            Render::g_panoramaRenderer.Render(fbW, fbH, dt, panoramaSpeed);

            {
                Render::GuiRenderState renderState;
                Render::GuiGraphics graphics(guiW, guiH, &g_guiAtlas, &renderState,
                                             &g_fontRenderer);
                screens.Render(graphics, static_cast<int>(gx), static_cast<int>(gy), 0.0f);
                g_guiRenderer.Render(renderState, winW, winH, fbW, fbH, guiScale,
                                     &g_fontRenderer);
            }

            if (Render::g_renderBackend) {
                Render::g_renderBackend->EndFrame(window);
            } else {
                glfwSwapBuffers(window);
            }
            Input::ResetMouseDelta();
            PROFILE_FRAME_MARK_NAMED("TitleFrame");

            // ── Did a button commit to something? ──────────────────────────
            TitleAction action = Render::ConsumeTitleAction();
            if (action.kind != TitleAction::Kind::None) {
                screens.Clear();
                screens.Update(guiW, guiH);
                return action;
            }
        }

        // Window closed from the title screen → quit.
        TitleAction quit;
        quit.kind = TitleAction::Kind::Quit;
        return quit;
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

        // Biome colour tables. Loads PNGs into CPU tables and precomputes from
        // the static biome table — depends on no registry, so it can come
        // first, and it MUST: ItemRegistry::Initialize resolves items' grass /
        // foliage tints through these, and a bush loaded before them would
        // cache the no-colormap fallback and render grey forever.
        // Also has to precede the first mesh build, which it still does.
        Game::BiomeRegistry::LoadColormaps(GetAssetPath("assets/textures"));

        // Initialize block registries
        Game::BlockRegistry::Init();
        // Initialize item registry AFTER blocks so block-items can copy their display names.
        // (Sprite item textures are preloaded later, AFTER the render backend is up.)
        Game::ItemRegistry::Initialize();
        // Crafting recipes resolve their baked-in slugs against BOTH registries,
        // so this has to come last. Runs on the client too — the crafting
        // screen predicts its result square with the same lookup the server
        // uses, which is why the output appears the instant you place the last
        // ingredient rather than a round trip later.
        Game::RecipeManager::Initialize();
        // Block loot tables borrow RecipeManager's slug → ItemID map, so they
        // come after it. Client-side too: harmless there (only the server rolls
        // drops), and it keeps one boot order for both.
        Game::LootTables::Initialize();
        // Weapon ATTACK_DAMAGE / ATTACK_SPEED modifiers, same slug map, same
        // reason to run on both sides: the server scales the damage with it and
        // the client draws the attack indicator from it. Skipping this does not
        // fail loudly — every weapon silently resolves to (0, 0), so a
        // netherite axe hits for the bare-hand 1.0 and recharges in 5 ticks.
        Game::InitItemAttributes();

        // Use platform-specific asset path function
        std::string modelsPath = GetAssetPath("assets/models/block");

        // Load block models
        if (!Game::BlockModelRegistry::LoadModels(modelsPath)) {
            Log::Warning("Failed to load block models from %s, using default models", modelsPath.c_str());
        }

        // Blockstate → model dispatch. MUST run after both BlockRegistry::Init
        // (needs the per-block state definitions) and LoadModels (it rotates
        // already-resolved models). A missing directory is fine — every block
        // just keeps its default model.
        Game::BlockStateModels::Load(GetAssetPath("assets/blockstates"));

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

        // Crumbling overlay (the 10-stage crack texture drawn while mining).
        // Non-fatal: a missing shader just hides the overlay — mining still works.
        if (!Render::g_blockBreakOverlay.Initialize()) {
            Log::Warning("Failed to initialize block break overlay (crack textures will not render)");
        }

        // Sky (sun/moon/stars/day-night) + clouds. Non-fatal: failures fall
        // back to the plain clear-color sky.
        if (!Render::g_skyRenderer.Initialize()) {
            Log::Warning("Sky renderer init failed — sky will be a flat color");
        }
        if (!Render::g_cloudRenderer.Initialize()) {
            Log::Warning("Cloud renderer init failed — clouds will not render");
        }

        // Block-entity dispatcher + per-type renderers (chest, sign, banner,
        // bed, shulker, …). Must run after backend init so renderers can
        // create shaders and textures. Non-fatal: missing renderer → BE is
        // simply not drawn in-world.
        Render::RegisterAllBlockEntityRenderers();

        // Vanilla first-person held-item renderer. Always enabled —
        // independent of the portal feature flag. Non-fatal failure:
        // if shaders don't load, nothing renders in the hand slot.
        if (!Render::g_heldItemRenderer.Initialize()) {
            Log::Warning("Held item renderer init failed — hotbar items will not appear in hand");
        }

#if ENABLE_PORTAL_GUN
        // Phase 4 placeholder portal renderer. Failure is non-fatal — log and
        // continue (the rest of the game should still work; portals just
        // don't render).
        if (!Render::g_portalRenderer.Initialize()) {
            Log::Warning("Portal renderer init failed — portals will not draw");
        }
        // Particle system for the rim sparks. Same non-fatal contract.
        if (!Render::g_portalParticleSystem.Initialize()) {
            Log::Warning("Portal particle system init failed — sparks will not draw");
        }
        // First-person portal-gun viewmodel — loads the real Portal v_portalgun
        // mesh (extracted via SourceIO from Portal-Root/) + its VTF→PNG
        // textures. Non-fatal: failure just hides the viewmodel.
        if (!Render::g_portalGunViewmodel.Initialize()) {
            Log::Warning("Portal gun viewmodel init failed — gun will not appear in hand");
        }
        // Portal quickinfo crosshair overlay — the bracket pair with
        // last-placed pulses. Non-fatal: if the atlas PNG is missing
        // we just don't draw the brackets.
        if (!Render::g_portalCrosshair.Initialize()) {
            Log::Warning("Portal crosshair init failed — bracket overlay disabled");
        }
        // Wire client physics to consult ClientPortalManager when checking
        // block solidity. Lets the player walk through the wall in the 1×2
        // opening behind a fully-paired portal — but only when their AABB
        // fits inside the opening laterally (so they can't tunnel through
        // the block from the side). Server physics doesn't touch this hook.
        Game::SetPortalPassthroughFn(
            [](int x, int y, int z, const Game::AABB& aabb) -> bool {
                return Client::GetClientPortalManager()
                    .IsBlockBehindActivePortal(x, y, z, aabb);
            });
#endif

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

        // Clipboard, for chat's copy-on-click segments. Installed here because
        // this is the only place that owns the GLFWwindow; the GUI layer has no
        // other reason to know about GLFW.
        Render::SetClipboardHandler([](const std::string& text) {
            if (GLFWwindow* w = glfwGetCurrentContext()) {
                glfwSetClipboardString(w, text.c_str());
            }
        });

        // Set up chat message callback
        SetChatMessageCallback([](const Network::ChatMessageS2CPacket& packet) {
            // Translate the wire segments into the renderer's own segment type.
            // Two types rather than one shared struct keeps the GUI free of any
            // network include, matching how the rest of the client is layered.
            std::vector<Render::ChatSegment> segments;
            segments.reserve(packet.segments.size());
            for (const auto& s : packet.segments) {
                Render::ChatSegment seg;
                seg.text  = s.text;
                seg.color = s.color;
                seg.click = (s.click == Network::ChatClickAction::CopyToClipboard)
                          ? Render::ChatClickAction::CopyToClipboard
                          : Render::ChatClickAction::None;
                seg.clickValue = s.clickValue;
                seg.hoverText  = s.hoverText;
                segments.push_back(std::move(seg));
            }
            g_chatComponent.AddMessage(std::move(segments));
        });
        SetChatBubbleCallback([](uint32_t senderId, const std::string& msg) {
            if (Client::g_remotePlayerManager) {
                Client::g_remotePlayerManager->SetChatBubble(senderId, msg);
            }
        });
        // World time sync (TimeUpdate 0x19). Fires on the network I/O thread;
        // EnvironmentState stages the values in atomics.
        SetTimeUpdateCallback([](uint64_t gameTime, uint64_t dayTime, bool doDaylightCycle) {
            Render::EnvironmentState::Get().OnTimeSync(gameTime, dayTime, doDaylightCycle);
        });

        // Compile shaders
        Shader blockShader = InitializeShaders();

        Log::Info("✓ Game systems initialized");
        return true;
    }

    int Run(int argc, char** argv) {
        // Open the log file FIRST, before anything can fail.
        //
        // Everything below logs, and until this runs those lines only reach
        // stdout — which for a launcher-started build goes nowhere at all
        // (macOS `open --args` gives the process no terminal). That is why a
        // player's "it just closed" has never come with any evidence.
        //
        // Uses the DEFAULT game directory rather than waiting for
        // InitializeGameDirectorySystem: that runs hundreds of lines later,
        // after renderer and asset init, which is exactly the window where the
        // interesting startup failures happen. The path is the same either way,
        // and OpenLogFile creates the folder itself.
        {
            const std::string logPath =
                Platform::GameDirectory::GetDefaultGameDirectory() + "/logs/latest.log";
            if (Log::OpenLogFile(logPath)) {
                Log::Info("Log file: %s", logPath.c_str());
            } else {
                Log::Warning("Could not open log file at %s — "
                             "this session will leave no diagnostics on disk",
                             logPath.c_str());
            }
        }

        // This is the frame thread. Claim it before anything spawns a worker,
        // or the scheduler treats it as just another compute thread and parks
        // it behind terrain generation — which shows up as tens of ms of
        // phantom "self time" inside glfwPollEvents.
        Core::SetCurrentThreadPriority(Core::ThreadPriorityClass::Interactive);

        // Pin the terrain library's data root before anything can generate a
        // chunk. Two places need it — BlockPredicate (block tags, used by every
        // placed feature) and FossilTemplate (structure NBTs) — and both check
        // MC_DATA_ROOT first, falling back to walking UP FROM THE WORKING
        // DIRECTORY otherwise.
        //
        // That fallback is why the game worked from an IDE and not from the
        // launcher: CLion runs it with the cwd inside the repo, so the walk
        // finds data/, while `open` (which is how the launcher starts the
        // bundle, and there is no way to set a cwd with it) hands the app "/".
        // The walk then hits the filesystem root and gives up, BlockPredicate
        // throws, ServerWorkerPool::ProcessChunkGeneration catches it per job —
        // and every single chunk fails to generate with no crash and no visible
        // error. You spawn, you can look around, and nothing ever loads.
        //
        // overwrite=0 so an explicitly exported MC_DATA_ROOT still wins.
        {
            const std::string dataRoot = GetAssetPath("data");
            if (std::filesystem::exists(dataRoot)) {
#ifdef _WIN32
                _putenv_s("MC_DATA_ROOT", dataRoot.c_str());
#else
                setenv("MC_DATA_ROOT", dataRoot.c_str(), 0);
#endif
                Log::Info("Terrain data root: %s", dataRoot.c_str());
            } else {
                Log::Error("=========================================================");
                Log::Error("data/ NOT FOUND (looked in: %s)", dataRoot.c_str());
                Log::Error("Chunk generation WILL fail — every chunk throws while");
                Log::Error("resolving block tags, and the world loads up empty.");
                Log::Error("The build must copy data/ next to assets/ (CMakeLists).");
                Log::Error("=========================================================");
            }
        }

        // Parse command-line arguments
        bool useVulkan = false;
        bool crashTest = false;
        bool isRemoteClient = false;
        std::string remoteServerAddress;
        uint16_t remoteServerPort = 25565;
        std::string playerName; // Empty → server auto-assigns "PlayerN" based on connection ID
        Game::PlayerColorId playerColor = Game::PlayerColorId::Default;
        // Friends-service identity (from the launcher; empty token = guest).
        std::string friendsSessionToken;
        int64_t friendsAccountId = 0;
        std::string friendsServiceHost = Friends::kDefaultServiceHost;
        uint16_t friendsServicePort = Friends::kDefaultServicePort;
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
            if (arg == "--color" && i + 1 < argc) {
                std::string colorSlug = argv[++i];
                playerColor = Game::ParsePlayerColorName(colorSlug);
                Log::Info("Player color set to: %s (id=%u)",
                          Game::LookupPlayerColor(playerColor).name,
                          static_cast<unsigned>(playerColor));
            }
            if (arg == "--session" && i + 1 < argc) {
                friendsSessionToken = argv[++i];
            }
            if (arg == "--account-id" && i + 1 < argc) {
                friendsAccountId = std::atoll(argv[++i]);
            }
            if (arg == "--friends-service" && i + 1 < argc) {
                // "host" or "host:port" override of the shared defaults.
                std::string hostPort = argv[++i];
                auto colonPos = hostPort.rfind(':');
                if (colonPos != std::string::npos && colonPos + 1 < hostPort.size()) {
                    friendsServiceHost = hostPort.substr(0, colonPos);
                    int p = std::atoi(hostPort.c_str() + colonPos + 1);
                    if (p > 0 && p <= 65535) friendsServicePort = static_cast<uint16_t>(p);
                } else {
                    friendsServiceHost = hostPort;
                }
            }
        }
        if (!friendsSessionToken.empty()) {
            Log::Info("Friends session provided (account %lld, service %s:%u)",
                      static_cast<long long>(friendsAccountId),
                      friendsServiceHost.c_str(),
                      static_cast<unsigned>(friendsServicePort));
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

        // Local crash report, installed AFTER sentry_init on purpose: the last
        // handler registered is the first to run, and ours deliberately chains
        // back to whatever was there, so Sentry still gets its report.
        //
        // Sentry is the better report when it arrives — symbolicated and
        // automatic — but it needs network, a working crashpad process, and a
        // crash of a kind crashpad claims. This one always leaves a file the
        // player can attach to a message.
        Platform::InstallCrashHandler(
            Platform::GameDirectory::GetDefaultGameDirectory() + "/crash-reports",
            GAME_VERSION);

        // Intentional crash for testing the crash pipeline (run with
        // --crash-test). Exercises BOTH reporters: Sentry via crashpad, and
        // the local handler above.
        if (crashTest) {
            Log::Info("Crash test requested — crashing in 3 seconds...");
            Log::Info("Expect a report at: %s", Platform::CrashReportPath());
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
            // Explicitly request 8 stencil bits for the default framebuffer.
            // GLFW defaults to 8 already on every modern desktop driver, but
            // making this explicit guarantees the portal renderer (Phase 6+)
            // gets a stencil buffer regardless of platform-specific defaults.
            glfwWindowHint(GLFW_STENCIL_BITS, 8);
            glfwWindowHint(GLFW_DEPTH_BITS,   24);
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

        // Eagerly preload sprite-based item textures NOW (backend is up). Without this, the
        // first render frame that uses a sprite item raced against texture creation — the
        // unselected Search tab compass icon stayed empty until the user clicked Search,
        // which forced a re-render after the texture had finished uploading.
        // Block items don't need this — they live in the block atlas built later.
        Render::GuiGraphics::PreloadItem(Game::Items::Compass);

        // Initialize game directory system (creates obeycraft folder and loads options.txt)
        if (!Platform::InitializeGameDirectorySystem()) {
            Log::Error("Failed to initialize game directory system");
            return -1;
        }

        // Apply the SAVED vsync setting through the backend — this must run
        // AFTER InitializeGameDirectorySystem (which loads options.txt);
        // before that, GetVSync() returns the compiled-in default (true).
        // The GL-only glfwSwapInterval during GL setup has the same
        // too-early problem, so this call is the authoritative one for both
        // backends. On Vulkan it flags a swapchain recreate (present mode
        // lives in the swapchain; VKBackend defaults to vsync-on FIFO).
        if (Render::g_renderBackend) {
            Render::g_renderBackend->SetVSync(Platform::g_gameSettings.GetVSync());
        }

        // Initialize input. Bindings are registered BEFORE Input::Init so the
        // GLFW callbacks it installs already have a table to dispatch into,
        // then loaded from options.txt (absent entries keep the vanilla
        // default). Mirrors MC building its KeyMapping set before Options
        // reads them back in.
        Input::InitKeyMappings();
        Input::LoadKeyBindings();
        Input::Init(window);
        SetCursorCaptured(window, true);

        // Apply fullscreen setting from saved preferences
        if (Platform::g_gameSettings.GetFullscreen()) {
            ToggleFullscreen(window);
        }

        // Initialize game systems BEFORE any chunk loading
        if (!InitializeGameSystems(window)) {
            Log::Error("Failure to init");
            return 1;
        }

        // === TITLE SCREEN PHASE ===
        // Runs before any server/world/network init — the player chooses
        // Singleplayer / Multiplayer / Quit. The CLI --server flag skips the
        // menu so the launcher's "Join Server" flow still connects directly.
        // ── Friends service connection (app lifetime, spans all sessions) ──
        // Guests (no --session) get no client; every friends feature checks
        // for null and disables itself.
        if (!friendsSessionToken.empty()) {
            Client::g_friendsClient = std::make_unique<Client::FriendsClient>();
            Client::g_friendsClient->Start(friendsServiceHost, friendsServicePort,
                                           friendsSessionToken, friendsAccountId);
        }

        // ═══════════════ OUTER SESSION LOOP ═══════════════
        // Each iteration is: title screen → one world/server session →
        // session teardown. "Save and Quit to Title" loops back here;
        // closing the window or "Quit Game" breaks out to the process-level
        // cleanup after the loop. The loop body keeps its original
        // indentation — it is the whole remainder of Run() minus the final
        // process cleanup.
        const bool cliRemoteClient = isRemoteClient;
        bool firstSession = true;
        // Set when an in-game "Join friend" tears down the current session:
        // the next outer-loop iteration consumes it directly instead of
        // showing the title screen.
        std::optional<Render::TitleAction> pendingSessionAction;
        for (;;) {
        // CLI --server bypasses the title screen for the FIRST session only;
        // after a quit-to-title the menu shows normally.
        isRemoteClient = cliRemoteClient && firstSession;
        firstSession = false;

        // No world session yet — the options menu's World Settings entry
        // stays greyed out during the title phase.
        Render::WorldSettingsContext::Clear();

        Render::TitleAction titleAction;   // world choice consumed by server init below
        if (pendingSessionAction) {
            // In-game friend join: skip the title phase, go straight into
            // the new session with the stashed action.
            titleAction = *pendingSessionAction;
            pendingSessionAction.reset();
            if (titleAction.kind == Render::TitleAction::Kind::Multiplayer) {
                isRemoteClient = true;
                remoteServerAddress = titleAction.host;
                remoteServerPort = titleAction.port;
                Log::Info("Auto-joining %s:%u (friend join)",
                          remoteServerAddress.c_str(),
                          static_cast<unsigned>(remoteServerPort));
            }
            SetCursorCaptured(window, true);
        } else if (!isRemoteClient) {
            titleAction = RunTitleScreenPhase(window);
            if (titleAction.kind == Render::TitleAction::Kind::Quit) {
                Log::Info("Quit from title screen — shutting down");
                // Only render/GUI systems exist at this point; release them in
                // the same order as the main shutdown sequence (dependents
                // before the backend).
                Render::g_panoramaRenderer.Shutdown();
                g_hudRenderer = Render::HudRenderer();
                g_guiRenderer.Shutdown();
                g_fontRenderer.Shutdown();
                g_guiAtlas.Shutdown();
                Render::g_crosshair.Shutdown();
                Render::g_blockHighlight.Shutdown();
                Render::g_blockBreakOverlay.Shutdown();
                Render::g_skyRenderer.Shutdown();
                Render::g_cloudRenderer.Shutdown();
                if (Render::g_atlasBuilder)    Render::g_atlasBuilder.reset();
                if (Render::g_textureAnimator) Render::g_textureAnimator.reset();
                if (Client::g_friendsClient) {
                    Client::g_friendsClient->Stop();
                    Client::g_friendsClient.reset();
                }
                // A prior session (quit-to-title) may have initialized the
                // debug system; ImGui must go down before the backend.
                if (s_debugSystemInitialized) {
                    Debug::DebugSystem::Shutdown();
                    s_debugSystemInitialized = false;
                }
                if (Render::g_renderBackend) {
                    Render::g_renderBackend->Shutdown();
                    Render::g_renderBackend.reset();
                }
                glfwDestroyWindow(window);
                glfwTerminate();
                return 0;
            }
            if (titleAction.kind == Render::TitleAction::Kind::Multiplayer) {
                isRemoteClient = true;
                remoteServerAddress = titleAction.host;
                remoteServerPort = titleAction.port;
                Log::Info("Title screen: joining server %s:%u",
                          remoteServerAddress.c_str(),
                          static_cast<unsigned>(remoteServerPort));
            }
            // Singleplayer (or Multiplayer) — panorama is done, gameplay owns
            // the cursor again.
            Render::g_panoramaRenderer.Shutdown();
            SetCursorCaptured(window, true);
        }

        // Raw-input preference for gameplay mouse-look (Mouse Settings).
        if (glfwRawMouseMotionSupported() &&
            Platform::g_gameSettings.GetBool("rawMouseInput", false)) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }

        // Register custom item renderers (MC's BlockEntityWithoutLevelRenderer equivalent).
        // These render block entities via 3D entity-texture models, not the standard
        // block-model JSON system. Add more entries here for sign, banner, head, etc.
        // MUST run AFTER InitializeGameSystems → ItemRegistry::Initialize, since each
        // walks the registry to find every item with the matching specialKind.
        Render::RegisterChestItemRenderer();
        Render::RegisterBedItemRenderer();
        Render::RegisterShulkerBoxItemRenderer();
        Render::RegisterBannerItemRenderer();
        Render::RegisterHeadItemRenderer();
        Render::RegisterShieldItemRenderer();

        // Initialize remote player tracking and renderer
        Client::g_remotePlayerManager = std::make_unique<Client::RemotePlayerManager>();
        Render::PlayerRenderer playerRenderer;
        if (!playerRenderer.Initialize()) {
            Log::Warning("Failed to initialize player renderer, remote players won't be visible");
        }

        // Dropped items in the world. Created BEFORE the connection opens so a
        // spawn packet arriving on the very first tick has somewhere to land.
        Client::g_itemEntityManager = std::make_unique<Client::ItemEntityManager>();
        Client::g_clientMobManager = std::make_unique<Client::ClientMobManager>();
        Render::MobRenderer mobRenderer;
        if (!mobRenderer.Initialize()) {
            Log::Warning("[PlatformMain] mob renderer failed to initialize — mobs will be invisible");
        }
        Render::ItemEntityRenderer itemEntityRenderer;
        if (!itemEntityRenderer.Initialize()) {
            Log::Warning("Failed to initialize item entity renderer, dropped items won't be visible");
        }

        // === MINECRAFT-STYLE ARCHITECTURE INITIALIZATION ===
        Log::Info("Initializing Minecraft Java Edition Architecture...");

        // ClientBlockAccess for remote clients (physics/raycast backed by client chunk cache)
        std::unique_ptr<Client::ClientBlockAccess> clientBlockAccess;
        Game::World* world = nullptr;

        // Fresh session: drop any stale time from a previous world so the sky
        // doesn't flash the old time before the first TimeUpdate arrives, and
        // apply this world's skybox (multiplayer joins default to vanilla).
        Render::EnvironmentState::Get().ResetSession();
        Render::g_skyRenderer.SetSkybox(titleAction.skybox, titleAction.skyboxMode);
        // Enable the in-game World Settings screen. Sky choices persist to
        // worlds.json only for locally-hosted created worlds; multiplayer
        // and Minecraft-save sessions get session-only changes.
        Render::WorldSettingsContext::Set(
            titleAction.worldName,
            !isRemoteClient && !titleAction.useMinecraftSave);

        if (!isRemoteClient) {
            // 1. Initialize server-side systems (server creates and owns the world)
            Server::IntegratedServerConfig serverConfig;
            serverConfig.tickRate = 20;                     // 20 TPS like Minecraft
            serverConfig.enableAsyncChunkLoading = true;     // Async via ServerWorkerPool (non-blocking)
            serverConfig.useMinecraftSave = titleAction.useMinecraftSave;

            // World chosen on the Select World screen: created worlds are
            // procedural-from-seed (metadata only, no save path yet), while
            // the "world" list entry keeps the auto-detected Anvil save.
            if (!titleAction.useMinecraftSave) {
                serverConfig.useLocalSaveDirectory = false;
            }

            // World game mode from the create/select screen. World JSON ids:
            // 0 Survival, 1 Creative, 2 Hardcore — hardcore plays as survival
            // (server GameMode has no hardcore variant).
            serverConfig.defaultGameMode = (titleAction.gameMode == 1) ? 1 : 0;

            // Day/night cycle state restored from worlds.json metadata.
            serverConfig.initialDayTime  = titleAction.dayTime;
            serverConfig.doDaylightCycle = titleAction.doDaylightCycle;

            // Read-only worlds (imported from the player's Minecraft install)
            // must never write chunks back — see IntegratedServerConfig.
            serverConfig.readOnlyWorld = titleAction.readOnlyWorld;

            if (titleAction.useMinecraftSave && !titleAction.worldPath.empty()) {
                // A specific world picked on the Select World screen. Point at
                // the world FOLDER (not /region — MinecraftChunkLoader appends that).
                serverConfig.minecraftWorldPath = titleAction.worldPath;
                Log::Info("✓ Loading Anvil world%s: %s",
                          serverConfig.readOnlyWorld ? " (read-only)" : "",
                          serverConfig.minecraftWorldPath.c_str());
            } else if (serverConfig.useLocalSaveDirectory &&
                       Platform::g_gameDirectory.HasDefaultSaveWorld()) {
                // Legacy fallback: the auto-detected saves/world.
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

            // Seed the generator for created worlds BEFORE any chunk
            // generation kicks off (server thread starts further down).
            if (!titleAction.useMinecraftSave) {
                world->SetGenerationSeed(titleAction.seed);
                Log::Info("World '%s': procedural generation from seed %d (%s)",
                          titleAction.worldName.c_str(), titleAction.seed,
                          titleAction.gameMode == 0 ? "Survival" : "Creative");
            }

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

            // Physics/raycast read through this on a remote client. It is
            // ALSO created for the host below — see the shared setup.
        }

        // Mesh pipeline backpressure — and the ceiling on how fast chunks become
        // visible. A permit is held from submit through compile until the render
        // thread uploads the result, so throughput is permits / round-trip.
        //
        // MC sizes its equivalent from
        //   min(availableProcessors, maxMemory * 0.3 / TOTAL_BUFFERS_SIZE)
        // (SectionBufferBuilderPool.allocate, Minecraft.java:550). That cap is
        // MEMORY-driven: each SectionBufferBuilderPack is a real allocated buffer
        // set. Ours are pure counters — the buffers live elsewhere — so copying
        // MC's number matched its arithmetic while our actual constraint is the
        // per-frame upload cost, not heap.
        //
        // Sized from measurement instead. Peak chunk arrival is ~120 chunks/s,
        // and each chunk costs sections/chunk (7.9) x the MC-faithful remesh
        // factor (2.2) ~ 17.4 section meshes:
        //
        //     peak demand      ~2086 sections/s
        //     observed permit round-trip ~6.9 ms (about 1.7 frames)
        //     permits needed   2086 * 0.0069 ~ 14.4
        //
        // 16 covers that with margin. At peak that is ~0.5 ms/frame of upload
        // (0.060 ms per section) against a ~4 ms frame — the pool still throttles
        // scheduling the moment uploads fall behind, which is the point of it.
        // Raise further only with a trace showing permits starved AND frame time
        // to spare; this is the knob that trades frame smoothness for fill speed.
        {
            const unsigned hw = std::thread::hardware_concurrency();
            const size_t permitCount = std::max<size_t>(16, hw > 0 ? hw : 4);
            Render::GetMeshUploadPermits().Initialize(permitCount);
            Log::Info("✓ Mesh upload permits: %zu", permitCount);
        }

        // ClientBlockAccess is needed in BOTH modes. Besides being the remote
        // client's physics/raycast source, it is the ILevelWrite that item
        // behaviours (hoe, shovel, bucket, …) write through when the client
        // runs them for prediction — and prediction always targets the CLIENT
        // chunk cache, never the server World, because that cache is what the
        // renderer meshes from.
        clientBlockAccess = std::make_unique<Client::ClientBlockAccess>();
        Client::g_clientBlockAccess = clientBlockAccess.get();

        // 4. Initialize client-side systems (always needed)
        Client::InitializeClientChunkManager();
        Render::InitializeClientMeshManager(Client::g_clientChunkManager.get());
        Log::Info("✓ Client systems initialized (chunk manager, mesh manager)");

        // MC ClientPacketListener.startWaitingForNewLevel, on entering a level.
        // Arms the client-side readiness watch; once the player's own section
        // compiles it sends PlayerLoadedC2S, which is what lifts the server's
        // interaction gate (PlayerSession::HasClientLoaded).
        Client::g_levelLoadTracker.StartClientLoad();

        // 5. Initialize player and controller
        Game::ClientPlayer player;
        player.color = playerColor; // from --color CLI arg parsed earlier
        Game::ClientPlayerController playerController;
        playerController.SetPlayer(&player);
        Render::SetInventoryScreenPlayer(&player);
        if (!isRemoteClient) {
            playerController.SetWorld(world);
            playerController.SetBlockAccess(world);
        } else {
            // Remote client: no World for block placement (server handles it).
            // Block READS still have to work though — the controller needs
            // hardness/target lookups for the mining state machine, and it
            // gets them from the client chunk cache. Leaving this null was why
            // mining on a remote server ignored hardness entirely: every
            // lookup returned Air, whose destroyTime of 0 means "instant".
            playerController.SetWorld(nullptr);
            playerController.SetBlockAccess(clientBlockAccess.get());
        }

        // 6. Configure IntegratedServer with player (host only)
        if (!isRemoteClient && Server::g_integratedServer) {
            Server::g_integratedServer->SetPlayer(&player);
        }

        // NOTE: SetTeleportCallback now lives after `camera` is declared
        // (right before the main loop) so the lambda can capture &camera.
        // Without that, mouse-look's `camera.yaw` keeps the player looking
        // in the pre-teleport direction even after a server snap — visible
        // as portals teleporting position but not view, and as /tp <x y z
        // yaw pitch> ignoring the rotation arguments.

        // 7. Initialize rendering systems (keeping existing ones that still work)
        if (!Render::InitializeChunkRenderer()) {
            Log::Error("Failed to initialize chunk renderer");
            return -7;
        }

        // 8. Initialize debug system — ONCE per process (ImGui backend init
        // is not re-entrant); later sessions in the outer loop reuse it.
        if (!s_debugSystemInitialized) {
            Debug::DebugSystem::Initialize(window);
            s_debugSystemInitialized = true;
        }

        // 9. Start the IntegratedServer thread (host only)
        if (!isRemoteClient) {
            if (!Server::StartIntegratedServer()) {
                // Almost always "port 25565 already bound" — i.e. a second copy
                // of the game is running (i.e. one from the IDE and one from
                // the launcher). Silently continuing produces the world's most
                // confusing bug report: the game loads, you can look around,
                // and no chunk ever appears, because the client below dials
                // 127.0.0.1 and finds either nothing or the OTHER instance's
                // server. Loud and fatal beats quiet and mystifying.
                Log::Error("=========================================================");
                Log::Error("FAILED TO START INTEGRATED SERVER");
                Log::Error("Port 25565 is most likely already in use by another");
                Log::Error("running copy of the game. Close it and try again.");
                Log::Error("(check with: lsof -nP -iTCP:25565)");
                Log::Error("=========================================================");
                return -8;
            }
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

        // Set player name + colour for handshake — both forwarded to the server
        // so OTHER clients can render this player's stick figure correctly.
        networkClient->SetPlayerName(playerName);
        networkClient->SetPlayerColor(static_cast<uint8_t>(playerColor));

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

        // Relayed friend join: connectHost/serverPort point at the friends
        // service, not the host's machine. Present the ticket first so the
        // relay can splice us to the host's outbound tunnel; the game
        // protocol then proceeds untouched. The ticket is service-generated
        // hex, so it needs no escaping here.
        if (!titleAction.relayTicket.empty()) {
            networkClient->SetConnectPreamble(
                std::string("{\"op\":\"relay_attach\",\"role\":\"joiner\",\"ticket\":\"")
                + titleAction.relayTicket + "\"}\n");
            Log::Info("Joining through the friends relay");
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

        // Menus opened from here on (ESC pause menu) render over the live
        // world — switch the screen stack to the transparent background mode.
        Render::GetScreenManager().SetInWorld(true);

        // Presence: in a world. Hosting = integrated server (friends can
        // join via join_info); Playing = we're a client on someone's server.
        if (Client::g_friendsClient) {
            if (!isRemoteClient) {
                const std::string worldName = titleAction.worldName.empty()
                    ? std::string("world") : titleAction.worldName;
                const uint16_t hostPort =
                    Server::g_integratedServer && Server::g_integratedServer->GetNetworkServer()
                        ? Server::g_integratedServer->GetNetworkServer()->GetPort()
                        : uint16_t(25565);

                // Announce immediately so friends see the world right away;
                // the UPnP attempt below refines this with the WAN address.
                Client::g_friendsClient->SetPresence(
                    Client::FriendPresence::State::Hosting, worldName, hostPort);

                // Friends who can't reach us directly are relayed: the
                // service pushes relay_open, our friends client dials out,
                // and the resulting socket is adopted here as a normal
                // player connection. Looked up per-call so a torn-down
                // session can't leave a dangling server pointer.
                Client::g_friendsClient->SetRelaySocketHandler([](auto handle) {
                    if (Server::g_integratedServer) {
                        if (auto* netServer = Server::g_integratedServer->GetNetworkServer()) {
                            netServer->AdoptConnection(handle);
                            return;
                        }
                    }
                    Log::Warning("Relay tunnel arrived with no server to adopt it");
                });

                // Try to open the router port so friends connect DIRECTLY
                // (no relay hop). Blocking + slow, so it runs on a worker;
                // failure is fine — the service verifies reachability and
                // falls back to relaying either way.
                if (!g_portMapper) {
                    g_portMapper = std::make_unique<Client::UPnPPortMapper>();
                }
                std::thread([worldName, hostPort]() {
                    const auto mapping = g_portMapper->Map(hostPort);
                    if (Client::g_friendsClient) {
                        Client::g_friendsClient->SetPresence(
                            Client::FriendPresence::State::Hosting,
                            worldName, hostPort, mapping.externalIp);
                    }
                }).detach();
            } else {
                Client::g_friendsClient->SetPresence(
                    Client::FriendPresence::State::Playing, connectHost, 0);
            }
        }

        // === MINECRAFT-STYLE MAIN LOOP ===
        // Matches Minecraft.java: processQueuedPackets() → tick() (20 TPS) → render() (uncapped)
        // Packets and game logic run at fixed 20 TPS. Rendering is decoupled at uncapped FPS.

        // Initialize camera for client thread
        Render::Camera camera;
        camera.position = glm::vec3(0.0f, 67.0f, 0.0f);
        camera.physicsControlled = true;

        // Wire teleport packet → local player snap (matches MC client's
        // handleMovePlayer: always snap, no prediction-error threshold; zero
        // velocity to match server's Vec3.ZERO delta in
        // connection.teleport(x,y,z,yRot,xRot)). Yaw/pitch on the wire come
        // from `camera.yaw` (sent in PlayerMoveC2S) so we write them BACK to
        // `camera.yaw` here — `player.yaw` is informational only and doesn't
        // drive rendering. Without the camera write, portal teleports and
        // `/tp ... <yaw> <pitch>` would snap position but ignore rotation.
        SetTeleportCallback([&player, &camera](double x, double y, double z,
                                                float yRot, float xRot,
                                                double dx, double dy, double dz) {
            glm::dvec3 dpos(x, y, z);
            player.physics.position = glm::vec3(dpos);
            // Velocity in blocks/sec from the server (rotated through the
            // portal pair for portal teleports, zero for /tp). Writing to
            // physics.velocity preserves the player's momentum across the
            // teleport instead of stopping them on landing.
            player.physics.velocity = glm::vec3(
                static_cast<float>(dx),
                static_cast<float>(dy),
                static_cast<float>(dz));
            player.predictedPos = dpos;
            player.serverPos    = dpos;
            player.visualPos    = dpos;
            player.yaw   = yRot;
            player.pitch = xRot;
            camera.yaw   = yRot;
            camera.pitch = xRot;
            // Teleports break falls (MC resetFallDistance on teleport) —
            // without this a /tp or respawn mid-fall would carry the
            // accumulated distance into the next landing.
            player.physics.fallDistance = 0.0f;
            player.landedFallSinceMoveSend = 0.0f;
        });

        // Network tracking
        uint32_t playerMoveSequence = 0;

        // Performance tracking
        Debug::PerformanceMetrics metrics;
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        // Client tick timing (20 TPS, matching Minecraft and server)
        static constexpr auto CLIENT_TICK_INTERVAL = std::chrono::milliseconds(50);
        static constexpr int MAX_TICKS_PER_FRAME = 10;
        auto nextClientTick = std::chrono::steady_clock::now();

        // Speed-driven FOV (MC GameRenderer.tickFov + fovModifier). Smoothed
        // at 20 TPS with MC's 0.5 blend factor and lerped across the frame so
        // the zoom eases in instead of snapping the instant sprint engages.
        float fovModifier    = 1.0f;   // current (this tick)
        float fovModifierOld = 1.0f;   // previous tick — render lerps between
        double fovTickAccum  = 0.0;

        // Set by the pause menu's "Save and Quit to Title": breaks the main
        // loop, the session teardown below runs, and the outer session loop
        // returns to the title screen.
        bool returnToTitle = false;

        while (!glfwWindowShouldClose(window) && !returnToTitle) {
            frameStartTime = std::chrono::high_resolution_clock::now();

            // === PER-FRAME: Poll events and handle input (must be every frame for responsiveness) ===
            bool cursorEnabled;
            // Covers polling AND every UI branch (chat / inventory / pause) —
            // not just input. The children below say which part actually ran.
            { PROFILE_ZONE_N("InputUI");
            PROFILE_TIMER_START(input);
            { PROFILE_ZONE_N("PollEvents");
            glfwPollEvents();
            }
            { PROFILE_ZONE_N("KeyStates");
            Input::UpdateKeyStates();
            }

            // Apply menu-editable options every frame (cheap settings-map
            // lookups). FOV feeds the projection below; sensitivity/invert
            // feed Camera::Update's mouse-look. Sensitivity 1.0 (= 50% on
            // the slider, the default) maps to the engine's historical
            // 0.1°/px feel; the slider scales linearly around that, up to
            // 4.0 (200%, 0.4°/px). Stored values keep this meaning across
            // the slider's rescale, so an existing options.txt is unchanged.
            { PROFILE_ZONE_N("Settings");
            camera.fov              = Platform::g_gameSettings.GetFOV();
            camera.mouseSensitivity = Platform::g_gameSettings.GetMouseSensitivity() * 0.1f;
            camera.invertY          = Platform::g_gameSettings.GetInvertYMouse();
            }

            // Escape no longer closes the game — use the window close button instead

            if (Input::ConsumeClick(*Input::Binds::Fullscreen)) {
                ToggleFullscreen(window);
            }

            // ESC edge-detection is SHARED across every branch below. The
            // chat and inventory branches consume ESC to close themselves via
            // their own trackers; updating the shared held-state up here
            // guarantees that same physical press can't re-edge in the game
            // branch one frame later and pop the pause menu open.
            extern bool s_escKeyHeld;
            const bool escIsDown = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            const bool escPressedThisFrame = escIsDown && !s_escKeyHeld;
            s_escKeyHeld = escIsDown;

            // ── Pause-menu / options overlay (ESC — MC Game Menu) ──────────
            // Takes priority over chat/inventory (those close before the
            // pause menu can open, so they're never open simultaneously).
            if (!Render::GetScreenManager().Empty()) {
                PROFILE_ZONE_N("ScreenMgrInput");
                auto& screens = Render::GetScreenManager();

                // Screens swallow gameplay input — but HandlePlayerInput
                // doesn't run in this branch, so whatever movement was held
                // when the screen opened (W while dying → death screen)
                // stays LATCHED and physics keeps walking the body forever.
                // Clear it here every frame the screen is up.
                player.SetMovementInput(glm::vec3(0.0f));
                player.SetJumpPressed(false);
                player.SetJumpHeld(false);
                player.SetSprintPressed(false);
                player.SetSneakPressed(false);

                // Mouse position in GUI coords (same mapping as the render
                // pass in RenderHUD).
                auto [pmx, pmy] = Input::GetMousePosition();
                double pgx = 0.0, pgy = 0.0;
                {
                    int winW = 0, winH = 0, fbW = 0, fbH = 0;
                    glfwGetWindowSize(window, &winW, &winH);
                    glfwGetFramebufferSize(window, &fbW, &fbH);
                    if (winW > 0 && winH > 0 && fbW > 0 && fbH > 0) {
                        const float pScale = ComputeGuiScale(fbW, fbH, winW);
                        pgx = pmx * (static_cast<double>(fbW) / winW) / pScale;
                        pgy = pmy * (static_cast<double>(fbH) / winH) / pScale;
                    }
                }

                // LMB edge → click/drag/release.
                static bool pauseLmbHeld = false;
                const bool pLmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                if (pLmb && !pauseLmbHeld)      screens.MouseClicked(pgx, pgy, GLFW_MOUSE_BUTTON_LEFT);
                else if (!pLmb && pauseLmbHeld) screens.MouseReleased(pgx, pgy, GLFW_MOUSE_BUTTON_LEFT);
                else if (pLmb)                  screens.MouseDragged(pgx, pgy);
                pauseLmbHeld = pLmb;

                // Scroll → options lists; consume so the hotbar doesn't move.
                auto [pScrollX, pScrollY] = Input::GetScrollOffset();
                if (pScrollY != 0.0) screens.MouseScrolled(pgx, pgy, pScrollY);
                Input::ResetScrollOffset();

                // Chars → screens (future edit boxes); drains the queue.
                while (Input::HasCharInput()) screens.CharTyped(Input::PopCharInput());

                // ESC closes the top screen (shared edge state — see above).
                if (escPressedThisFrame) screens.KeyPressed(GLFW_KEY_ESCAPE, 0);

                // Every key press, from the GLFW callback — see the title
                // phase for why the old polled whitelist wasn't enough. ESC is
                // skipped because escPressedThisFrame above already delivered
                // it through the shared edge, and sending it twice would close
                // a screen that had just consumed it.
                {
                    int uiKey = 0, uiMods = 0;
                    while (Input::PopUiKeyPress(uiKey, uiMods)) {
                        if (uiKey == GLFW_KEY_ESCAPE) continue;
                        screens.KeyPressed(uiKey, uiMods);
                    }
                }

                // One-shot option applications (same set as the title phase,
                // plus render distance which needs the live connection).
                const uint32_t applied = screens.ConsumeAppliedSettings();
                if (applied & Render::ScreenManager::APPLY_VSYNC) {
                    if (Render::g_renderBackend)
                        Render::g_renderBackend->SetVSync(Platform::g_gameSettings.GetVSync());
                }
                if (applied & Render::ScreenManager::APPLY_FULLSCREEN) {
                    if (Platform::g_gameSettings.GetFullscreen() != s_isFullscreen)
                        ToggleFullscreen(window);
                }
                if (applied & Render::ScreenManager::APPLY_RAW_MOUSE) {
                    if (glfwRawMouseMotionSupported()) {
                        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
                            Platform::g_gameSettings.GetBool("rawMouseInput", false)
                                ? GLFW_TRUE : GLFW_FALSE);
                    }
                }
                if (applied & Render::ScreenManager::APPLY_RENDER_DISTANCE) {
                    // Same path as the debug-UI slider: re-send client
                    // settings so the server retunes the watch set.
                    const int newDist = Platform::g_gameSettings.GetRenderDistance();
                    Log::Info("Render distance changed to %d (options)", newDist);
                    if (networkClient) {
                        if (auto conn = networkClient->GetConnection()) {
                            conn->SendClientSettings(
                                newDist,
                                Platform::g_gameSettings.GetVSync(),
                                Platform::g_gameSettings.GetMouseSensitivity());
                        }
                    }
                }

                // "Save and Quit to Title" ends the session: the main loop
                // breaks, the session teardown saves + stops everything, and
                // the outer loop shows the title screen again. A Multiplayer
                // action (Friends → Join while in-game) rides the same
                // teardown, then the outer loop consumes it as a pending
                // auto-join instead of showing the title.
                // Death screen "Respawn" → PERFORM_RESPAWN player action.
                // The server revives + teleports; its SetHealthS2C (health
                // back to 20) then closes the screen.
                if (Render::ConsumeDeathRespawnRequest()) {
                    playerController.SendPlayerAction(Network::PlayerAction::PERFORM_RESPAWN);
                    // MC calls startWaitingForNewLevel on respawn too — the
                    // server re-arms its own 60-tick wait in
                    // PlayerSession::Respawn, so the client has to re-report.
                    Client::g_levelLoadTracker.StartClientLoad();
                }

                Render::TitleAction pauseAction = Render::ConsumeTitleAction();
                if (pauseAction.kind == Render::TitleAction::Kind::QuitToTitle) {
                    Log::Info("Save and Quit to Title from pause menu");
                    returnToTitle = true;
                } else if (pauseAction.kind == Render::TitleAction::Kind::Multiplayer) {
                    Log::Info("Joining %s:%u from in-game (session handover)",
                              pauseAction.host.c_str(),
                              static_cast<unsigned>(pauseAction.port));
                    pendingSessionAction = pauseAction;
                    returnToTitle = true;
                } else if (pauseAction.kind == Render::TitleAction::Kind::Quit) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }
            // Chat system: open on T or /, route input when open
            else if (g_chatScreen.IsOpen()) {
                PROFILE_ZONE_N("ChatInput");
                // Mouse, so chat lines can be clicked. MC's chat is clickable
                // (ChatComponent.getClickedComponentStyleAt) and /seed leans on
                // it; without this the copy-on-click segment is inert.
                {
                    int winW2 = 0, winH2 = 0, fbW2 = 0, fbH2 = 0;
                    glfwGetWindowSize(window, &winW2, &winH2);
                    glfwGetFramebufferSize(window, &fbW2, &fbH2);
                    const float gScale = ComputeGuiScale(fbW2, fbH2, winW2);
                    auto [cmx, cmy] = Input::GetMousePosition();
                    if (winW2 > 0 && winH2 > 0 && gScale > 0.0f) {
                        g_chatComponent.SetMousePos(
                            static_cast<int>(cmx * (static_cast<double>(fbW2) / winW2) / gScale),
                            static_cast<int>(cmy * (static_cast<double>(fbH2) / winH2) / gScale));
                    }
                    static bool chatLmbHeld = false;
                    const bool chatLmb =
                        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    if (chatLmb && !chatLmbHeld) g_chatComponent.HandleClick();
                    chatLmbHeld = chatLmb;

                    // Pointing-hand cursor over a clickable chat component,
                    // as MC does.
                    SetChatPointerCursor(window, g_chatComponent.IsHoveringClickable());
                }

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
                // Up/down for history, Left/Right for cursor, Home/End for jump, Delete
                static bool upHeld = false, downHeld = false;
                static bool leftHeld = false, rightHeld = false;
                static bool homeHeld = false, endHeld = false, deleteHeld = false;
                bool upDown    = glfwGetKey(window, GLFW_KEY_UP)    == GLFW_PRESS;
                bool downDown  = glfwGetKey(window, GLFW_KEY_DOWN)  == GLFW_PRESS;
                bool leftDown  = glfwGetKey(window, GLFW_KEY_LEFT)  == GLFW_PRESS;
                bool rightDown = glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
                bool homeDown  = glfwGetKey(window, GLFW_KEY_HOME)  == GLFW_PRESS;
                bool endDown   = glfwGetKey(window, GLFW_KEY_END)   == GLFW_PRESS;
                bool deleteDown = glfwGetKey(window, GLFW_KEY_DELETE) == GLFW_PRESS;
                bool tabDown    = glfwGetKey(window, GLFW_KEY_TAB)    == GLFW_PRESS;
                static bool tabHeld = false;
                if (upDown    && !upHeld)    g_chatScreen.OnKeyDown(GLFW_KEY_UP);
                if (downDown  && !downHeld)  g_chatScreen.OnKeyDown(GLFW_KEY_DOWN);
                if (leftDown  && !leftHeld)  g_chatScreen.OnKeyDown(GLFW_KEY_LEFT);
                if (rightDown && !rightHeld) g_chatScreen.OnKeyDown(GLFW_KEY_RIGHT);
                if (homeDown  && !homeHeld)  g_chatScreen.OnKeyDown(GLFW_KEY_HOME);
                if (endDown   && !endHeld)   g_chatScreen.OnKeyDown(GLFW_KEY_END);
                if (deleteDown && !deleteHeld) g_chatScreen.OnKeyDown(GLFW_KEY_DELETE);
                if (tabDown && !tabHeld)     g_chatScreen.OnKeyDown(GLFW_KEY_TAB);
                upHeld = upDown; downHeld = downDown;
                leftHeld = leftDown; rightHeld = rightDown;
                homeHeld = homeDown; endHeld = endDown; deleteHeld = deleteDown;
                tabHeld = tabDown;

                g_chatScreen.Update(1.0f / 60.0f); // Approximate frame dt for cursor blink

                // Handle submitted message or command
                std::string submitted = g_chatScreen.ConsumeSubmittedMessage();

                // ── Client-side commands ───────────────────────────────────
                // /clearchat wipes only THIS player's chat view. It is handled
                // here rather than in the server dispatcher on purpose:
                //
                //  * there is no server state to change — the history lives in
                //    ChatComponent on the client, so a round trip would only
                //    buy a new S2C packet to send the answer back;
                //  * it therefore works when joined to someone else's server,
                //    which a server-side command could not be relied on to do.
                //
                // Vanilla has no equivalent (its closest relative,
                // ClientboundDeleteChatPacket, removes one specific message for
                // moderation), so there is no MC behaviour to mirror here.
                if (!submitted.empty()) {
                    std::string cmd = submitted;
                    // Accept "/clearchat" with any trailing whitespace/args.
                    const size_t sp = cmd.find_first_of(" \t");
                    if (sp != std::string::npos) cmd = cmd.substr(0, sp);
                    for (char& c : cmd) c = static_cast<char>(std::tolower(
                        static_cast<unsigned char>(c)));

                    if (cmd == "/clearchat") {
                        // "As if the game had just opened": drop the message
                        // list AND the up-arrow recall history, so nothing is
                        // left to scroll back to.
                        g_chatComponent.Clear();
                        g_chatScreen.ClearHistory();
                        submitted.clear();   // never reaches the server
                    }
                }

                if (!submitted.empty() && networkClient && networkClient->IsConnected()) {
                    auto conn = networkClient->GetConnection();
                    if (conn) {
                        // Send everything to server — server decides if it's a command or chat
                        // The ChatMessageC2SPacket.isCommand flag tells the server to route
                        // to the CommandDispatcher instead of broadcasting as chat.
                        conn->SendChatMessage(submitted);
                    }
                }

                // Drain char queue to prevent stale input
                // Skip player input — chat has focus
            } else if (Render::GetInventoryScreen().IsOpen()) {
                PROFILE_ZONE_N("InventoryInput");
                // ── Inventory screen overlay ───────────────────────────────────
                auto& inv = Render::GetInventoryScreen();

                // Char input → search box
                while (Input::HasCharInput()) inv.OnCharInput(Input::PopCharInput());

                // Edge-detect keys (E/ESC/Q close or drop, 1-9 swap, arrows/etc edit search).
                // NOTE: eHeld is the SHARED static defined just below the chain so that pressing
                // E to open the inventory in the game branch doesn't immediately retrigger
                // OnKeyDown(E) here on the next frame (which would close it).
                extern bool s_eKeyHeld;
                static bool escHeld=false, qHeld=false;
                static bool num1=false, num2=false, num3=false, num4=false,
                            num5=false, num6=false, num7=false, num8=false, num9=false;
                static bool ileftH=false, irightH=false, ihomeH=false, iendH=false,
                            ibsH=false, idelH=false;
                int mods = ((glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)   == GLFW_PRESS) ||
                            (glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT)  == GLFW_PRESS)) ? GLFW_MOD_SHIFT : 0;
                mods |= ((glfwGetKey(window, GLFW_KEY_LEFT_CONTROL)    == GLFW_PRESS) ||
                         (glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL)   == GLFW_PRESS)) ? GLFW_MOD_CONTROL : 0;

                auto edge = [&](bool& held, int key, int glfwKey) {
                    bool down = glfwGetKey(window, key) == GLFW_PRESS;
                    if (down && !held) inv.OnKeyDown(glfwKey, mods);
                    held = down;
                };
                edge(s_eKeyHeld, GLFW_KEY_E,    GLFW_KEY_E);
                edge(escHeld,  GLFW_KEY_ESCAPE, GLFW_KEY_ESCAPE);
                edge(qHeld,    GLFW_KEY_Q,      GLFW_KEY_Q);
                edge(num1, GLFW_KEY_1, GLFW_KEY_1);
                edge(num2, GLFW_KEY_2, GLFW_KEY_2);
                edge(num3, GLFW_KEY_3, GLFW_KEY_3);
                edge(num4, GLFW_KEY_4, GLFW_KEY_4);
                edge(num5, GLFW_KEY_5, GLFW_KEY_5);
                edge(num6, GLFW_KEY_6, GLFW_KEY_6);
                edge(num7, GLFW_KEY_7, GLFW_KEY_7);
                edge(num8, GLFW_KEY_8, GLFW_KEY_8);
                edge(num9, GLFW_KEY_9, GLFW_KEY_9);
                edge(ileftH,  GLFW_KEY_LEFT,      GLFW_KEY_LEFT);
                edge(irightH, GLFW_KEY_RIGHT,     GLFW_KEY_RIGHT);
                edge(ihomeH,  GLFW_KEY_HOME,      GLFW_KEY_HOME);
                edge(iendH,   GLFW_KEY_END,       GLFW_KEY_END);
                edge(ibsH,    GLFW_KEY_BACKSPACE, GLFW_KEY_BACKSPACE);
                edge(idelH,   GLFW_KEY_DELETE,    GLFW_KEY_DELETE);

                // Mouse position: feed in window-pixel coords + GUI virtual size.
                // Use the same GUI-scale formula as the render path (line 167).
                auto [mx, my] = Input::GetMousePosition();
                int winW = 0, winH = 0, fbW = 0, fbH = 0;
                glfwGetWindowSize(window, &winW, &winH);
                glfwGetFramebufferSize(window, &fbW, &fbH);
                if (fbW > 0 && fbH > 0 && winW > 0) {
                    float invGuiScale = ComputeGuiScale(fbW, fbH, winW);
                    int   guiWp = static_cast<int>(static_cast<float>(fbW) / invGuiScale);
                    int   guiHp = static_cast<int>(static_cast<float>(fbH) / invGuiScale);
                    // mx/my are in WINDOW (logical) pixels — convert via window→GUI scale.
                    inv.OnMouseMove(mx, my, winW, winH, guiWp, guiHp);
                }

                // Edge-detect mouse buttons (Input.cpp doesn't register a glfwSetMouseButtonCallback)
                static bool lmbH=false, rmbH=false, mmbH=false;
                bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT)   == GLFW_PRESS;
                bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT)  == GLFW_PRESS;
                bool mmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
                // These latches are stale while no screen is up, so a screen
                // that opens with a button already down — the right-click that
                // opened a crafting table — would see the very next diff as a
                // fresh press and land it on whatever slot is under the cursor.
                // Adopt the live state instead, delivering nothing.
                if (inv.ConsumeFreshlyOpened()) { lmbH = lmb; rmbH = rmb; mmbH = mmb; }
                if (lmb != lmbH) inv.OnMouseButton(GLFW_MOUSE_BUTTON_LEFT,   lmb ? GLFW_PRESS : GLFW_RELEASE, mods);
                if (rmb != rmbH) inv.OnMouseButton(GLFW_MOUSE_BUTTON_RIGHT,  rmb ? GLFW_PRESS : GLFW_RELEASE, mods);
                if (mmb != mmbH) inv.OnMouseButton(GLFW_MOUSE_BUTTON_MIDDLE, mmb ? GLFW_PRESS : GLFW_RELEASE, mods);
                lmbH = lmb; rmbH = rmb; mmbH = mmb;

                // Scroll wheel: route to inventory and consume so the hotbar doesn't move.
                auto [sx, sy] = Input::GetScrollOffset();
                if (sy != 0.0) inv.OnScroll(sy);
                Input::ResetScrollOffset();

                inv.Update(1.0f / 60.0f);

                // Drain pending clicks → server.
                Network::InventoryClickC2SPacket click;
                while (inv.ConsumePendingClick(click)) {
                    if (networkClient && networkClient->IsConnected()) {
                        auto conn = networkClient->GetConnection();
                        if (conn) {
                            // 0xFF action = our local sentinel for "send InventoryCloseC2S".
                            if (click.action == 0xFF) {
                                Network::InventoryCloseC2SPacket close{};
                                auto data = Network::Serialization::Serialize(close);
                                conn->SendPacket(static_cast<uint8_t>(Network::PacketId::InventoryCloseC2S), data);
                            } else {
                                auto data = Network::Serialization::Serialize(click);
                                conn->SendPacket(static_cast<uint8_t>(Network::PacketId::InventoryClickC2S), data);
                            }
                        }
                    }
                }
            } else {
                PROFILE_ZONE_N("GameKeybinds");
                // Chat just lost focus — drop any pointing-hand cursor it left
                // set, or it would persist into gameplay.
                SetChatPointerCursor(window, false);

                // Drain char queue when chat is closed (prevent buildup)
                while (Input::HasCharInput()) Input::PopCharInput();

                // Open chat on T or /
                if (Input::ConsumeClick(*Input::Binds::Chat)) {
                    g_chatScreen.Open(false);
                } else if (Input::ConsumeClick(*Input::Binds::Command)) {
                    g_chatScreen.Open(true);
                }

                // The inventory binding opens the inventory (MC key.inventory,
                // default E). The inventory branch still edge-detects the raw
                // key to CLOSE, so mark that latch held here — otherwise the
                // same physical press would immediately close what it opened.
                extern bool s_eKeyHeld;
                if (Input::ConsumeClick(*Input::Binds::Inventory)) {
                    // Survival panel or creative picker, depending on game mode
                    // (MC InventoryScreen.init hands over to
                    // CreativeModeInventoryScreen for infinite-materials players).
                    Render::OpenInventoryScreen();
                    s_eKeyHeld = true;
                }

                // ESC opens the pause menu (MC Game Menu). Uses the shared
                // edge computed above so an ESC that just closed chat or the
                // inventory can't also open the pause menu.
                if (escPressedThisFrame) {
                    Render::GetScreenManager().Push(std::make_unique<Render::PauseScreen>());
                }

            }

            // ── Screen-state bookkeeping ─────────────────────────────────
            // OUTSIDE the branch chain above, and it has to stay that way.
            // MC runs this from Minecraft.tick, not from handleKeybinds, and
            // for good reason: a screen can open without the gameplay branch
            // ever running that frame. A crafting table does exactly that — the
            // SERVER opens it, so the frame it appears the chain takes the
            // "a screen is open" branch and none of this would run.
            //
            // When it lived in the else branch, that left the crafting screen
            // with uiActive still false and HandlePlayerInput never called, so
            // held-RMB kept re-firing placements behind the open panel and the
            // E that closed it was still queued on the inventory binding —
            // which the gameplay branch then consumed the next frame, popping
            // the inventory open on top.
            //
            // Two DIFFERENT questions, deliberately kept apart.
            //
            //  • screenOpen   — MC's `minecraft.screen != null`. A real UI
            //    owns the keyboard: presses belong to it, not to the world.
            //  • cursorVisible — that, OR the Tab manual cursor. The world
            //    is still running and nothing owns the keyboard; the mouse
            //    is just free so you can poke at debug windows.
            //
            // Conflating them is what broke Tab: routing keys to the UI
            // whenever the cursor was up meant the Tab that RAISED the
            // cursor turned every later Tab into a UI keypress with no
            // screen to receive it, so the binding could never fire again
            // and you were stuck in cursor mode.
            //
            // GLFW reflects the manual toggle's last-frame state; the
            // screen flags are OR'd in so the very frame an overlay pops,
            // WASD input is already dropped.
            const bool screenOpen = g_chatScreen.IsOpen() ||
                                    Render::GetInventoryScreen().IsOpen() ||
                                    !Render::GetScreenManager().Empty();
            const bool cursorVisible = screenOpen ||
                (glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_NORMAL);

            // Screen open/close, handled the way MC does it.
            //
            //  • OPEN  — Minecraft.setScreen:1123 calls KeyMapping.releaseAll(),
            //    dropping every queued click and clearing held state, and
            //    Minecraft.tick:1779 pins missTime to 10000 for as long as a
            //    screen is up so no attack can fire out of a UI frame.
            //  • CLOSE — MouseHandler.grabMouse:398 calls KeyMapping.setAll(),
            //    which restores KEYBOARD state only. Mouse buttons stay up on
            //    purpose, so the click that dismissed the screen cannot act on
            //    the world. Input::RestoreKeyboardState mirrors both halves.
            //
            // Input::SetUiActive is the `minecraft.screen != null` check that
            // the GLFW callbacks themselves consult, so a press landing while
            // a screen is up is never recorded as gameplay input at all.
            // screenOpen, NOT cursorVisible — the Tab cursor is not a
            // screen, and MC has no equivalent of it. Gameplay input is
            // still fully suppressed while it's up, but by
            // HandlePlayerInput below (which zeroes movement and drains
            // attack/use), so bindings keep working the whole time.
            Input::SetUiActive(screenOpen);
            {
                static bool s_prevUiActive = false;
                if (screenOpen && !s_prevUiActive) {
                    Input::ReleaseAll();
                } else if (!screenOpen && s_prevUiActive) {
                    Input::RestoreKeyboardState();
                }
                s_prevUiActive = screenOpen;
            }
            // MC Minecraft.tick:1778-1780 — refreshed every frame a screen
            // is open; ContinueAttack(false) clears it once the button is up.
            if (cursorVisible) playerController.SetMissTime(10000);

            // Drains attack/use and stops an in-progress break/place whenever
            // the cursor is up, which is what tears down held-RMB when a screen
            // takes over.
            { PROFILE_ZONE_N("PlayerInput");
            HandlePlayerInput(player, playerController, camera, cursorVisible);
            }

            // Resolve cursor state AFTER chat/inventory/pause handling so
            // opening/closing this frame takes effect immediately.
            { PROFILE_ZONE_N("CursorToggle");
            cursorEnabled = HandleCursorToggle(window, camera,
                g_chatScreen.IsOpen() || Render::GetInventoryScreen().IsOpen() ||
                !Render::GetScreenManager().Empty());
            }

            PROFILE_TIMER_END(input, metrics.inputHandlingTime);
            }

            // Friend invites → chat notification (the Friends screen also
            // shows the latest invite as a joinable banner row).
            if (Client::g_friendsClient) {
                for (const auto& invite : Client::g_friendsClient->ConsumeInvites()) {
                    g_chatComponent.AddMessage(
                        invite.fromName + " invited you to '" + invite.world +
                        "' - press Esc > Friends to join");
                }
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

                // MC ClientPacketListener.tick: levelLoadTracker.tickClientLoad()
                // then notifyPlayerLoaded() once the level is ready. Runs right
                // after the packet drain, as it does there — the chunk that
                // makes us ready may have arrived in the drain above.
                Client::g_levelLoadTracker.Tick(player.physics.position);

                // (Mesh scheduling used to live here, in the 20 Hz tick. It is a
                // FRAME phase now — see the MeshSchedule block next to MeshUpload
                // below. Back when a pass could start at most permits.Available()
                // sections, calling it at tick rate hard-capped meshing at
                // 20 x permits, which is exactly what traces showed while the
                // mesh workers sat 1.6% busy. The pass is no longer capped by
                // the permit pool at all, but it still belongs on the frame.)

                // 2b. MC Minecraft.tick line 1742: level.tickRateManager().tick(),
                //     BEFORE anything reads runsNormally(). It is also what
                //     counts a /tick step down, so the step ends on its own.
                Client::g_clientTickRate.Tick();

                // 2c. The local player's own 20 Hz tick (MC Player.tick). Its
                //     physics half runs per frame; the attack cooldown that
                //     feeds the crosshair indicator has to count in TICKS, or
                //     the bar fills at frame rate.
                player.Tick();

                // 3. Interpolate remote player positions (Minecraft's InterpolationHandler)
                if (Client::g_remotePlayerManager) {
                    Client::g_remotePlayerManager->Tick();
                }

                // MC ClientLevel.tickEntities skips every entity for which
                // tickRateManager.isEntityFrozen() holds — which under /tick
                // freeze is everything except players. Client-side mobs run
                // their own animation clock (that is what smooths them between
                // position packets), so without this they keep walking and
                // swinging in a world the server has stopped simulating.
                //
                // Remote PLAYERS above are deliberately outside the gate:
                // TickRateManager.isEntityFrozen exempts Player, which is why
                // you can still walk around a frozen world in vanilla.
                const bool entitiesFrozen = Client::g_clientTickRate.IsEntityFrozen();

                if (Client::g_itemEntityManager && !entitiesFrozen) {
                    // Feet position, for pickup animations to fly toward.
                    Client::g_itemEntityManager->Tick(player.predictedPos);
                }

                // Mobs run the same physics classes the server does, so
                // they need the block view and the world clock before they
                // tick — otherwise they fall through the world between
                // position packets. The setters run even while frozen so a
                // resumed mob is not looking at a stale world.
                if (Client::g_clientMobManager) {
                    Client::g_clientMobManager->SetBlockAccess(Client::g_clientBlockAccess);
                    Client::g_clientMobManager->SetTime(
                        Render::EnvironmentState::Get().GameTime(),
                        Render::EnvironmentState::Get().DayTime());
                    if (!entitiesFrozen) Client::g_clientMobManager->Tick();
                }

                // 3b. Advance local world time (ClientLevel.tickTime mirror —
                //     smooth day/night between the server's 20-tick syncs).
                //     Gated exactly as MC gates it (ClientLevel.tick:269,
                //     `if (tickRateManager().runsNormally()) { … tickTime(); }`):
                //     the server stops advancing dayTime while frozen, so
                //     without this the sky would keep sliding toward a sunset
                //     the world never reaches, then snap back on the next
                //     TimeUpdate.
                if (Client::g_clientTickRate.RunsNormally()) {
                    Render::EnvironmentState::Get().TickClient();
                }

                // Screen 20Hz tick (death-screen button delay, caret blink).
                // The title phase has its own pump; in-game screens only got
                // Update/Render before this.
                Render::GetScreenManager().Tick();

                // 4. Send player position to server (one packet per tick = 20 Hz)
                if (networkClient->IsConnected()) {
                    glm::vec3 playerPos = player.physics.position;
                    Network::PlayerMoveC2SPacket movePacket;
                    movePacket.position = playerPos;
                    movePacket.rotation = glm::vec2(camera.yaw, camera.pitch);
                    movePacket.onGround = player.physics.isOnGround;
                    movePacket.isCrouching = Input::IsDown(*Input::Binds::Sneak);
                    movePacket.isSprinting = player.physics.isSprinting;
                    movePacket.jumpedThisTick = player.jumpedSinceMoveSend;
                    player.jumpedSinceMoveSend = false;
                    movePacket.fallDistance = player.landedFallSinceMoveSend;
                    player.landedFallSinceMoveSend = 0.0f;
                    movePacket.sequenceNumber = ++playerMoveSequence;
                    movePacket.timestamp = std::chrono::steady_clock::now();
                    networkClient->GetConnection()->SendPlayerMove(movePacket);
                }

                // 5. Held-item viewmodel: per-tick state advance (equip
                //    swap detection, swing timer, etc). Rising-edge of
                //    the attack button drives the swing animation; we
                //    track previous state across ticks here.
                {
                    static bool s_prevLmbHeld = false;
                    static bool s_prevRmbHeld = false;
                    // Vanilla triggers the swing on:
                    //   • LMB rising edge — every attack swings
                    //   • RMB rising edge ONLY when the click would
                    //     actually place a block (held item is a block
                    //     item AND we're aimed at a placeable surface).
                    //     Right-clicking with a tool / food / empty
                    //     hand etc. doesn't swing.
                    //
                    // Any open UI suppresses both. The check covers the pause
                    // menu and chat as well as the inventory — it used to look
                    // at the inventory alone, so dismissing the pause menu with
                    // a click threw a phantom swing.
                    //
                    // Held state comes from Input's event-driven layer, which
                    // only records presses that happened with no UI active — so
                    // a click that dismissed a screen can't produce a swing
                    // here either. The edge latches stay because the swing is a
                    // visual pulse rather than an action; the clicks themselves
                    // were already consumed by the break/place path above.
                    // The Tab manual cursor counts here as well as real screens.
                    // Bindings stay live while it's up (that's how Tab itself
                    // gets you back out), so held state is genuinely set when
                    // you click an ImGui window — and without this gate that
                    // would throw a phantom swing. The click itself is already
                    // harmless: HandlePlayerInput drains attack/use whenever the
                    // cursor is visible, for either reason.
                    const bool uiHoldsCursor =
                        Render::GetInventoryScreen().IsOpen() ||
                        g_chatScreen.IsOpen() ||
                        !Render::GetScreenManager().Empty() ||
                        glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_NORMAL;
                    const bool lmbHeld = !uiHoldsCursor && Input::IsDown(*Input::Binds::Attack);
                    const bool rmbHeld = !uiHoldsCursor && Input::IsDown(*Input::Binds::Use);
                    const bool lmbEdge = lmbHeld && !s_prevLmbHeld;
                    const bool rmbEdge = rmbHeld && !s_prevRmbHeld;
                    s_prevLmbHeld = lmbHeld;
                    s_prevRmbHeld = rmbHeld;

                    // RMB → swing only if the click will place a block.
                    bool placeEdge = false;
                    if (rmbEdge) {
                        const Game::ItemID held = player.inventory.GetSelectedItem();
                        if (held != Game::ItemID(0) &&
                            player.lastBlockHit.has_value() &&
                            Game::ItemRegistry::Get(held).renderType
                                == Game::ItemRenderType::Block) {
                            placeEdge = true;
                        }
                    }

                    // Held-action swing pump: PlayerController flags a swing
                    // each time continuous mining ticks (MC's continueDestroyBlock
                    // cadence) AND each time held-RMB re-fires a placement
                    // (every PLACE_REFIRE_TICKS). Without the place-side flag
                    // the arm only swung on the initial RMB edge, not on
                    // subsequent placements in a contiguous strip.
                    const bool armSwing = playerController.ConsumeMiningSwingTrigger();

                    // View angles for the held-item sway. These are already
                    // MC's convention now, so they go straight through — the
                    // pitch used to be negated here to undo the camera's old
                    // positive-up convention.
                    Render::g_heldItemRenderer.Tick(
                        player.inventory.GetSelectedItem(),
                        player.inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN).itemId,
                        lmbEdge || placeEdge || armSwing,
                        // MC ItemInHandRenderer.tick:564 — getItemSwapScale(1.0F).
                        player.GetItemSwapScale(1.0f),
                        camera.pitch, camera.yaw);
                }

                nextClientTick += CLIENT_TICK_INTERVAL;
                ticksThisFrame++;
            }
            }

            // === PER-FRAME: Game logic (variable dt for smooth rendering) ===
            float dt;
#if ENABLE_PORTAL_GUN
            // Hoisted OUTSIDE the GameLogic profile scope so the render
            // code further down (which is also outside the scope) can
            // read it. Set when the predicted teleport snap fires THIS
            // frame, so the render code below can suppress the local-
            // player ghost / see-through body for one frame. Without
            // this gate, the virtual camera for the see-through pass —
            // computed by applying SrcToDst to the (now post-teleport)
            // real camera — lands at a doubly-transformed location,
            // and the local body rendered at the new player position
            // via that virtCam briefly shows up at a garbage screen
            // position before the next frame's render uses a fresh
            // camera state. Visible symptom: "my player model shows
            // up in front of me and flickers for a frame on teleport."
            bool justTeleportedThisFrame = false;
#endif
            { PROFILE_ZONE_N("GameLogic");
            PROFILE_TIMER_START(gamelogic);
            Time::Tick();
            dt = static_cast<float>(Time::Delta());
            metrics.AddFrameTimeSample(dt * 1000.0f);

            // Capture pre-physics state for the portal prediction below.
            // Critically, prevVel is the velocity BEFORE the physics
            // step's collision snap might have zeroed it — for fast
            // falls (>1 m/frame, i.e. > ~62 m/s) the player AABB can
            // pass through the 1 m thick portal block in a single frame
            // and trigger the snap on the SOLID block below the portal.
            // Without saving prevVel, the trigger would then apply M to
            // a zeroed velocity → player exits the destination at 0 m/s
            // → infinite-fall acceleration restarts from scratch.
#if ENABLE_PORTAL_GUN
            const glm::vec3 prevEye = player.GetEyePosition();
            const glm::vec3 prevVel = player.physics.velocity;
#endif

            // Dead players are frozen — MC's corpse is immobile and the
            // server drops our move packets anyway, so keep the body exactly
            // where it died until PERFORM_RESPAWN teleports it. (health is
            // the server-synced value; respawn restores it to 20 and physics
            // resumes.)
            if (player.health <= 0) {
                player.physics.velocity = glm::vec3(0.0f);
            } else {
                player.UpdatePhysics(dt, blockAccessForPhysics);
            }

#if ENABLE_PORTAL_GUN
            // Client-side teleport prediction. Server still detects
            // independently (its own crossing test on the next 50 ms
            // tick) — when its packet arrives the position will already
            // match, so the snap is invisible. This eliminates the
            // ~50 ms (3 frames at 60 fps) gap where the camera was past
            // the source plane and the portal mesh stopped drawing.
            {
                const glm::vec3 currEye = player.GetEyePosition();
                auto pred = Client::GetClientPortalManager().CheckEyeCrossing(
                    prevEye, currEye, player.physics.position,
                    prevVel, camera.yaw, camera.pitch,
                    player.physics.GetCurrentHeight(),
                    Game::PlayerPhysics::WIDTH * 0.5f);
                if (pred.valid) {
                    player.physics.position = pred.newFeet;
                    player.physics.velocity = pred.newVelocity;
                    camera.yaw              = pred.newYawDeg;
                    camera.pitch            = pred.newPitchDeg;
                    player.predictedPos     = glm::dvec3(pred.newFeet);
                    player.serverPos        = glm::dvec3(pred.newFeet);
                    player.visualPos        = glm::dvec3(pred.newFeet);
                    justTeleportedThisFrame = true;
                    Log::Info("[PortalPredict] Client predicted teleport "
                              "to (%.2f,%.2f,%.2f) yaw %.1f pitch %.1f",
                              pred.newFeet.x, pred.newFeet.y, pred.newFeet.z,
                              pred.newYawDeg, pred.newPitchDeg);
                }
            }
#endif

            camera.position = player.GetEyePosition();
            camera.Update(dt);
            player.UpdateRaycast(camera);
            playerController.Tick(dt);

            // === Speed-driven FOV ==========================================
            // MC AbstractClientPlayer.getFieldOfViewModifier (line 103):
            //   modifier  = 1
            //   if flying          → modifier *= 1.1
            //   speedFactor        = MOVEMENT_SPEED / walkingSpeed
            //   modifier *= (speedFactor + 1) / 2
            //   modifier  = lerp(fovEffectScale, 1, modifier)
            // Our currentSpeed is the direct analogue of the MOVEMENT_SPEED
            // attribute (walk/sprint plus the consecutive-jump bonus), so
            // dividing by WALK_SPEED reproduces MC's ratio exactly: walking
            // gives 1.0 (no change) and sprinting 1.3 → ×1.15.
            //
            // This holds ONLY because sneaking is kept out of currentSpeed.
            // MC drives sneak through a separate mechanism — an input scale
            // via Attributes.SNEAKING_SPEED in LocalPlayer.modifyInput, not a
            // MOVEMENT_SPEED modifier — so vanilla sits at exactly 1.0 while
            // crouched. Folding sneak back into currentSpeed (UpdateBaseSpeed)
            // would make this ratio drop below 1 and zoom the FOV *in* while
            // shifting, which is a bug, not a feature.
            {
                float target = 1.0f;
                if (player.physics.isFlying) target *= 1.1f;
                const float speedFactor =
                    player.physics.currentSpeed / Game::PlayerPhysics::WALK_SPEED;
                target *= (speedFactor + 1.0f) * 0.5f;

                const float effectScale = Platform::g_gameSettings.GetFOVEffectScale();
                target = 1.0f + (target - 1.0f) * effectScale;

                // MC advances this once per client tick with a fixed 0.5
                // blend; run the same discrete step so the ease-in duration
                // doesn't drift with framerate.
                fovTickAccum += dt;
                while (fovTickAccum >= 0.05) {
                    fovModifierOld = fovModifier;
                    fovModifier   += (target - fovModifier) * 0.5f;
                    fovModifier    = std::clamp(fovModifier, 0.1f, 1.5f);
                    fovTickAccum  -= 0.05;
                }

                // Sub-tick interpolation (MC getFov's lerp(partialTicks, …)).
                const float partial = static_cast<float>(fovTickAccum / 0.05);
                camera.fov *= fovModifierOld + (fovModifier - fovModifierOld) * partial;
            }
            player.UpdateVisual(dt);
            player.UpdateStatistics(dt);
            PROFILE_TIMER_END(gamelogic, metrics.gameLogicTime);
            }

            // === F5 third-person render camera ==============================
            // The LOGICAL camera (raycast, interaction, move packets) stays
            // at the eye — everything above already consumed it. For the
            // render phase we now derive the detached camera: ThirdFront
            // flips the view (yaw+180, pitch negated), then the camera pulls
            // back up to 4 blocks along -forward with MC Camera.getMaxZoom's
            // 8-corner jittered raycast so it never clips into walls. The
            // original eye state is restored at the end of the frame (mouse
            // look accumulates on yaw/pitch, so ThirdFront's flip must not
            // leak into the next frame).
            const bool tpActive = !camera.IsFirstPerson();
            const glm::vec3 tpSavedPos  = camera.position;
            const float     tpSavedYaw  = camera.yaw;
            const float     tpSavedPitch = camera.pitch;
            if (tpActive) {
                if (camera.perspective == Render::Perspective::ThirdFront) {
                    camera.yaw   = tpSavedYaw + 180.0f;
                    camera.pitch = -tpSavedPitch;
                }
                const glm::vec3 back = -camera.GetForward();
                float maxZoom = 4.0f;                     // MC DEFAULT_CAMERA_DISTANCE
                for (int i = 0; i < 8; ++i) {             // MC Camera.getMaxZoom
                    const glm::vec3 off(((i & 1) * 2 - 1) * 0.1f,
                                        ((i >> 1 & 1) * 2 - 1) * 0.1f,
                                        ((i >> 2 & 1) * 2 - 1) * 0.1f);
                    if (auto hit = Game::Raycast::CastRay(tpSavedPos + off, back, maxZoom)) {
                        const float d = glm::length(hit->hitPoint - tpSavedPos);
                        if (d < maxZoom) maxZoom = d;
                    }
                }
                camera.position = tpSavedPos + back * maxZoom;
            }

            // === PER-FRAME: Set player position for mesh prioritization ===
            glm::vec3 playerPos = player.physics.position;
            Render::SetClientMeshPlayerPosition(playerPos);

            // 6b. Schedule mesh builds — a FRAME phase, like MC's
            // LevelRenderer compiling sections every frame.
            //
            // Moved out of the 20 Hz ClientTick: back when a pass was capped by
            // the permit pool, tick-rate scheduling capped meshing at 20 x
            // permits no matter how idle the mesh workers were. The server was
            // delivering ~570 sections/s, so the client fell steadily behind and
            // chunks appeared long after they had arrived.
            //
            // This pass is now ADMISSION ONLY — it feeds the compile queue and
            // is not throttled by the permit pool. Rate limiting lives with the
            // mesh workers, which take a permit when they start a job, so
            // throughput no longer scales with frame rate. Do not re-add a cap
            // here; see the comment on ScheduleMeshBuildsWithSnapshots.
            { PROFILE_ZONE_N("MeshSchedule");
            PROFILE_TIMER_START(meshsched);
            Render::ScheduleClientMeshBuilds(player.physics.position);
            PROFILE_TIMER_END(meshsched, metrics.meshSchedulingTime);
            }

            // 7. Perform GPU uploads
            // Frame phase. The cost you see here is only the CPU side of
            // handing data to the driver — the transfer itself is paid later,
            // in Present. Watch the UploadBytes plot, not this zone's width.
            { PROFILE_ZONE_N("MeshUpload");
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

            // Get framebuffer size — needed for viewport.
            glfwGetFramebufferSize(window, &width, &height);

            // Update the environment state (time-of-day colors, fog, sun/moon
            // angles) BEFORE clearing — the clear color is the fog color.
            int effectiveRenderDist = Platform::g_gameSettings.GetRenderDistance();
            if (Client::g_networkClient && Client::g_networkClient->GetServerViewDistance() > 0) {
                effectiveRenderDist = std::min(effectiveRenderDist, Client::g_networkClient->GetServerViewDistance());
            }
            {
                const auto nowForPartial = std::chrono::steady_clock::now();
                const float remaining =
                    std::chrono::duration<float>(nextClientTick - nowForPartial).count();
                const float tickSeconds =
                    std::chrono::duration<float>(CLIENT_TICK_INTERVAL).count();
                const float envPartialTick =
                    std::clamp(1.0f - remaining / tickSeconds, 0.0f, 1.0f);
                Render::EnvironmentState::Get().UpdateFrame(
                    envPartialTick, camera.GetForward(), camera.position.y,
                    effectiveRenderDist, Platform::g_gameSettings.GetFogEnabled());
            }
            const glm::vec3 clearColor = Render::EnvironmentState::Get().Frame().fogColor;

            // Clear framebuffer via render backend.
            // Stencil is cleared too — the portal renderer (Phase 6+) reads
            // stencil values to gate per-portal scene re-renders, and any
            // leftover bits from the previous frame would silently mask the
            // wrong region. The stencil clear is a no-op cost when no
            // portals are visible.
            if (Render::g_renderBackend) {
                Render::g_renderBackend->SetClearColor(clearColor.r, clearColor.g, clearColor.b, 1.0f);
                Render::g_renderBackend->Clear(true, true, true);
            } else {
                glClearColor(clearColor.r, clearColor.g, clearColor.b, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            }

            // Set viewport.
            if (Render::g_renderBackend) {
                Render::g_renderBackend->SetViewport(0, 0, width, height);
            }
            float aspect = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);

            float farPlane = static_cast<float>(effectiveRenderDist) * 16.0f * 4.0f;
            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.05f, farPlane);

            // MC GameRenderer.bobHurt — the damage tilt and death spin, set on
            // the camera BEFORE anything reads the view matrix (the chunk
            // renderer takes its own copy from the same camera).
            {
                const float remaining = std::chrono::duration<float>(
                    nextClientTick - std::chrono::steady_clock::now()).count();
                const float tickSeconds =
                    std::chrono::duration<float>(CLIENT_TICK_INTERVAL).count();
                const float partialTickView =
                    std::clamp(1.0f - remaining / tickSeconds, 0.0f, 1.0f);
                camera.viewTilt = Render::Camera::MakeViewTilt(
                    player.hurtTime, player.hurtDuration, player.hurtDir,
                    player.health <= 0, player.deathTime, partialTickView);
                // The hand rides the same pose in vanilla.
                Render::g_heldItemRenderer.SetViewTilt(camera.viewTilt);
            }

            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 viewProj = proj * view;
            frustum = Frustum::FromMatrix(viewProj);

            // Sky pass (MC addSkyPass): camera-centered, before terrain.
            // Own projection — the 512-radius sky disc would be clipped by
            // the main far plane at low render distances.
            {
                PROFILE_ZONE_N("SkyPass");
                glm::mat4 skyProj = glm::perspective(glm::radians(camera.fov), aspect, 0.05f, 2048.0f);
                glm::mat4 viewRotation = glm::mat4(glm::mat3(view));
                Render::g_skyRenderer.Render(skyProj, viewRotation);
            }

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

            // Render remote players (stick figures) before UI overlays.
            //
            // partialTick = how far through the current 50ms client tick we are.
            // 0.0 = just ticked, 1.0 = about to tick again. After the tick
            // catch-up loop above, `nextClientTick` points at the NEXT tick
            // boundary, so the time since the last tick = TICK - (nextTick - now).
            // Mirrors MC's `Minecraft.getDeltaTracker().getGameTimeDeltaPartialTick()`.
            // The renderer uses this to lerp each remote player's render
            // position between its previous-tick snapshot and current value
            // (see RemotePlayer::renderPrev*) → smooth motion at any FPS.
            // True iff the body bounding-extent actually crosses the
            // given (entry) clip plane. GetStraddlingGhost.valid alone
            // fires for any portal within 1m of the body center —
            // which is too loose for the half-body split: a player
            // standing fully on one side of a portal that happens to
            // face the wrong way would otherwise get clipped out
            // entirely by the entry plane test, making them invisible
            // when viewed through the other portal. We only want the
            // entry/exit split when the body geometrically straddles
            // the plane. Hoisted to the outer render-loop scope so it
            // can be used both by the main-scene ghost paths and by
            // the see-through callback below.
            auto BodyStraddlesEntryPlane = [](const glm::vec3& pos,
                                              const glm::vec4& plane,
                                              float height) {
                const glm::vec3 n = glm::vec3(plane);
                const float sdF = glm::dot(pos, n) + plane.w;          // feet
                const float sdH = sdF + height * n.y;                   // head
                return (sdF * sdH) <= 0.0f;
            };
            if (Client::g_remotePlayerManager) {
                PROFILE_ZONE_N("RemotePlayers");
                const auto nowForPartial = std::chrono::steady_clock::now();
                const float remaining =
                    std::chrono::duration<float>(nextClientTick - nowForPartial).count();
                const float tickSeconds =
                    std::chrono::duration<float>(CLIENT_TICK_INTERVAL).count();
                // MC DeltaTracker.Timer.getGameTimeDeltaPartialTick(false)
                // returns exactly 1.0F while the game is frozen. That single
                // line is what makes /tick freeze look frozen: an entity
                // renders at lerp(partialTick, prevTickPos, pos), and prevTick
                // still holds the position from BEFORE the last tick that ran.
                // Letting partialTick keep cycling 0..1 would replay that last
                // tick's movement forever, so a frozen mob would sit there
                // twitching back and forth once every 50 ms.
                const float partialTick =
                    Client::g_clientTickRate.IsEntityFrozen()
                        ? 1.0f
                        : std::clamp(1.0f - remaining / tickSeconds, 0.0f, 1.0f);
#if ENABLE_PORTAL_GUN
                // Phase G (remote): for each remote player straddling an
                // active portal pair we need BOTH halves drawn — entry-clipped
                // body on the source side, exit-clipped ghost on the
                // destination side. Skip them in the bulk pass below and
                // re-render each individually so the per-player clip
                // planes can be applied.
                std::unordered_set<uint32_t> straddlingIds;
                {
                    auto& portals = Client::GetClientPortalManager();
                    for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                        const glm::vec3 renderPosCheck {
                            glm::mix(rp.renderPrevPosition.x, rp.position.x, partialTick),
                            glm::mix(rp.renderPrevPosition.y, rp.position.y, partialTick),
                            glm::mix(rp.renderPrevPosition.z, rp.position.z, partialTick),
                        };
                        auto gC = portals.GetStraddlingGhost(renderPosCheck, 1.8f);
                        if (gC.valid && BodyStraddlesEntryPlane(renderPosCheck,
                                                                gC.entryClipPlane, 1.8f)) {
                            straddlingIds.insert(id);
                        }
                    }
                }
                playerRenderer.Render(proj, view, camera.position,
                                      *Client::g_remotePlayerManager,
                                      partialTick, &straddlingIds);
#else
                playerRenderer.Render(proj, view, camera.position,
                                      *Client::g_remotePlayerManager, partialTick);
#endif
                Client::g_remotePlayerManager->UpdateBubbles(dt);

                // Dropped items, drawn with the same partialTick the remote
                // players use so everything in the world moves on one clock.
                itemEntityRenderer.Render(proj, view, camera.position, partialTick);
                if (Client::g_clientMobManager) {
                    mobRenderer.Render(proj, view, camera.position,
                                       *Client::g_clientMobManager, partialTick);
                }

                // Third person: the local player becomes visible (MC renders
                // the camera entity when the camera is detached). Uses the
                // SAVED eye yaw/pitch — the ThirdFront flip is a camera-only
                // transform; the body still faces where the player looks.
                if (tpActive) {
                    playerRenderer.RenderSingle(
                        proj, view, glm::vec3(player.visualPos),
                        tpSavedYaw, tpSavedYaw, tpSavedPitch,
                        player.physics.isSneaking,
                        static_cast<uint8_t>(player.color),
                        glm::mat4(1.0f), glm::vec4(0.0f),
                        // Your own corpse topples too — MC renders the camera
                        // entity like any other while the camera is detached.
                        Render::MobRenderer::DeathFlipDegrees(player.deathTime,
                                                              partialTick));
                }

#if ENABLE_PORTAL_GUN
                // Ghost rendering — for each remote player straddling an
                // active portal pair, draw BOTH halves:
                //   • Entry-clipped body on the source side (so the half
                //     that's already passed through the portal isn't drawn)
                //   • Exit-clipped ghost on the destination side (only the
                //     emerged half visible)
                // Together this produces Portal's iconic "see yourself
                // bisected by the portal" effect — but in multiplayer for
                // remote players.
                {
                    auto& portals = Client::GetClientPortalManager();
                    for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                        const glm::vec3 renderPos {
                            glm::mix(rp.renderPrevPosition.x, rp.position.x, partialTick),
                            glm::mix(rp.renderPrevPosition.y, rp.position.y, partialTick),
                            glm::mix(rp.renderPrevPosition.z, rp.position.z, partialTick),
                        };
                        const float headYaw = Client::RotLerp(partialTick,
                            rp.renderPrevRotation.x, rp.rotation.x);
                        const float pitch   = glm::mix(rp.renderPrevRotation.y,
                            rp.rotation.y, partialTick);
                        const float bodyYaw = Client::RotLerp(partialTick,
                            rp.renderPrevBodyYaw, rp.bodyYaw);
                        auto g = portals.GetStraddlingGhost(renderPos, 1.8f);
                        if (!g.valid) continue;
                        // Only emit the half-body split when the body
                        // actually crosses the entry plane (matches the
                        // straddlingIds gate so the bulk-pass exclusion
                        // and this re-emit agree). Without the gate,
                        // standing fully in front of a partner portal
                        // would render the remote at its dst-side ghost
                        // position with no src-side body — making them
                        // appear teleported.
                        if (!BodyStraddlesEntryPlane(renderPos,
                                                     g.entryClipPlane, 1.8f)) {
                            continue;
                        }
                        // 1) Entry-side body, clipped to the half on the
                        //    source side of the source portal plane. Uses
                        //    identity model — same world position as the
                        //    bulk render would have produced.
                        playerRenderer.RenderSingle(
                            proj, view, renderPos,
                            headYaw, bodyYaw, pitch, rp.isCrouching,
                            static_cast<uint8_t>(rp.color),
                            glm::mat4(1.0f), g.entryClipPlane);
                        // 2) Exit-side ghost, transformed through the
                        //    portal pair matrix and clipped to the
                        //    emerged half on the destination side.
                        playerRenderer.RenderSingle(
                            proj, view, renderPos,
                            headYaw, bodyYaw, pitch, rp.isCrouching,
                            static_cast<uint8_t>(rp.color),
                            g.transform, g.exitClipPlane);
                    }

                    // Local-player ghost — when straddling, render the
                    // local player on the destination side too. Visible
                    // through the destination portal from outside (and via
                    // the see-through pass below). The local body itself
                    // is invisible in first person, so no entry-side
                    // render here. Same straddle gate as above so a
                    // player just CLOSE to a portal doesn't sprout a
                    // duplicate copy on the dst side.
                    auto gLocal = portals.GetStraddlingGhost(
                        player.physics.position, 1.8f);
                    if (!justTeleportedThisFrame && gLocal.valid &&
                        BodyStraddlesEntryPlane(player.physics.position,
                                                gLocal.entryClipPlane, 1.8f)) {
                        const bool isCrouching =
                            Input::IsKeyDown(Input::Key::LeftShift);
                        playerRenderer.RenderSingle(
                            proj, view, player.physics.position,
                            camera.yaw, camera.yaw, camera.pitch,
                            isCrouching,
                            static_cast<uint8_t>(player.color),
                            gLocal.transform, gLocal.exitClipPlane);
                    }
                }
#endif
            }

            // Update per-frame item render context so animated items (compass, clock, etc.)
            // can resolve their visual state from world data. MC: equivalent to passing the
            // ClientLevel + LocalPlayer to ItemModelResolver each frame.
            {
                Game::ItemRenderContext ctx;
                ctx.playerX = player.physics.position.x;
                ctx.playerY = player.physics.position.y;
                ctx.playerZ = player.physics.position.z;
                // Mouse-look writes to `camera.yaw` every frame; `player.yaw`
                // is only assigned on init/teleport and stays stale otherwise.
                // The compass needs the live look-direction to counter-rotate
                // the needle as the player turns.
                ctx.playerYaw = camera.yaw;
                // Compass target: world spawn at (0, 0). LodestoneTracker support TODO.
                ctx.compassTargetX = 0.0f;
                ctx.compassTargetZ = 0.0f;
                ctx.timeSeconds = static_cast<float>(glfwGetTime());
                // Shield-raise predicate feed (BLOCK use animation running).
                ctx.usingItemBlock = player.usingItem
                    && player.useAnim == Game::ItemUseAnimation::BLOCK;
                Game::ItemRegistry::SetRenderContext(ctx);
                // Step the wobble simulation (MC: CompassAngleState ticks at 20 TPS).
                Game::ItemRegistry::TickAnimated(dt);
            }

            // Render UI overlay elements
            RenderBlockHighlight(player, proj, view,
                                 playerController.PickEntity() != 0);
            RenderBlockBreakOverlay(playerController, proj, view);

            // BlockEntity per-frame render pass. Iterates every loaded
            // client chunk's BE map, frustum/distance-culls each BE, and
            // dispatches to its registered renderer. Runs after the chunk
            // solid + cutout passes (so BEs sit on top of the cube voxels)
            // but before portals (so they're masked correctly when seen
            // through a portal).
            if (Render::g_blockEntityRenderDispatcher && Client::g_clientChunkManager) {
                PROFILE_ZONE_N("BlockEntities");
                Render::g_blockEntityRenderDispatcher->RenderAll(
                    Client::g_clientChunkManager.get(),
                    proj, view, camera.position, /*partialTick=*/0.0f);
            }
#if ENABLE_PORTAL_GUN
            // Phase 7 portal pass: recursive see-through rendering. The
            // lambda is invoked once per recursion level by the portal
            // renderer with that level's virtual camera + oblique
            // projection — it draws chunks, remote players, AND the local
            // player (rendered as a stick figure since the player can see
            // themselves through the portal). Stencil setup is handled by
            // the portal renderer; the lambda just renders. No nametags or
            // chat bubbles in see-through (per user request — model only).
            {
                PROFILE_ZONE_N("PortalRender");
                // partialTick — interpolation fraction within the current
                // 50ms client tick. Same calc as the main remote-player
                // render below; recomputed here so it's in lambda scope.
                const auto nowForPartial = std::chrono::steady_clock::now();
                const float remaining =
                    std::chrono::duration<float>(nextClientTick - nowForPartial).count();
                const float tickSeconds =
                    std::chrono::duration<float>(CLIENT_TICK_INTERVAL).count();
                const float partialTickPortal =
                    std::clamp(1.0f - remaining / tickSeconds, 0.0f, 1.0f);

                Render::g_portalRenderer.Render(
                    proj, view, camera, frustum, aspect, farPlane,
                    [&](const Render::Camera& virtCam,
                        const Frustum& virtFrust,
                        const glm::mat4& obliqueProj) {
                        // Chunks from the virtual camera (stencil + oblique
                        // projection both honored).
                        Render::RenderChunksAll(virtCam, virtFrust, obliqueProj);

                        const glm::mat4 virtView = virtCam.GetViewMatrix();

                        // Mobs and dropped items from the virtual camera. Both
                        // were missing here, so a zombie standing in front of a
                        // portal simply was not in the view through it — the
                        // terrain came through and the entities did not.
                        //
                        // Neither renderer needs the straddle split the players
                        // get below: that exists so a body half-way through a
                        // portal is drawn once on each side, and only the local
                        // and remote players are ever tracked that way.
                        itemEntityRenderer.Render(obliqueProj, virtView,
                                                  virtCam.position, partialTickPortal);
                        if (Client::g_clientMobManager) {
                            mobRenderer.Render(obliqueProj, virtView, virtCam.position,
                                               *Client::g_clientMobManager,
                                               partialTickPortal);
                        }

                        // Remote players (no nametags / bubbles — those are
                        // separate calls in the main pass below). Phase G
                        // (remote): exclude straddlers from the bulk pass
                        // so they get the same per-half entry/exit clip
                        // treatment as in the main scene render.
                        if (Client::g_remotePlayerManager) {
                            std::unordered_set<uint32_t> stRemote;
                            for (const auto& [id, rp] :
                                 Client::g_remotePlayerManager->GetPlayers()) {
                                auto gC = Client::GetClientPortalManager()
                                              .GetStraddlingGhost(rp.position, 1.8f);
                                if (gC.valid && BodyStraddlesEntryPlane(
                                                    rp.position,
                                                    gC.entryClipPlane, 1.8f)) {
                                    stRemote.insert(id);
                                }
                            }
                            playerRenderer.Render(obliqueProj, virtView,
                                                  virtCam.position,
                                                  *Client::g_remotePlayerManager,
                                                  partialTickPortal, &stRemote);
                        }

                        // Local player as a stick figure — invisible in the
                        // main pass (it IS the camera) but should appear
                        // through the portal so the user sees themselves.
                        // Use camera.yaw/pitch (live mouse-look values) —
                        // player.yaw/pitch are stale until next teleport.
                        // Body yaw = head yaw since the local player has no
                        // separate body-rotation tracking.
                        //
                        // Phase G: when straddling, clip the body at the
                        // src plane (entryClipPlane). Without this clip,
                        // the see-through view shows the player's FULL
                        // body at the entry position AND a ghost copy
                        // on the dst side — both visible simultaneously,
                        // breaking the "body bisects at the portal plane"
                        // iconic Portal trick.
                        const bool isCrouching =
                            Input::IsKeyDown(Input::Key::LeftShift);
                        auto& portalsST = Client::GetClientPortalManager();
                        auto gLocalST = portalsST.GetStraddlingGhost(
                            player.physics.position, 1.8f);
                        // Only apply the entry/exit half-body split when
                        // the body ACTUALLY crosses the plane. Without
                        // this gate, gLocalST.valid (which fires for any
                        // portal within 1m of the body center) makes
                        // "standing in front of a portal that happens
                        // to face you" clip out the whole body when the
                        // entry plane's kept-side is the OTHER side.
                        const bool straddlesEntry =
                            gLocalST.valid &&
                            BodyStraddlesEntryPlane(player.physics.position,
                                                    gLocalST.entryClipPlane, 1.8f);
                        const glm::vec4 entryClip =
                            straddlesEntry ? gLocalST.entryClipPlane
                                           : glm::vec4(0.0f);
                        // Skip the local body for the teleport frame
                        // itself. virtCam was derived from the just-
                        // snapped real camera, so applying SrcToDst
                        // again places the local body at a junk screen
                        // position for one frame; the next frame's
                        // render produces the correct view.
                        if (!justTeleportedThisFrame) {
                            playerRenderer.RenderSingle(
                                obliqueProj, virtView,
                                player.physics.position,
                                camera.yaw, camera.yaw, camera.pitch,
                                isCrouching,
                                static_cast<uint8_t>(player.color),
                                glm::mat4(1.0f), entryClip);
                        }

                        // Ghost render for through-portal visibility:
                        // when local or remote players straddle, also
                        // draw their ghost copy at the destination so
                        // looking back through the portal from the
                        // virtual camera shows the emerging half-body.
                        // Same straddle gate — only emit when there's
                        // an actual half-body to show; otherwise the
                        // ghost is fully in dst space already and the
                        // see-through pass renders it via the entry-
                        // render above. Also skip on the teleport
                        // frame (same reason as the body render).
                        if (straddlesEntry && !justTeleportedThisFrame) {
                            playerRenderer.RenderSingle(
                                obliqueProj, virtView,
                                player.physics.position,
                                camera.yaw, camera.yaw, camera.pitch,
                                isCrouching,
                                static_cast<uint8_t>(player.color),
                                gLocalST.transform, gLocalST.exitClipPlane);
                        }
                        if (Client::g_remotePlayerManager) {
                            for (const auto& [id, rp] :
                                 Client::g_remotePlayerManager->GetPlayers()) {
                                auto gR = portalsST.GetStraddlingGhost(
                                    rp.position, 1.8f);
                                if (!gR.valid) continue;
                                // Same body-straddles-plane gate as
                                // the local-player block above. When
                                // the remote is fully on one side,
                                // skip the half-body split entirely;
                                // the bulk pass already excluded them
                                // expecting the split (when stRemote
                                // also gates on this), so re-emit a
                                // single unclipped body here.
                                if (!BodyStraddlesEntryPlane(
                                        rp.position, gR.entryClipPlane, 1.8f)) {
                                    playerRenderer.RenderSingle(
                                        obliqueProj, virtView, rp.position,
                                        rp.rotation.x, rp.bodyYaw, rp.rotation.y,
                                        rp.isCrouching,
                                        static_cast<uint8_t>(rp.color),
                                        glm::mat4(1.0f), glm::vec4(0.0f));
                                    continue;
                                }
                                // Entry-clipped body on the source side
                                // (replaces the bulk pass we excluded).
                                playerRenderer.RenderSingle(
                                    obliqueProj, virtView, rp.position,
                                    rp.rotation.x, rp.bodyYaw, rp.rotation.y,
                                    rp.isCrouching,
                                    static_cast<uint8_t>(rp.color),
                                    glm::mat4(1.0f), gR.entryClipPlane);
                                // Exit-clipped ghost on the destination
                                // side.
                                playerRenderer.RenderSingle(
                                    obliqueProj, virtView, rp.position,
                                    rp.rotation.x, rp.bodyYaw, rp.rotation.y,
                                    rp.isCrouching,
                                    static_cast<uint8_t>(rp.color),
                                    gR.transform, gR.exitClipPlane);
                            }
                        }
                    });
            }
            // Tick + render the portal particle system. Tick uses dt
            // (frame time) so spawn rate is FPS-independent; render
            // happens AFTER the portal pass so sparks composite over the
            // see-through view (additive blending).
            Render::g_portalParticleSystem.Update(dt, Client::GetClientPortalManager());
            Render::g_portalParticleSystem.Render(proj, view, camera.position);

#endif

            // Clouds — MC's cloud pass runs after terrain and particles
            // (translucent, depth-tested against the world, no depth write).
            {
                PROFILE_ZONE_N("CloudPass");
                const auto nowForPartial = std::chrono::steady_clock::now();
                const float remaining =
                    std::chrono::duration<float>(nextClientTick - nowForPartial).count();
                const float tickSeconds =
                    std::chrono::duration<float>(CLIENT_TICK_INTERVAL).count();
                const float cloudPartialTick =
                    std::clamp(1.0f - remaining / tickSeconds, 0.0f, 1.0f);
                Render::g_cloudRenderer.Render(proj, view, camera.position,
                                               effectiveRenderDist, cloudPartialTick);
            }

#if ENABLE_PORTAL_GUN
            // First-person portal-gun viewmodel.
            //
            // This MUST come after the cloud pass, not before it. Every
            // first-person viewmodel here CLEARS THE DEPTH BUFFER before
            // drawing (so the gun cannot clip into a wall it is standing
            // against) — and the cloud pass depth-tests against the world.
            // Rendering the gun first therefore handed the clouds an empty
            // depth buffer and they drew straight through the terrain, which
            // looked like the gun making blocks transparent. The vanilla
            // held-item viewmodel below has always been on this side of the
            // clouds for the same reason; the portal gun was the odd one out.
            //
            // Still after the particle system, so the rim sparks composite
            // behind the gun. Hidden unless the gun is actually held (and
            // never in third person).
            if (camera.IsFirstPerson() &&
                player.inventory.GetSelectedItem() == Game::Items::PortalGun) {
                const float fbAspect = (height > 0)
                    ? static_cast<float>(width) / static_cast<float>(height)
                    : 16.0f / 9.0f;
                Render::g_portalGunViewmodel.Render(fbAspect, dt);
            }
#endif

            // Vanilla held-item viewmodel — both hands (main = selected
            // hotbar item, off = slot 45). The portal gun owns the main
            // hand when selected (its own viewmodel rendered above), but
            // the offhand still draws. Computes a fresh partialTick for
            // animation interp (the one above is scoped inside the
            // remote-player block) and accumulates walk distance from
            // horizontal velocity for the bob phase.
            bool renderMainHand =
                !player.inventory.GetSlot(Game::Inventory::HotbarToIndex(
                    player.inventory.GetSelectedSlot())).IsEmpty();
#if ENABLE_PORTAL_GUN
            if (player.inventory.GetSelectedItem() == Game::Items::PortalGun) {
                renderMainHand = false;
            }
#endif
            const bool drawHeldItem = camera.IsFirstPerson() &&
                (renderMainHand ||
                 !player.inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN).IsEmpty());
            if (drawHeldItem) {
                PROFILE_ZONE_N("HeldItem");
                const float fbAspect = (height > 0)
                    ? static_cast<float>(width) / static_cast<float>(height)
                    : 16.0f / 9.0f;
                const auto  nowForPT = std::chrono::steady_clock::now();
                const float ptRemain =
                    std::chrono::duration<float>(nextClientTick - nowForPT).count();
                const float ptTick =
                    std::chrono::duration<float>(CLIENT_TICK_INTERVAL).count();
                const float partialTickHeld =
                    std::clamp(1.0f - ptRemain / ptTick, 0.0f, 1.0f);
                // Accumulate walk distance from horizontal velocity. MC
                // does this once per tick; per-frame approximation is
                // close enough for the bob amplitude.
                static float s_walkDist = 0.0f;
                const float vx = player.physics.velocity.x;
                const float vz = player.physics.velocity.z;
                const float speed = std::sqrt(vx * vx + vz * vz);
                s_walkDist += speed * dt * 0.6f;
                // Feed the predicted hold-to-use state (eat wiggle / shield
                // block pose) — routed to whichever hand is using.
                Render::g_heldItemRenderer.SetUseState(
                    player.usingItem,
                    player.usingHand,
                    player.useAnim,
                    player.useItemRemaining,
                    player.useItemDuration);
                // Live view angles, MC convention — and they MUST be the same
                // ones Tick() was given. The sway is a lag term,
                // `(viewXRot - xBob) * 0.1` (ItemInHandRenderer), where xBob is
                // the copy Tick chases toward that same angle: feeding Tick
                // +pitch and Render -pitch made the difference read as -2*pitch
                // instead of ~0, so instead of settling after a flick the item
                // sat at a permanent -0.2 * pitch tilt that swung as you looked
                // up and down.
                Render::g_heldItemRenderer.Render(fbAspect, partialTickHeld,
                                                  s_walkDist,
                                                  camera.pitch, camera.yaw,
                                                  renderMainHand);
            }
            // Push the server-synced stat triple into the HUD before drawing
            // (health/hunger bars read these; SetHealthS2C writes the player).
            // MC Gui.renderCrosshair reads the CLIENT's own attack ticker.
            // getAttackStrengthScale(0.0) — the zero is MC's: the bar lags the
            // damage calculation's 0.5 by half a tick so a full-looking bar
            // always is one.
            {
                g_hudRenderer.SetAttackStrength(
                    player.GetAttackStrengthScale(0.0f),
                    player.GetCurrentItemAttackStrengthDelay());
                // MC only shows the "charged" burst when the crosshair is on
                // something hittable. Every id PickEntity can return is a
                // living, alive entity (mobs and other players), so the pick
                // alone answers MC's `instanceof LivingEntity && isAlive()`.
                g_hudRenderer.SetCrosshairOnLivingTarget(
                    playerController.HasEntityUnderCrosshair());
            }

            g_hudRenderer.SetHealth(player.health);
            g_hudRenderer.SetFood(player.food);
            g_hudRenderer.SetSaturation(player.saturation);
            // MC Gui gates the survival stat block on gameMode.canHurtPlayer()
            // (Gui.java:524). The extra !gameModeKnown term covers a window MC
            // doesn't have: our render loop spins from the moment the socket
            // connects, so without it a creative joiner gets a frame or two of
            // hearts and hunger drawn off the survival default before the first
            // abilities packet lands.
            g_hudRenderer.SetStatsHidden(!player.gameModeKnown ||
                                         player.IsCreative() || player.IsSpectator());
            {
                PROFILE_ZONE_N("HudRender");
                RenderHUD(window, player.inventory, dt, proj, view);
            }
            // Hide the crosshair while any Screen is shown (inventory, pause
            // menu, options). In MC the crosshair sits on an early HUD stratum
            // and screens draw over it; ours is a standalone pass drawn AFTER
            // the GUI, so "behind the screen" has to mean "not drawn at all" —
            // same visible result: no crosshair over the menu.
            if (!Render::GetInventoryScreen().IsOpen() &&
                Render::GetScreenManager().Empty() &&
                camera.IsFirstPerson()) {   // MC: crosshair only in first person
                RenderCrosshair(window);
#if ENABLE_PORTAL_GUN
                // Portal quickinfo brackets layer on top of the base
                // crosshair — only when holding the portal gun. The
                // last-placed pulse fires automatically via the
                // PortalCrosshair::NotifyPortalPlaced hook in
                // ClientPortalManager::OnPortalSet.
                if (player.inventory.GetSelectedItem() == Game::Items::PortalGun) {
                    int winW = 0, winH = 0, fbW = 0, fbH = 0;
                    glfwGetWindowSize(window, &winW, &winH);
                    glfwGetFramebufferSize(window, &fbW, &fbH);
                    Render::g_portalCrosshair.Render(winW, winH, fbW, fbH, dt);
                }
#endif
            }
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
                    // Sessions no longer keep a per-player "waiting for load"
                    // set — delivery is push-based. The meaningful number is
                    // the server's in-flight load count.
                    pipeSnap.sessionPendingLoads =
                        Server::g_integratedServer->GetPendingChunkLoadCount();
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

            // ── World Info panel snapshot ──────────────────────────────────
            // Assembled here because DebugSystem.cpp lives in the `imgui`
            // target and cannot see the world/registry headers. Only built
            // while the panel is actually open — the biome tints below run the
            // same 5x5 blend the mesher does, 25 samples per channel.
            if (Debug::DebugSystem::GetPanelVisibility().worldInfo) {
                Debug::WorldInfoSnapshot wi;

                const glm::vec3 p = player.physics.position;
                wi.posX = p.x; wi.posY = p.y; wi.posZ = p.z;
                wi.blockX = static_cast<int>(std::floor(p.x));
                wi.blockY = static_cast<int>(std::floor(p.y));
                wi.blockZ = static_cast<int>(std::floor(p.z));
                const auto cpos = Game::Math::WorldCoordinates::WorldToChunkPos(wi.blockX, wi.blockZ);
                wi.chunkX = cpos.x; wi.chunkZ = cpos.z;
                wi.localX = wi.blockX - cpos.x * Game::Math::CHUNK_SIZE_X;
                wi.localZ = wi.blockZ - cpos.z * Game::Math::CHUNK_SIZE_Z;
                wi.sectionIndex = Game::Math::WorldCoordinates::WorldYToSectionIndex(wi.blockY);
                wi.yawDeg = camera.yaw;
                wi.pitchDeg = camera.pitch;
                wi.facingName = std::string(Game::NameOf(Game::FromYRot(camera.yaw)));

                auto biomeName = [](uint16_t id) {
                    return std::string(Game::BiomeRegistry::Get(id).name);
                };
                // Read through the client's own accessor so the panel reports
                // what the MESHER saw, not what the server thinks.
                auto biomeAt = [&](int bx, int by, int bz) -> uint16_t {
                    if (Client::g_clientBlockAccess) {
                        return Client::g_clientBlockAccess->GetBiome(bx, by, bz);
                    }
                    return world ? world->GetBiome(bx, by, bz) : Game::kFallbackBiomeId;
                };

                const uint16_t feetBiome = biomeAt(wi.blockX, wi.blockY, wi.blockZ);
                const int eyeY = static_cast<int>(std::floor(p.y + 1.62f));
                wi.biomeStanding = biomeName(feetBiome);
                wi.biomeEye      = biomeName(biomeAt(wi.blockX, eyeY, wi.blockZ));

                const Game::BiomeInfo& bi = Game::BiomeRegistry::Get(feetBiome);
                wi.biomeTemperature = bi.temperature;
                wi.biomeDownfall    = bi.downfall;
                switch (bi.grassModifier) {
                    case Game::GrassColorModifier::DarkForest: wi.biomeGrassModifier = "dark_forest"; break;
                    case Game::GrassColorModifier::Swamp:      wi.biomeGrassModifier = "swamp"; break;
                    default:                                   wi.biomeGrassModifier = "none"; break;
                }
                wi.tintGrass       = Game::BiomeRegistry::GrassColor(feetBiome, wi.blockX, wi.blockZ);
                wi.tintFoliage     = Game::BiomeRegistry::FoliageColor(feetBiome);
                wi.tintDryFoliage  = Game::BiomeRegistry::DryFoliageColor(feetBiome);
                wi.tintWater       = Game::BiomeRegistry::WaterColor(feetBiome);

                const Game::BlockID below = Client::g_clientBlockAccess
                    ? Client::g_clientBlockAccess->GetBlock(wi.blockX, wi.blockY - 1, wi.blockZ)
                    : Game::BlockID::Air;
                wi.standingOnName = Game::BlockRegistry::Get(below).name;

                if (player.lastBlockHit.has_value()) {
                    const auto& hit = *player.lastBlockHit;
                    wi.hasTarget = true;
                    wi.targetX = hit.blockPos.x;
                    wi.targetY = hit.blockPos.y;
                    wi.targetZ = hit.blockPos.z;
                    wi.targetDistance = hit.distance;
                    wi.targetBlockId = static_cast<int>(hit.blockId);
                    wi.targetStateIndex = hit.stateIndex;
                    wi.biomeTarget = biomeName(biomeAt(hit.blockPos.x, hit.blockPos.y, hit.blockPos.z));

                    const Game::Block& b = Game::BlockRegistry::Get(hit.blockId);
                    wi.targetBlockName = b.name.empty() ? b.modelName : b.name;
                    wi.targetHardness = b.destroyTime;
                    wi.targetHasCollision = Game::BlockRegistry::HasCollision(hit.blockId);
                    switch (b.renderLayer) {
                        case Game::RenderLayer::Cutout:      wi.targetRenderLayer = "cutout"; break;
                        case Game::RenderLayer::Translucent: wi.targetRenderLayer = "translucent"; break;
                        default:                             wi.targetRenderLayer = "opaque"; break;
                    }

                    // Decode the state index back into its properties, so a
                    // furnace reads "facing=east" rather than "state 1".
                    const auto& def = Game::BlockRegistry::GetStateDefinition(hit.blockId);
                    if (def.properties.empty()) {
                        wi.targetStateProps = "-";
                    } else {
                        wi.targetStateProps.clear();
                        for (const auto& [k, v] : def.PropertiesOf(hit.stateIndex)) {
                            if (!wi.targetStateProps.empty()) wi.targetStateProps += ", ";
                            wi.targetStateProps += k + "=" + v;
                        }
                    }

                    static const char* kFaceNames[] = {
                        "+X east", "-X west", "+Y up", "-Y down", "+Z south", "-Z north" };
                    wi.targetFace = (hit.hitFace >= 0 && hit.hitFace < 6)
                                      ? kFaceNames[hit.hitFace] : "?";

                    const auto& shape =
                        Game::BlockRegistry::GetBlockShape(hit.blockId, hit.stateIndex);
                    wi.targetShapeMin[0] = shape.min.x; wi.targetShapeMin[1] = shape.min.y;
                    wi.targetShapeMin[2] = shape.min.z;
                    wi.targetShapeMax[0] = shape.max.x; wi.targetShapeMax[1] = shape.max.y;
                    wi.targetShapeMax[2] = shape.max.z;

                    wi.targetChunkLoaded = Client::g_clientBlockAccess &&
                        Client::g_clientBlockAccess->IsPositionLoaded(
                            hit.blockPos.x, hit.blockPos.y, hit.blockPos.z);
                }

                if (world) wi.seed = world->GetGenerationSeed();

                Debug::DebugSystem::SetWorldInfoSnapshot(wi);
            }

            // ── Entity panel snapshot ──────────────────────────────────────
            //
            // Built here rather than in DebugSystem because DebugSystem.cpp is
            // compiled into the `imgui` target and cannot see IntegratedServer.
            // Only assembled when the panel is actually open — walking every
            // mob twice per frame for a hidden window is pure waste.
            if (Debug::DebugSystem::GetPanelVisibility().entities) {
                Debug::EntitySnapshot es;

                for (uint16_t i = 0; i < static_cast<uint16_t>(Game::EntityTypeId::Count) &&
                                     es.typeRowCount < Debug::EntitySnapshot::kTypeCount; ++i) {
                    es.types[es.typeRowCount].name =
                        Game::GetEntityTypeInfo(static_cast<Game::EntityTypeId>(i)).slug.data();
                    ++es.typeRowCount;
                }

                if (Server::g_integratedServer) {
                    if (auto* mobs = Server::g_integratedServer->GetMobs()) {
                        // Atomic counters, NOT a walk of mobs->All(). The mob
                        // map lives on the server thread and is mutated every
                        // tick; iterating it from here is a data race that
                        // crashes rather than merely reporting a stale number.
                        for (int i = 0; i < es.typeRowCount; ++i) {
                            es.types[i].serverCount =
                                mobs->CountForType(static_cast<uint16_t>(i));
                            es.totalServerMobs += es.types[i].serverCount;
                        }
                        es.monsterCount =
                            mobs->CountForCategory(static_cast<int>(Game::MobCategory::Monster));
                        es.creatureCount =
                            mobs->CountForCategory(static_cast<int>(Game::MobCategory::Creature));
                        es.spawnableChunks = mobs->GetSpawnableChunkCount();
                    }
                }

                if (Client::g_clientMobManager) {
                    for (const auto& [id, entry] : Client::g_clientMobManager->All()) {
                        const auto idx = static_cast<size_t>(entry.mob->GetType());
                        if (idx < Debug::EntitySnapshot::kTypeCount) {
                            ++es.types[idx].clientCount;
                        }
                        ++es.totalClientMobs;
                    }
                }

                es.monsterCap =
                    Game::GetMobCategoryInfo(Game::MobCategory::Monster).maxInstancesPerChunk *
                    es.spawnableChunks / Game::kMagicNumber;
                es.creatureCap =
                    Game::GetMobCategoryInfo(Game::MobCategory::Creature).maxInstancesPerChunk *
                    es.spawnableChunks / Game::kMagicNumber;

                Debug::DebugSystem::SetEntitySnapshot(es);
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
                Input::SaveKeyBindings();
        Platform::g_gameSettings.Save();
            }

            // 11. Swap buffers (includes VSync wait)
            // NOT a vsync wait — this is the buffer swap, which is where the
            // driver flushes the queued command stream and blocks if the GPU
            // is behind the CPU. With vsync OFF a long "Present" means the GPU
            // is still chewing on work this frame queued (usually buffer
            // uploads), not that anything is waiting on the display.
            { PROFILE_ZONE_N("Present");
            PROFILE_TIMER_START(vsync);
            if (Render::g_renderBackend) {
                Render::g_renderBackend->EndFrame(window);
            } else {
                glfwSwapBuffers(window);
            }
            PROFILE_TIMER_END(vsync, metrics.vsyncWaitTime);
            }

            // Max Framerate option (Video Settings). 260 = Unlimited. VSync
            // already paces the loop when enabled, so the limiter only kicks
            // in for uncapped-swap configurations. sleep_until keeps the cap
            // steady without burning a core.
            {
                const int maxFps = Platform::g_gameSettings.GetMaxFPS();
                if (maxFps > 0 && maxFps < 260 && !Platform::g_gameSettings.GetVSync()) {
                    static auto s_nextFrameDeadline = std::chrono::steady_clock::now();
                    const auto frameBudget = std::chrono::nanoseconds(1'000'000'000LL / maxFps);
                    const auto nowClock = std::chrono::steady_clock::now();
                    if (s_nextFrameDeadline < nowClock - frameBudget) {
                        s_nextFrameDeadline = nowClock; // fell behind — resync
                    }
                    s_nextFrameDeadline += frameBudget;
                    std::this_thread::sleep_until(s_nextFrameDeadline);
                }
            }

            // Restore the logical eye camera after the third-person render
            // frame (see the F5 block above the render phase).
            if (tpActive) {
                camera.position = tpSavedPos;
                camera.yaw      = tpSavedYaw;
                camera.pitch    = tpSavedPitch;
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

        // === SESSION SHUTDOWN SEQUENCE (Minecraft-style) ===
        Log::Info("Shutting down session...");

        // Neutralize the disconnect callback FIRST: this is an intentional
        // teardown, and the socket closing must not flag the window to close
        // — that would turn "Save and Quit to Title" into an app exit.
        networkClient->SetOnDisconnected([](const std::string&) {});

        // Stop accepting relay tunnels: the server they'd be adopted into is
        // about to go away.
        if (Client::g_friendsClient) {
            Client::g_friendsClient->SetRelaySocketHandler(nullptr);
        }

        // Clear global block access for raycast if remote client
        if (isRemoteClient) {
            Game::SetGlobalBlockAccess(nullptr);
        }

        // 1. Disconnect NetworkClient
        Log::Info("Disconnecting NetworkClient...");
        networkClient->Disconnect();
        networkClient.reset();
        Client::g_networkClient = nullptr;   // global mirror of the session-scoped client
        Log::Info("✓ NetworkClient disconnected");

        // 2. Stop Network I/O Service (dedicated I/O thread)
        Log::Info("Stopping Network I/O Service...");
        Client::ShutdownNetworkIOService();
        Log::Info("✓ Network I/O Service stopped");

        // 3. Persist world time + gamerules back to worlds.json, then stop
        //    the IntegratedServer thread (host only). Minecraft-save worlds
        //    aren't tracked in worlds.json, so they're skipped.
        if (!isRemoteClient) {
            if (!titleAction.useMinecraftSave && Server::g_integratedServer &&
                Server::g_integratedServer->GetWorld()) {
                const auto* serverWorld = Server::g_integratedServer->GetWorld();
                auto worlds = Render::WorldList::Load();
                for (auto& entry : worlds) {
                    if (entry.name == titleAction.worldName) {
                        entry.dayTime         = serverWorld->GetDayTime();
                        entry.doDaylightCycle = serverWorld->GetDoDaylightCycle();
                        break;
                    }
                }
                Render::WorldList::Save(worlds);
                Log::Info("✓ World time saved (dayTime=%lld)",
                          static_cast<long long>(serverWorld->GetDayTime()));
            }

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
        Client::g_clientBlockAccess = nullptr;
        clientBlockAccess.reset();
        
        // 8. Shutdown rendering systems
        Render::ShutdownChunkRenderer();

        // 8a. Session-scoped render resources
        playerRenderer.Shutdown();
        Client::g_remotePlayerManager.reset();
        itemEntityRenderer.Shutdown();
        mobRenderer.Shutdown();
        Client::g_clientMobManager.reset();
        Client::g_itemEntityManager.reset();
        // The inventory screens are singletons but their menu is bound to this
        // session's ClientPlayer, which is about to go out of scope.
        Render::SetInventoryScreenPlayer(nullptr);

        // 9. Clear world reference (world was shut down by IntegratedServer)
        Game::g_world = nullptr;

        // ── End of session ──────────────────────────────────────────────
        // Quit-to-title: everything session-scoped is down; loop back to the
        // title screen. Window close / Quit falls through to process cleanup.
        if (returnToTitle && !glfwWindowShouldClose(window)) {
            Log::Info("World closed — returning to title screen");
            continue;
        }
        break;
        } // for(;;) — outer session loop

        // === PROCESS-LEVEL CLEANUP (runs once, after the last session) ===
        // Friends connection first — its io thread is independent of the
        // renderer, and stopping it flips this player offline for friends.
        if (Client::g_friendsClient) {
            Client::g_friendsClient->Stop();
            Client::g_friendsClient.reset();
        }
        // Hand the router port back (best-effort; routers also expire it).
        if (g_portMapper) {
            g_portMapper->Unmap();
            g_portMapper.reset();
        }

        // Destroy resources that depend on the render backend BEFORE destroying it
        g_hudRenderer = Render::HudRenderer();
        g_guiRenderer.Shutdown();
        g_fontRenderer.Shutdown();
        g_guiAtlas.Shutdown();
        Render::g_crosshair.Shutdown();
        Render::g_blockHighlight.Shutdown();
        Render::g_blockBreakOverlay.Shutdown();
        Render::g_skyRenderer.Shutdown();
        Render::g_cloudRenderer.Shutdown();
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

        // Stamp the log as ending deliberately. Without this marker a truncated
        // log and a clean one look identical, so you cannot tell "crashed" from
        // "player quit" — which matters most in the case the crash handler
        // never ran (see CrashHandler.hpp on crashpad winning the race).
        Log::CloseLogFile();

        return 0;
    }

} // namespace PlatformMain