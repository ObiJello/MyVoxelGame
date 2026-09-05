// File: src/platform/PlatformMain.cpp
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "client/input/Input.hpp"
#include "client/input/KeyMapping.hpp"
#include "client/dev/SessionReplay.hpp"
#include "common/core/Log.hpp"
#include <sentry.h>
#include "common/core/Config.hpp"
#include "common/core/Features.hpp"
#include "common/core/ThreadAllocator.hpp"
#include "common/core/HardwareProfile.hpp"
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
#include "client/renderer/blockentity/EndPortalRenderer.hpp"
#include "client/renderer/debug/Crosshair.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "client/portal/ClientImmersivePortals.hpp"
#include "client/renderer/portal/ImmersivePortalRenderer.hpp"
#include "client/portal/ImmersivePortalTraveler.hpp"
#include "client/portal/ImmersivePortalCollision.hpp"
#endif
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
#include "client/renderer/particle/MobParticleSystem.hpp"
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
#include "client/renderer/gui/screens/DisconnectedScreen.hpp"
#include "client/renderer/gui/screens/PauseScreen.hpp"
#include "client/renderer/gui/screens/DeathScreen.hpp"
#include "client/renderer/gui/screens/PanoramaRenderer.hpp"
#include "client/renderer/gui/screens/WorldSelectScreens.hpp"
#include "client/renderer/environment/EnvironmentState.hpp"
#include "client/renderer/environment/SkyRenderer.hpp"
#include "client/renderer/environment/CloudRenderer.hpp"
#include "client/renderer/gui/screens/OptionsScreens.hpp"
#include "client/renderer/gui/screens/SkyboxSelectScreen.hpp"
#include "client/network/FriendsClient.hpp"
#include "client/network/UPnPPortMapper.hpp"
#include "common/core/FriendsServiceConfig.hpp"
#include <algorithm> // std::clamp (FOV modifier)
#include <cstdlib>   // getenv (temp autoplay diagnostic)
#include "client/renderer/core/DevRenderSkip.hpp"
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
#include "client/renderer/entity/EntityCulling.hpp"
#include "client/renderer/texture/TextureAnimator.hpp"
#include "common/core/Profiling.hpp"
#include "common/core/Profiling_Tracy.hpp"

// Include world system headers
#include "common/world/level/World.hpp"
#include "common/world/level/WorldGlobals.hpp"
#include "server/world/ChunkProvider.hpp"
#include "server/world/MyTerrainGenerator.hpp"

// Include mesh system headers
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "client/renderer/debug/FlickerDiag.hpp"
#include "client/renderer/mesh/Mesher.hpp"
#include "client/renderer/mesh/MeshCensus.hpp"
#include "client/renderer/mesh/ClientMeshManager.hpp"
#include "client/renderer/mesh/MeshUploadPermits.hpp"

// Include new Minecraft-style architecture
#include "client/network/NetworkClient.hpp"
#include "server/session/PlayerSession.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkIOService.hpp"
#include "server/IntegratedServer.hpp"
#include "server/world/storage/anvil/WorldFolder.hpp"
#include "server/world/storage/anvil/WorldSidecar.hpp"
#include "server/network/NetworkServer.hpp"
#include "client/world/ClientChunkManager.hpp"
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include "client/world/LevelLoadTracker.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "server/world/ServerWorkerPool.hpp"
#include "client/world/ClientWorkerPool.hpp"

// Multiplayer player visibility
#include "client/entity/RemotePlayerManager.hpp"
#include "client/renderer/entity/PlayerRenderer.hpp"
#include "client/entity/ItemEntityManager.hpp"
#include "client/entity/XpOrbManager.hpp"
#include "client/renderer/entity/ItemEntityRenderer.hpp"
#include "client/renderer/entity/XpOrbRenderer.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/entity/ClientFallingBlocks.hpp"
#include "client/ClientTickRateManager.hpp"
#include "client/world/ClientAnimateTick.hpp"
#include "client/renderer/entity/BlockCubeEntityRenderer.hpp"
#include "client/renderer/mesh/FillPreviewRenderer.hpp"
#include "common/entity/SpawnEggs.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ItemEntityManager.hpp"
#include "server/entity/FallingBlockStore.hpp"
#include "server/level/ServerLevel.hpp"
#include "common/world/spawn/NaturalSpawner.hpp"   // kMagicNumber
#include "client/renderer/entity/MobRenderer.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

// Pointing-hand cursor for clickable chat components (MC swaps the cursor over
// a click event). Kept at file scope with an explicit reset so closing chat
// while hovering a link can't leave the hand cursor stuck on.
// A body drawn at the player's size: scaled about its feet, in world space
// (PlayerRenderer::RenderSingle applies `model` to world-space vertices).
// OBEY_PORTAL_DIAG: per frame, which pass drew the remote players and how
// each gate culled them, logged while a player is near a portal. Each entry
// is (inLevel, drawn, cullDistance, cullFrustum, cullCrossing) summed over
// the pass's calls this frame.
struct PlayerDrawDiag {
    int main[5]{}, crossers[5]{}, reverse[5]{}, far[5]{};
    void Add(int* slot, const Render::PlayerRenderer::Tally& t) {
        slot[0] += t.inLevel; slot[1] += t.drawn; slot[2] += t.cullDistance;
        slot[3] += t.cullFrustum; slot[4] += t.cullCrossing;
    }
    void Reset() { *this = PlayerDrawDiag{}; }
};
static PlayerDrawDiag g_playerDrawDiag;
static const bool g_portalDiag = std::getenv("OBEY_PORTAL_DIAG") != nullptr;

// Third-person camera state (see the F5 block in the frame loop): the
// zoom in body heights and the arrow keys' orbit in degrees.
static float s_thirdPersonZoom  = 4.0f;
static float s_thirdPersonOrbit = 0.0f;

static glm::mat4 BodyScaleModel(const glm::vec3& feet, float scale) {
    if (std::abs(scale - 1.0f) < 1e-4f) return glm::mat4(1.0f);
    return glm::translate(glm::mat4(1.0f), feet) *
           glm::scale(glm::mat4(1.0f), glm::vec3(scale)) *
           glm::translate(glm::mat4(1.0f), -feet);
}

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
#include "common/core/Assert.hpp"   // Client::g_clientThreadId

#include "platform/GameDirectory.hpp"
#include "client/resource/ResourcePacks.hpp"
#include "client/renderer/viewmodel/HeldItemSpriteMesh.hpp"
#include "client/renderer/entity/ItemEntityRenderer.hpp"
#include <thread>
#include "platform/CrashHandler.hpp"
#include "common/core/JobSystem.hpp"
#include "common/core/TickParallel.hpp"
#include "client/renderer/backend/RenderBackend.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <objc/objc.h>
#include <objc/message.h>
#include <unistd.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
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

// ── Sentry shutdown ─────────────────────────────────────────────────────────
//
// sentry_close() joins the "sentry-http" background worker, and that worker can
// be MID-REQUEST — a TLS handshake, loading the CA store, allocating inside
// OpenSSL. Doing that from an atexit handler is unsafe: by then main has
// returned and atexit callbacks are interleaving with static destructors, so
// the worker is running network I/O against a runtime that is already being
// torn down. It shows up as a crash on QUIT, on the sentry-http thread, inside
// malloc, with main parked in sentry__bgworker_shutdown waiting for it.
//
// So close it explicitly at the end of Run(), while the process is still fully
// alive. The atexit registration stays as a fallback for the early-return exit
// paths, and this flag makes whichever runs first the only one that acts —
// sentry_close() must not run twice.
static std::atomic<bool> s_sentryActive{false};

static void CloseSentryOnce() {
    bool expected = true;
    if (s_sentryActive.compare_exchange_strong(expected, false)) {
        sentry_close();
    }
}

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


    // The engine's own copy of an asset (bundle Resources, or the tree).
    std::string GetVanillaAssetPath(const std::string& relativePath) {
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

    // MC's FallbackResourceManager in one line: an enabled resource pack
    // that carries the asset wins over the engine's copy.
    std::string GetAssetPath(const std::string& relativePath) {
        if (std::string pack = Resources::FindOverride(relativePath); !pack.empty()) return pack;
        return GetVanillaAssetPath(relativePath);
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
            // Use the block's actual model shape so partial blocks (leaf
            // litter, slabs, fences, …) outline their real geometry instead of
            // the enclosing full cube.
            // World-aware so a paired chest outlines the half that is actually
            // there — otherwise the selection box stops a pixel short of the
            // seam that the raycast now accepts.
            //
            // One draw per box of the shape. MC outlines the whole VoxelShape
            // (LevelRenderer.renderShape → shape.forAllEdges), so a stair gets
            // the L-shaped profile rather than a cube around its empty half.
            // Vanilla's merged edge walk drops the internal edges where two
            // boxes meet and this does not, which shows as one extra line
            // across the step; the alternative is merging voxel shapes here
            // for a seam the player has to look for.
            const auto shapes = Client::g_clientBlockAccess
                ? Game::BlockRegistry::GetBlockShapeSetAt(*Client::g_clientBlockAccess,
                                                          hit->blockPos, hit->state)
                : Game::BlockRegistry::GetBlockShapeSet(hit->state);
            for (const auto& shape : shapes) {
                Render::g_blockHighlight.Render(hit->blockPos, proj, view,
                                                shape.min, shape.max);
            }
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
        // One crack box per box of the shape, so a stair cracks along its own
        // L rather than inside a phantom cube. SetTarget only stores the
        // extents — the mesh is a unit cube scaled at draw time — so retargeting
        // between Render calls is free.
        const auto stateIdx = Game::Raycast::GetBlockStateAt(bp.x, bp.y, bp.z);
        const auto shapes = Client::g_clientBlockAccess
            ? Game::BlockRegistry::GetBlockShapeSetAt(*Client::g_clientBlockAccess, bp, stateIdx)
            : Game::BlockRegistry::GetBlockShapeSet(stateIdx);
        for (const auto& shape : shapes) {
            Render::g_blockBreakOverlay.SetTarget(bp, stage, shape.min, shape.max);
            Render::g_blockBreakOverlay.Render(proj, view);
        }
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

    // Debug free-camera banner (F+C chord). Set once per frame by the chord
    // handler in the main loop; read here because RenderHUD's GuiGraphics is
    // the one cheap screen-text path the game has — the F3 overlay lives in
    // the imgui target and can't be reached per-frame for a one-line hint.
    static bool s_freeCamHudBanner = false;

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
        // ── Nametags above remote players ─────────────────────────────────────
        // Drawn BEFORE the HUD so the hotbar, chat and screens paint over a
        // tag that projects onto them, not the other way round.
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

            // One tag, at a point in THIS level's space, with the occlusion
            // already decided. Players in this level project directly; a
            // player seen through a portal projects at the image of their
            // position on this side of it.
            auto drawTag = [&](const Client::RemotePlayer& rp, const glm::vec3& tagWorld, bool occluded) {
                glm::vec4 clip = nameVp * glm::vec4(tagWorld, 1.0f);
                if (clip.w <= 0.0f) return;

                float ndcX = clip.x / clip.w;
                float ndcY = clip.y / clip.w;
                float sx = (ndcX * 0.5f + 0.5f) * guiWidth;
                float sy = (1.0f - (ndcY * 0.5f + 0.5f)) * guiHeight;

                // Perspective scale (MC's 0.025 world-units-per-font-pixel becomes this many GUI
                // pixels at our viewport): factor of guiHeight maps NDC's 2.0-unit Y range to
                // pixels, divide by 2 for the half-range, multiply by projY/depth for projection.
                float scale = (0.025f * static_cast<float>(guiHeight) * projY) / (2.0f * clip.w);
                // The tag grows and shrinks with the body, within limits
                // that keep a tiny player's name readable and a giant's
                // from covering the screen.
                scale *= std::clamp(rp.scale, 0.5f, 8.0f);

                int textW = g_fontRenderer.GetStringWidth(rp.name);
                const int lineH = Render::FontRenderer::LINE_HEIGHT;

                graphics.PushMatrix();
                graphics.Translate(sx, sy);
                graphics.Scale(scale, scale);
                int tagX = -textW / 2;
                // MC's tag pose is scaled by −0.025 in Y: the text stands
                // ABOVE its anchor. Drawn downward from it, a scaled tag
                // hung into the head.
                int tagY = -lineH;

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
            };

            // MC NameTagFeatureRenderer adds two passes (lines 49-54):
            //   Visible (in front of geometry):  solid white text, NO background  (line 53)
            //   Occluded (behind blocks):        50% white + 25% black bg          (line 51)
            // We approximate the depth test by raycasting from the camera to the tag's
            // world position — a hit means the player is behind something.
            auto blocked = [](const glm::vec3& from, const glm::vec3& to) {
                const glm::vec3 ray = to - from;
                const float len = glm::length(ray);
                if (len <= 0.001f) return false;
                return Game::Raycast::CastRay(from, ray / len, len).has_value();
            };

            for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                if (rp.name.empty()) continue;
                // Hide nametag entirely when the player is shifting/sneaking
                if (rp.isCrouching) continue;

                // MC NameTagFeatureRenderer line 43: translate(x, nameTagAttachment.y + 0.5, z)
                // where nameTagAttachment is at the top of the player's bbox (~1.8 high).
                // Our remote player position is at feet, so feet + 1.8 + 0.5 = feet + 2.3.
                // A scaled body is 1.8 × scale tall; the tag rides its top.
                const glm::vec3 tagWorld(rp.position.x, rp.position.y + 1.8f * rp.scale + 0.5f, rp.position.z);

                if (Client::IsRemotePlayerInBoundLevel(rp)) {
                    // MC default render distance for nametags is 64 blocks
                    float dx = rp.position.x - cameraPos.x;
                    float dz = rp.position.z - cameraPos.z;
                    if (dx * dx + dz * dz > 64.0f * 64.0f) continue;
                    drawTag(rp, tagWorld, blocked(cameraPos, tagWorld));
                }

#if ENABLE_IMMERSIVE_PORTALS
                // Through each portal of this level whose far side holds
                // the player: the tag sits at the image of the player's
                // position on this side, and shows only when the line from
                // the eye to it passes through the surface — i.e. when the
                // player is in view through the portal. Occlusion is tested
                // in two legs: eye to surface here, surface to player there.
                Client::GetClientImmersivePortals().ForEach([&](const Game::Immersive::Portal& p) {
                    if (!p.Has(Game::Immersive::PortalFlag::Visible) || p.IsMirror()) return;
                    if (p.destDimension != rp.dimension) return;
                    if (!p.IsInFront(glm::dvec3(cameraPos))) return;
                    const glm::dvec3 image = p.InverseTransformPoint(glm::dvec3(tagWorld));
                    const glm::dvec3 eye(cameraPos);
                    if (glm::length(image - eye) > 64.0) return;
                    const auto through = p.RaytraceSegment(eye, image, 0.0);
                    if (!through) {
                        // No surface between the eye and the image: the
                        // image is on THIS side of the plane. That is a
                        // player still filed in the far level while their
                        // copy finishes coming through (RemotePlayer::
                        // PendingCrossing keeps them there until the whole
                        // body is past the plane) — standing, in effect,
                        // just in front of the portal here. Their tag goes
                        // where they are, tested against this level only.
                        const double depth = p.SignedDistanceToPlane(image);
                        if (depth <= 0.0 || depth > 3.0 * std::max(rp.scale, 0.05f)) return;
                        drawTag(rp, glm::vec3(image), blocked(cameraPos, glm::vec3(image)));
                        return;
                    }
                    bool occluded = blocked(cameraPos, glm::vec3(through->point) -
                                                       glm::vec3(glm::normalize(through->point - eye)) * 0.05f);
                    if (!occluded) {
                        const glm::vec3 farEye(p.TransformPoint(glm::dvec3(through->point)));
                        Client::ClientLevels::WithLevel(p.destDimension, [&]() {
                            occluded = blocked(farEye, tagWorld);
                        });
                    }
                    drawTag(rp, glm::vec3(image), occluded);
                });
#endif
            }
        }

        { PROFILE_ZONE_N("Hud");
        g_hudRenderer.Render(graphics, inventory, deltaTime);
        }

        // Free-camera hint — drawn while the debug fly camera is detached so
        // it's obvious why the player is standing still and the crosshair is
        // gone. Top-center, out of the way of the hotbar and chat.
        if (s_freeCamHudBanner) {
            graphics.DrawCenteredString(
                "FREE CAMERA - culling frozen at player (F+C to return)",
                guiWidth / 2, 4, 0xFFFFFF55);
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

        // Chat bubbles above remote players (rendered in GUI space with text)
        if (Client::g_remotePlayerManager) {
            glm::mat4 vp = proj * view;
            for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                if (!Client::IsRemotePlayerInBoundLevel(rp)) continue;
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

        // Video Settings "Mipmap Levels" — MC passes options.mipmapLevels
        // into the AtlasManager at startup (Minecraft.java:523). options.txt
        // is not loaded yet at this point (this reads the compiled-in 4);
        // the saved value is applied right after InitializeGameDirectorySystem.
        Render::g_atlasBuilder->SetMipmapLevels(Platform::g_gameSettings.GetMipmapLevels());

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

    // MC Minecraft.reloadResourcePacks, for the parts of this engine that
    // can be rebuilt while it runs: the block atlas and colormaps, the GUI
    // atlas, font and menu sheets, the sky and cloud textures, every
    // renderer's texture cache (through Resources::CacheStale) and the
    // chunk meshes (their UVs moved with the atlas). Block models,
    // blockstates, item definitions and lang are startup-only — see
    // ResourcePacks.hpp — and the pack screen says so.
    void ReloadResources() {
        if (!Resources::ApplySelection()) return;
        Log::Info("[ResourcePacks] reloading resources...");

        // The mesh workers read the atlas UV table while they build; let
        // the ones in flight finish before the table is replaced.
        if (Threading::g_clientWorkerPool) {
            Threading::g_clientWorkerPool->CancelAllJobs();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (Threading::g_clientWorkerPool->GetActiveJobCount() > 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        if (Render::g_atlasBuilder) {
            Render::g_atlasBuilder->ReleaseGpuResources();
            if (!Render::g_atlasBuilder->BuildFromJSON(GetAssetPath("assets/atlases/blocks.json"),
                                                        GetAssetPath("assets/textures"))) {
                Log::Error("[ResourcePacks] block atlas rebuild failed");
            }
        }
        Game::BiomeRegistry::LoadColormaps(GetAssetPath("assets/textures"));
        // The workers cache sprite rects and ids per thread; the new atlas
        // numbered its sprites afresh.
        Render::Mesher::InvalidateAtlasCaches();
        Render::g_blockBreakOverlay.InvalidateAtlas();

        g_guiAtlas.Shutdown();
        if (!g_guiAtlas.Initialize(GetAssetPath("assets/textures/gui/sprites"))) {
            Log::Warning("[ResourcePacks] GUI atlas rebuild failed");
        }
        g_fontRenderer.Shutdown();
        if (!g_fontRenderer.Initialize(GetAssetPath("assets/textures/font/ascii.png"))) {
            Log::Warning("[ResourcePacks] font rebuild failed");
        }
        Render::g_crosshair.Shutdown();
        Render::g_crosshair.Initialize(GetAssetPath("assets/textures/gui/sprites/hud/crosshair.png"));
        Render::ResetMenuTextures();
        Render::HeldItemSpriteMesh::ClearCache();
        Render::ClearBlockItemMeshCacheForReload();
        Render::g_mobParticleSystem.ReloadTextures();
        Render::g_skyRenderer.ReloadResources();
        Render::g_cloudRenderer.ReloadTexture();

        // MC LevelRenderer.allChanged: every section is rebuilt with the new
        // atlas (its UVs) and colormaps.
        if (Render::g_clientMeshManager && Client::g_clientChunkManager) {
            std::vector<Render::ClientMeshManager::SectionKey> keys;
            Render::g_clientMeshManager->ForEachActiveSection(
                [&keys](const Render::ClientMeshManager::SectionKey& key, const Render::GPUSectionData*) { keys.push_back(key); });
            for (const auto& key : keys) {
                Client::g_clientChunkManager->MarkSectionDirty(key.chunkPos, key.sectionY);
            }
            Log::Info("[ResourcePacks] %zu sections queued for remesh", keys.size());
        }
        if (Resources::AnyEnabledPackHasStartupOnlyContent()) {
            Log::Info("[ResourcePacks] a selected pack carries models / blockstates / items / lang: those apply at the next launch");
        }
    }

    void HandlePlayerInput(Game::ClientPlayer& player, Game::ClientPlayerController& controller, Render::Camera& camera, bool cursorVisible, bool freeCamDetached) {
        // When the cursor is visible (Tab-toggle, inventory or chat
        // open) the player is interacting with UI, not the world —
        // drop world-space movement & action input so WASD held when
        // opening an overlay doesn't keep walking, and clicks don't
        // shoot portals / break blocks behind the cursor.
        //
        // The debug free camera detaches input the same way: while it is
        // active WASD/jump/sneak fly the detached camera (see the freeCam
        // update in the GameLogic phase), so none of it may reach the player
        // — and, like MC's spectator-style detach, block interaction is dead
        // too: every gameplay action below is DRAINED but not acted on, so
        // nothing queues up and fires on re-attach.
        if (cursorVisible || freeCamDetached) {
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
        if (cursorVisible || freeCamDetached) {
            // A screen is up (or the free camera is detached): drop anything
            // queued and make sure an in-progress break/use is torn down. MC
            // does the same via KeyMapping.releaseAll on setScreen plus
            // missTime.
            while (Input::ConsumeClick(*Input::Binds::Attack)) {}
            while (Input::ConsumeClick(*Input::Binds::Use))    {}
            controller.ContinueAttack(false);
            controller.StopUseItem();
        } else {
            auto dispatch = [&]() {
                while (Input::ConsumeClick(*Input::Binds::Attack)) {
                    controller.StartAttack();
                }
                while (Input::ConsumeClick(*Input::Binds::Use)) {
                    controller.StartUseItem();
                }
                controller.ContinueAttack(Input::IsDown(*Input::Binds::Attack));
                if (!Input::IsDown(*Input::Binds::Use)) controller.StopUseItem();
            };
#if ENABLE_IMMERSIVE_PORTALS
            // A click through a portal acts on the FAR level: the press
            // handlers predict placement, read blocks and write predictions
            // through the bound level, exactly as the controller's tick does
            // while the crosshair reaches through (see the Tick call).
            if (player.lastBlockHit && player.lastBlockHitPortalId != 0 &&
                player.lastBlockHitDimension != Client::ClientLevels::ActiveDimension()) {
                Client::ClientLevels::WithLevel(player.lastBlockHitDimension, [&]() {
                    controller.SetBlockAccess(Client::g_clientBlockAccess);
                    dispatch();
                });
                controller.SetBlockAccess(Client::g_clientBlockAccess);
            } else
#endif
            dispatch();
        }

        // Inventory selection
        // MC handleKeybinds:1897 — `while (keyHotbarSlots[i].consumeClick())`.
        for (int i = 0; i < 9; ++i) {
            // Drained-but-ignored while the free camera is detached, so a
            // number key pressed mid-flight neither switches slots now nor
            // fires a stale click on re-attach.
            while (Input::ConsumeClick(*Input::Binds::Hotbar[i])) {
                if (!freeCamDetached) controller.OnHotbarChanged(i);
            }
        }

        // Pick block (P key) — MC Minecraft.pickBlock: the client only says
        // what it aimed at (an entity under the crosshair beats the block
        // behind it); the server resolves the item and places it —
        // PlayerSession::HandlePickItem. Creative only in this game, and the
        // server enforces that too.
        if (Input::ConsumeClick(*Input::Binds::PickItem)) {
            const bool creative = player.gameModeKnown && player.gameMode == 1;
            if (!freeCamDetached && creative) {
                Network::PickItemC2SPacket pick;
                if (const int32_t entityId = controller.PickEntity(); entityId != 0) {
                    pick.kind = Network::PickItemC2SPacket::Kind::Entity;
                    pick.entityId = entityId;
                    controller.SendPickItem(pick);
                } else if (player.lastBlockHit.has_value()) {
                    pick.kind = Network::PickItemC2SPacket::Kind::Block;
                    pick.x = player.lastBlockHit->blockPos.x;
                    pick.y = player.lastBlockHit->blockPos.y;
                    pick.z = player.lastBlockHit->blockPos.z;
                    controller.SendPickItem(pick);
                }
            }
        }

        // Swap main/off hand (F) — MC's SWAP_ITEM_WITH_OFFHAND player action.
        // Gated on !cursorVisible so typing "f"/"q" into chat or the
        // inventory search box doesn't fire world actions.
        //
        // F is also half of the F+C free-camera chord. A press that lands
        // while C is already physically held is chord intent, not a swap —
        // consume it and do nothing, so forming the chord C-first never
        // swaps hands. (F-first can't be helped: the swap fired on F's own
        // press frame, before any chord existed.) Everything is likewise
        // consumed-and-dropped while already detached.
        if (!cursorVisible && Input::ConsumeClick(*Input::Binds::SwapOffhand)) {
            if (!freeCamDetached && !Input::IsKeyDown(Input::Key::C)) {
                controller.SendPlayerAction(Network::PlayerAction::SWAP_ITEM_WITH_OFFHAND);
            }
        }

        // Drop held item (Q) — MC's DROP_ITEM player action. No item-entity
        // system yet, so the server just shrinks the stack.
        if (!cursorVisible && Input::ConsumeClick(*Input::Binds::Drop)) {
            if (!freeCamDetached) {
                controller.SendPlayerAction(Network::PlayerAction::DROP_ITEM);
            }
        }

        // Mouse wheel for inventory scrolling. Dead while the free camera is
        // detached (the offsets are reset per frame, so skipping is enough).
        auto [scrollX, scrollY] = Input::GetScrollOffset();
        if (!freeCamDetached) {
            if (scrollY > 0) {
                controller.OnHotbarChanged((player.GetSelectedSlot() - 1 + 9) % 9);
            } else if (scrollY < 0) {
                controller.OnHotbarChanged((player.GetSelectedSlot() + 1) % 9);
            }
        }

        // Debug noclip toggle. Swallowed while the free camera is detached —
        // silently flipping the frozen player's physics mode mid-flight would
        // change what happens the moment control returns.
        if (Input::ConsumeClick(*Input::Binds::Noclip)) {
            if (!freeCamDetached) player.ToggleNoclip();
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
    // Native full-screen transitions animate for ~1 s; the tracker below must
    // not read the OLD state during that window and "correct" the setting.
    static double s_fullscreenSettleUntil = 0.0;
    static int s_windowedX = 0, s_windowedY = 0;
    static int s_windowedWidth = Config::WindowWidth, s_windowedHeight = Config::WindowHeight;

#ifdef __APPLE__
    // macOS: the NATIVE full screen (the window's own Space, what the green
    // button does), not GLFW's borderless window over the display. Game
    // Mode only engages for a game in a full-screen Space; GLFW's
    // monitor-mode window never qualified, so the game never got it. GLFW
    // tracks the transition itself (it observes the window's full-screen
    // notifications), so its size callbacks and swapchain recreation run as
    // for any resize. Plain ObjC runtime calls, as the launcher does for
    // its colour space — no Objective-C++ translation unit needed.
    namespace {
        bool MacIsNativeFullscreen(GLFWwindow* window) {
            id ns = glfwGetCocoaWindow(window);
            if (!ns) return false;
            const unsigned long mask =
                ((unsigned long (*)(id, SEL))objc_msgSend)(ns, sel_registerName("styleMask"));
            return (mask & (1ul << 14)) != 0;   // NSWindowStyleMaskFullScreen
        }
        void MacToggleNativeFullscreen(GLFWwindow* window) {
            id ns = glfwGetCocoaWindow(window);
            if (!ns) return;
            unsigned long behavior =
                ((unsigned long (*)(id, SEL))objc_msgSend)(ns, sel_registerName("collectionBehavior"));
            behavior |= (1ul << 7);             // NSWindowCollectionBehaviorFullScreenPrimary
            ((void (*)(id, SEL, unsigned long))objc_msgSend)(ns, sel_registerName("setCollectionBehavior:"), behavior);
            ((void (*)(id, SEL, id))objc_msgSend)(ns, sel_registerName("toggleFullScreen:"), nullptr);
        }
    }
#endif

#ifdef __APPLE__
    // Logs native full-screen transitions however they happen — F11, the
    // green button, Esc — so a session's log says whether the window was
    // ever in the state Game Mode needs. Cheap: one ObjC call, every 30th
    // frame.
    void MacTrackNativeFullscreen(GLFWwindow* window) {
        static int  s_counter = 0;
        static int  s_known   = -1;   // -1 unknown, else 0/1
        if ((++s_counter % 30) != 0) return;
        const int now = MacIsNativeFullscreen(window) ? 1 : 0;
        if (now == s_known) return;
        if (s_known != -1 || now == 1) {
            Log::Info("[GameMode] native full screen is now %s", now ? "ON (Game Mode eligible)" : "off");
        }
        s_known = now;
        // The green button, the View menu and macOS's own Esc change the
        // window behind ToggleFullscreen's back. Mirror them into the flag
        // and the saved option: entered with the green button, the options
        // screen showed "Fullscreen: OFF" and switching it on LEFT full
        // screen (ToggleFullscreen toggles the real window), and the next
        // launch came up windowed. Skipped while a ToggleFullscreen of our
        // own is still animating, so the old state is not written back.
        if (glfwGetTime() >= s_fullscreenSettleUntil && s_isFullscreen != (now == 1)) {
            s_isFullscreen = (now == 1);
            Platform::g_gameSettings.SetFullscreen(s_isFullscreen);
            Platform::g_gameSettings.Save();
        }
    }
#endif

    void ToggleFullscreen(GLFWwindow* window) {
#ifdef __APPLE__
        // The window's ACTUAL state, not the flag: the green button and Esc
        // leave native full screen behind our back.
        const bool actual = MacIsNativeFullscreen(window);
        if (!actual) {
            glfwGetWindowPos(window, &s_windowedX, &s_windowedY);
            glfwGetWindowSize(window, &s_windowedWidth, &s_windowedHeight);
        }
        MacToggleNativeFullscreen(window);
        s_isFullscreen = !actual;
        s_fullscreenSettleUntil = glfwGetTime() + 2.0;
        Log::Info(s_isFullscreen ? "Entering native full screen (Game Mode eligible)"
                                 : "Leaving native full screen");
        Platform::g_gameSettings.SetFullscreen(s_isFullscreen);
        Input::SaveKeyBindings();
        Platform::g_gameSettings.Save();
        return;
#endif
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
    // --world <name>: build the Singleplayer TitleAction SelectWorldScreen's
    // LaunchWorld would for the worlds.json entry of that name. False when no
    // entry matches, so the caller can fall back to the title screen.
    static bool DevHarnessWorldAction(const std::string& name, Render::TitleAction& out) {
        for (const Render::WorldEntry& e : Render::WorldList::Load()) {
            if (e.name != name) continue;
            Render::TitleAction a;
            a.kind = Render::TitleAction::Kind::Singleplayer;
            a.useMinecraftSave = e.isMinecraftSave;
            a.worldPath        = e.savePath;
            a.readOnlyWorld    = e.readOnly;
            a.worldName        = e.name;
            a.seed             = e.seed;
            a.gameMode         = e.gameMode;
            a.generateStructures = e.generateStructures;
            a.worldType   = e.worldType;
            a.flatPreset  = e.flatPreset;
            a.flatLayers  = e.flatLayers;
            a.singleBiome = e.singleBiome;
            a.worldgenTweaks  = e.worldgenTweaks;
            a.dayTime         = e.dayTime;
            a.doDaylightCycle = e.doDaylightCycle;
            a.difficulty      = e.difficulty;
            a.skybox          = e.skybox;
            a.skyboxMode      = e.skyboxMode;
            a.babyModels      = e.babyModels;
            out = std::move(a);
            return true;
        }
        Log::Warning("[Harness] --world: no worlds.json entry named \"%s\"; showing title", name.c_str());
        return false;
    }

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

        // If the last session ended because the server dropped us, say so —
        // on top of the title screen, which is the parent MC's
        // createDisconnectScreen falls back to. No-op after a normal quit.
        Render::ShowPendingDisconnectScreen();

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
            if (applied & Render::ScreenManager::APPLY_RESOURCE_PACKS) {
                ReloadResources();
            }
            if (applied & Render::ScreenManager::APPLY_MIPMAPS) {
                if (Render::g_atlasBuilder)
                    Render::g_atlasBuilder->SetMipmapLevels(Platform::g_gameSettings.GetMipmapLevels());
            }
            if (applied & Render::ScreenManager::APPLY_MESH_OPTIONS) {
                // No world: just publish, so the first mesh of the next
                // session is built with the new options.
                Render::Mesher::SyncMeshOptionsFromSettings();
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

        // Resource packs first: everything below reads assets, and a pack
        // has to be able to replace any of them. options.txt is loaded much
        // later (InitializeGameDirectorySystem), so the two lists are read
        // straight off the disk here, MC's Options.resourcePacks and
        // incompatibleResourcePacks.
        Resources::Initialize(
            Platform::GameDirectory::GetDefaultGameDirectory() + "/resourcepacks",
            GetVanillaAssetPath("assets"),
            Resources::ParsePackList(Platform::GameSettings::PeekStringFromDisk("resourcePacks", "[]")),
            Resources::ParsePackList(Platform::GameSettings::PeekStringFromDisk("incompatibleResourcePacks", "[]")));

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

        // Fill the shape caches now, on this thread, while nothing else is
        // running. AFTER Load, never between it and LoadModels — see the note
        // on the declaration. This removes every lazy population from the hot
        // paths, which is what lets multiple threads query shapes at once.
        Game::BlockRegistry::PrewarmShapeCaches();

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

        // End portal. Not part of the dispatcher above: it draws off a
        // per-chunk position index instead of block entities, because
        // world-generated portals never get one (see EndPortalRenderer.hpp).
        // Non-fatal — a failure leaves the portal invisible, as it is today.
        if (!Render::g_endPortalRenderer.Initialize()) {
            Log::Warning("End portal renderer init failed — end portals will not render");
        }

        // Vanilla first-person held-item renderer. Always enabled —
        // independent of the portal feature flag. Non-fatal failure:
        // if shaders don't load, nothing renders in the hand slot.
        if (!Render::g_heldItemRenderer.Initialize()) {
            Log::Warning("Held item renderer init failed — hotbar items will not appear in hand");
        }

        // Mob/world particle system (hearts, smoke, explosions, spell
        // swirls). Non-fatal failure: mobs just emit nothing.
        if (!Render::g_mobParticleSystem.Initialize()) {
            Log::Warning("Mob particle system init failed — mob particles will not draw");
        }

#if ENABLE_IMMERSIVE_PORTALS
        // Immersive portals: the recursive see-through pass. Non-fatal — a
        // failure leaves portals invisible, the rest of the game runs.
        if (!Render::g_immersivePortalRenderer.Initialize()) {
            Log::Warning("Immersive portal renderer init failed — portals will not be see-through");
        }
#endif
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
                if (Client::GetClientPortalManager().IsBlockBehindActivePortal(x, y, z, aabb)) {
                    return true;
                }
#if ENABLE_IMMERSIVE_PORTALS
                return Client::ImmersivePortalCollision::PassthroughHook(x, y, z, aabb);
#else
                return false;
#endif
            });
#elif ENABLE_IMMERSIVE_PORTALS
        Game::SetPortalPassthroughFn(&Client::ImmersivePortalCollision::PassthroughHook);
#endif
#if ENABLE_IMMERSIVE_PORTALS
        // Cross-portal collision, the other direction: the far side of a
        // portal the player is stepping into is solid where its world is.
        Game::SetPortalExtraSolidFn(&Client::ImmersivePortalCollision::ExtraSolidHook);
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
        // MC Screen.handleComponentClicked: RunCommand sends the value the way
        // a typed line goes (the server routes a leading '/' to its
        // dispatcher); SuggestCommand opens the chat with the value in it.
        Render::SetRunCommandHandler([](const std::string& command) {
            if (Client::g_networkClient && Client::g_networkClient->IsConnected()) {
                if (auto conn = Client::g_networkClient->GetConnection()) conn->SendChatMessage(command);
            }
        });
        Render::SetSuggestCommandHandler([](const std::string& command) {
            g_chatScreen.Open(false);
            g_chatScreen.InsertText(command);
        });
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
                switch (s.click) {
                    case Network::ChatClickAction::CopyToClipboard: seg.click = Render::ChatClickAction::CopyToClipboard; break;
                    case Network::ChatClickAction::RunCommand:      seg.click = Render::ChatClickAction::RunCommand;      break;
                    case Network::ChatClickAction::SuggestCommand:  seg.click = Render::ChatClickAction::SuggestCommand;  break;
                    default:                                        seg.click = Render::ChatClickAction::None;            break;
                }
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
        // Stamp the client main thread before anything else, so
        // ASSERT_CLIENT_THREAD is armed for the whole run. Counterpart to
        // IntegratedServer stamping g_serverThreadId — together they are what
        // keep packet handlers on the thread that owns the state they touch.
        Client::g_clientThreadId = std::this_thread::get_id();

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
        bool vanillaPortals = false;   // --vanilla-portals: block portals instead of immersive ones
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
        // ── Dev harness (scripted runs for profiling) ─────────────────────
        //   --world <name>        skip the title screen, load this worlds.json
        //                         entry as singleplayer (first session only)
        //   --exec "<cmd>"        send this chat line/command once the session
        //                         is up (repeatable, sent in order)
        //   --exec-delay <sec>    seconds after session start before --exec
        //                         lines are sent (default 8)
        //   --quit-after <sec>    close the game this many seconds after
        //                         session start (0 = never)
        //   --env NAME=VALUE      set an environment variable at startup (the
        //                         OBEY_* switches, through play.sh's launch)
        //   --record <name>       record the player's pose every frame from
        //                         the moment the level has loaded, saved to
        //                         <obeycraft>/recordings/<name>.rec when the
        //                         session ends (in-game: "/record <name>",
        //                         "/record stop")
        //   --replay <name>       drive the player along that recording by
        //                         time — same views at the same seconds at
        //                         any frame rate — and quit when it ends
        //                         (unless --quit-after is set). In-game:
        //                         "/replay <name>", "/replay stop".
        //   --replay-hold <sec>   how long the player is parked on the
        //                         recording's first pose before playback
        //                         starts, so the chunks around it stream in
        //                         the same way every run (default 8)
        // See src/client/dev/SessionReplay.hpp for what a replay does and
        // does not reproduce (portal crossings yes, block breaking no).
        // While active, a "[Harness]" line is logged every second with the
        // client and server entity counts, so time-to-done can be read from
        // the log without a profiler attached.
        std::string devWorldName;
        std::string devRecordName;
        std::string devReplayName;
        double      devReplayHoldSec = 8.0;
        std::vector<std::string> devExecCommands;
        // --exec-late "<cmd>": sent 5 s before --quit-after fires.
        std::vector<std::string> devExecLateCommands;
        // --exec-at <sec> "<cmd>": sent once, this many seconds after session
        // start (repeatable; for timed sequences like "tp far, then tp back").
        std::vector<std::pair<double, std::string>> devExecAtCommands;
        double devExecDelaySec  = 8.0;
        double devQuitAfterSec  = 0.0;
        // --remesh-at <sec>: mark every active client section dirty once, this
        // many seconds after session start (0 = never). Diagnostic for meshes
        // that were built before a neighbour chunk arrived: if gpuSections in
        // the [HarnessChunks] line drops after the remesh, the earlier meshes
        // were stale.
        double devRemeshAtSec   = 0.0;
        bool   devRemeshDone    = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--world" && i + 1 < argc) {
                devWorldName = argv[++i];
                continue;
            }
            if (arg == "--exec" && i + 1 < argc) {
                devExecCommands.emplace_back(argv[++i]);
                continue;
            }
            if (arg == "--exec-late" && i + 1 < argc) {
                devExecLateCommands.emplace_back(argv[++i]);
                continue;
            }
            if (arg == "--exec-at" && i + 2 < argc) {
                const double at = std::atof(argv[++i]);
                devExecAtCommands.emplace_back(at, argv[++i]);
                continue;
            }
            if (arg == "--exec-delay" && i + 1 < argc) {
                devExecDelaySec = std::atof(argv[++i]);
                continue;
            }
            if (arg == "--quit-after" && i + 1 < argc) {
                devQuitAfterSec = std::atof(argv[++i]);
                continue;
            }
            // --env NAME=VALUE: set an environment variable before any system
            // reads it. The OBEY_* kill switches (OBEY_NO_FACE_CULL,
            // OBEY_NO_TWO_SIDED, OBEY_MESH_CENSUS, ...) are read lazily, so
            // this makes them usable from tools/play.sh, whose LaunchServices
            // launch drops the caller's environment.
            if (arg == "--env" && i + 1 < argc) {
                const std::string kv = argv[++i];
                const size_t eq = kv.find('=');
                const std::string name = kv.substr(0, eq);
                const std::string value = (eq == std::string::npos) ? "1" : kv.substr(eq + 1);
#ifdef _WIN32
                _putenv_s(name.c_str(), value.c_str());
#else
                setenv(name.c_str(), value.c_str(), 1);
#endif
                Log::Info("--env %s=%s", name.c_str(), value.c_str());
                continue;
            }
            if (arg == "--remesh-at" && i + 1 < argc) {
                devRemeshAtSec = std::atof(argv[++i]);
                continue;
            }
            if (arg == "--record" && i + 1 < argc) {
                devRecordName = argv[++i];
                continue;
            }
            if (arg == "--replay" && i + 1 < argc) {
                devReplayName = argv[++i];
                continue;
            }
            if (arg == "--replay-hold" && i + 1 < argc) {
                devReplayHoldSec = std::atof(argv[++i]);
                continue;
            }
            if (arg == "--vulkan") {
                useVulkan = true;
                Log::Info("Vulkan backend requested via --vulkan flag");
            }
            if (arg == "--vanilla-portals") {
                vanillaPortals = true;
                Log::Info("Vanilla (block) nether portals requested via --vanilla-portals");
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
            s_sentryActive.store(true);
            // Fallback for the early-return exit paths only. The normal path
            // closes explicitly at the end of Run(); see CloseSentryOnce.
            std::atexit([]() { CloseSentryOnce(); });
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

        // Video Settings "Retina Resolution" (macOS). A window-creation hint,
        // so it is read straight off options.txt here — the settings object
        // is not loaded until InitializeGameDirectorySystem, hundreds of
        // lines below. Off on Intel Macs by default: their integrated GPUs
        // pay four times the fragment work for the Retina framebuffer out
        // of shared system memory, and that is the single largest GPU cost
        // on those machines. Apple Silicon keeps Retina, as it always has.
        const bool retinaFramebuffer = Platform::GameSettings::PeekRetinaFramebufferFromDisk();
#ifdef __APPLE__
        Log::Info("Retina framebuffer: %s", retinaFramebuffer ? "on" : "off");
#else
        (void)retinaFramebuffer;
#endif

        // Setup graphics API context based on backend choice
        if (useVulkan) {
#ifdef HAS_VULKAN
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan manages its own context
    #ifdef __APPLE__
            glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, retinaFramebuffer ? GLFW_TRUE : GLFW_FALSE);
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
            glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, retinaFramebuffer ? GLFW_TRUE : GLFW_FALSE);
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
#ifdef __APPLE__
        // What macOS Game Mode will see: the running bundle's category. It
        // engages only for a games category in native full screen, and a
        // process launched from a bundle without the key (an older
        // installed copy, a bare executable) never gets it — this line is
        // the first thing to check when the menu-bar icon says "off".
        {
            const char* category = "(none)";
            std::string categoryStorage;
            bool supportsGameMode = false;
            if (CFBundleRef bundle = CFBundleGetMainBundle()) {
                if (CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("LSApplicationCategoryType"))) {
                    char buf[128] = {0};
                    if (CFGetTypeID(v) == CFStringGetTypeID() &&
                        CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof(buf), kCFStringEncodingUTF8)) {
                        categoryStorage = buf;
                        category = categoryStorage.c_str();
                    }
                }
                if (CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("GCSupportsGameMode"))) {
                    supportsGameMode = CFGetTypeID(v) == CFBooleanGetTypeID() &&
                                       CFBooleanGetValue(static_cast<CFBooleanRef>(v));
                }
            }
            Log::Info("[GameMode] bundle category=%s GCSupportsGameMode=%d (needs a games category + native full screen)",
                      category, supportsGameMode ? 1 : 0);
        }
#endif

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
        // Options.loadSelectedResourcePacks dropped packs that no longer
        // exist or are no longer compatible when the lists were read early;
        // options.txt takes the cleaned lists.
        {
            const Resources::OptionLists lists = Resources::CurrentOptionLists();
            Platform::g_gameSettings.SetResourcePacks(Resources::SerializePackList(lists.selected));
            Platform::g_gameSettings.SetIncompatibleResourcePacks(Resources::SerializePackList(lists.incompatible));
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

        // Same too-early problem for the block atlas, which was built with
        // the compiled-in mipmap depth (4) before options.txt was read. A
        // saved Mipmap Levels other than 4 rebuilds the chain here, once;
        // the common case is a no-op.
        if (Render::g_atlasBuilder) {
            Render::g_atlasBuilder->SetMipmapLevels(Platform::g_gameSettings.GetMipmapLevels());
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
        const bool devAutoWorld = !devWorldName.empty() && firstSession && !isRemoteClient;
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
        } else if (devAutoWorld && DevHarnessWorldAction(devWorldName, titleAction)) {
            // --world: the matching worlds.json entry, launched exactly as
            // SelectWorldScreen would, without the title screen. An unknown
            // name logs and falls through to the normal title screen.
            Log::Info("[Harness] --world: loading \"%s\" without the title screen",
                      devWorldName.c_str());
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
                Render::g_endPortalRenderer.Shutdown();
#if ENABLE_IMMERSIVE_PORTALS
        Render::g_immersivePortalRenderer.Shutdown();
#endif
                Render::SkyboxThumbnails::Get().Shutdown();   // preview cards, before the backend goes
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

        // Dropped items, orbs, mobs and falling blocks live in the level
        // (ClientLevel.hpp) — created with it below, before the connection
        // opens, so a spawn packet on the first tick has somewhere to land.
        Render::MobRenderer mobRenderer;
        if (!mobRenderer.Initialize()) {
            Log::Warning("[PlatformMain] mob renderer failed to initialize — mobs will be invisible");
        }
        Render::ItemEntityRenderer itemEntityRenderer;
        if (!itemEntityRenderer.Initialize()) {
            Log::Warning("Failed to initialize item entity renderer, dropped items won't be visible");
        }
        // Falling blocks and primed TNT. A separate renderer from MobRenderer
        // because those two ARE blocks, and MobRenderer is built around
        // ModelPart skeletons; this one reuses the item renderer's block-model
        // path so a falling cobblestone and a dropped one are one mesh builder.
        if (!Render::g_blockCubeEntityRenderer.Initialize()) {
            Log::Warning("Failed to initialize block-entity renderer, "
                         "falling blocks and TNT won't be visible");
        }
        if (!Render::g_fillPreviewRenderer.Initialize()) {
            Log::Warning("Failed to initialize the fill preview renderer");
        }
        Render::XpOrbRenderer xpOrbRenderer;
        if (!xpOrbRenderer.Initialize()) {
            Log::Warning("Failed to initialize XP orb renderer, experience orbs won't be visible");
        }

        // === MINECRAFT-STYLE ARCHITECTURE INITIALIZATION ===
        Log::Info("Initializing Minecraft Java Edition Architecture...");

        Game::World* world = nullptr;

        // Fresh session: drop any stale time from a previous world so the sky
        // doesn't flash the old time before the first TimeUpdate arrives, and
        // apply this world's skybox (multiplayer joins default to vanilla).
        Render::EnvironmentState::Get().ResetSession();
        Render::g_skyRenderer.SetSkybox(titleAction.skybox, titleAction.skyboxMode);
        // This world's baby look (multiplayer joins and Minecraft saves get
        // the default, New, for the session).
        Render::SetBabyModelLook(titleAction.babyModels == "classic"
                                     ? Render::BabyModelLook::Classic
                                     : Render::BabyModelLook::New);
        // A resource pack selection saved with this world (Options → Resource
        // Packs while in it) stands in for the global one for the session.
        if (!isRemoteClient && !titleAction.useMinecraftSave && !titleAction.worldName.empty()) {
            std::string reason;
            if (auto root = Game::Anvil::RootForWorldName(titleAction.worldName, reason)) {
                const Game::Anvil::WorldSidecar sidecar = Game::Anvil::ReadWorldSidecar(root->Root().string());
                if (sidecar.hasResourcePacks) {
                    Resources::SelectFromOptionLists(sidecar.resourcePacks, sidecar.incompatibleResourcePacks);
                    ReloadResources();
                }
            }
        }
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
            serverConfig.immersivePortals = !vanillaPortals;
            serverConfig.worldWrapSize    = titleAction.worldWrap;
            serverConfig.dimensionStack   = titleAction.dimensionStack;
            serverConfig.difficulty       = std::clamp(titleAction.difficulty, 0, 3);

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

            // Where an ObeyCraft world persists. Only for worlds that are OURS:
            // an imported Minecraft save is loaded read-only and never gets a
            // save path, which is the difference between the two fields.
            //
            // The folder is created by IntegratedServer::Initialize, so a world
            // made before this existed picks one up the first time it is opened
            // — same seed, same terrain, and from then on it saves.
            if (!titleAction.useMinecraftSave && !titleAction.worldName.empty()) {
                std::string reason;
                const std::string folder =
                    Game::Anvil::SanitiseFolderName(titleAction.worldName);
                serverConfig.savePath =
                    Platform::g_gameDirectory.GetSavesDirectory() + "/" + folder;
                serverConfig.worldDisplayName = titleAction.worldName;
                serverConfig.worldSeed        = titleAction.seed;
                Log::Info("✓ World saves to: %s", serverConfig.savePath.c_str());
            }

            // MC IntegratedServer.java:69 — the player who launched this
            // process is the singleplayer owner, and is exempt from keep-alive
            // and the read timeout. Passed raw: an empty --name means "use the
            // server's own default", which the server resolves at login where
            // kDefaultPlayerName and the collision policy both live. Only the
            // isRemoteClient path leaves this unset, and that path has no
            // integrated server at all.
            serverConfig.singleplayerProfileName = playerName;
            serverConfig.hasSingleplayerOwner    = true;

            Server::InitializeIntegratedServer(serverConfig);
            Log::Info("✓ IntegratedServer initialized (20 TPS, world created on server)");

            // Get world reference for legacy systems (temporary)
            world = Server::g_integratedServer->GetWorld();
            Game::g_world = world;

            // Seed the generator for created worlds BEFORE any chunk
            // generation kicks off (server thread starts further down).
            if (!titleAction.useMinecraftSave) {
                world->SetGenerationSeed(titleAction.seed);
                // After the seed (these push the whole generation config to
                // the generator).
                static const char* kWorldTypeIds[] = {
                    "default", "flat", "large_biomes", "amplified", "single_biome_surface"};
                const char* worldTypeId =
                    (titleAction.worldType >= 0 && titleAction.worldType <= 4)
                        ? kWorldTypeIds[titleAction.worldType] : "default";
                world->SetWorldGenOptions(worldTypeId, titleAction.flatPreset,
                                          titleAction.flatLayers, titleAction.singleBiome);
                world->SetWorldGenTweaks(titleAction.worldgenTweaks);
                world->SetGenerateStructures(titleAction.generateStructures);
                Log::Info("World '%s': procedural generation from seed %d (%s, type %s, structures %s)",
                          titleAction.worldName.c_str(), titleAction.seed,
                          titleAction.gameMode == 0 ? "Survival" : "Creative",
                          worldTypeId,
                          titleAction.generateStructures ? "on" : "off");
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
            const size_t hw = Core::HardwareProfile::Get().logicalCores;
            // 2026-08-29: doubled. Mesh throughput was pinned at permits x fps
            // (~900 sections/s at 57 fps) while the mesh workers idled 87%,
            // so a 25k-section backlog after a far teleport took 30 s to
            // drain. The per-section upload also got cheaper (translucent
            // sort moved to the worker), which is what pays for the extra
            // per-frame upload work.
            // 2026-08-30: MC has no upload permit at all — uploadAllPendingUploads
            // drains everything each frame and the only back-pressure is its
            // SectionBufferBuilderPool, sized from memory (maxMemory*0.3 /
            // pack size = hundreds of packs). 128 in flight here (~10 MB of
            // finished meshes) approximates that; the drain is already
            // "upload all pending".
            //
            // Small machines: the 128 floor is the M4 number — ~10 MB of
            // finished meshes uploaded in ONE frame if they all land at once,
            // which a 2-core Intel laptop's GL driver cannot stage in a frame
            // the way an M4's does. Scaled by core count below 8 threads
            // (16 per thread: 32 on a 2-thread, 64 on a 4-thread, 96 on a
            // 6-thread machine); 8 threads and up keep the exact formula
            // they had, so the tuned machines are untouched.
            const size_t permitCount = hw >= 8 ? std::max<size_t>(128, hw * 8)
                                               : std::max<size_t>(32, hw * 16);
            Render::GetMeshUploadPermits().Initialize(permitCount);
            Log::Info("✓ Mesh upload permits: %zu (%zu logical cores)", permitCount, hw);
        }

        // ClientBlockAccess is needed in BOTH modes. Besides being the remote
        // client's physics/raycast source, it is the ILevelWrite that item
        // behaviours (hoe, shovel, bucket, …) write through when the client
        // runs them for prediction — and prediction always targets the CLIENT
        // chunk cache, never the server World, because that cache is what the
        // renderer meshes from.
        // 4. Initialize client-side systems (always needed)
        // Publish cutoutLeaves / cullLeaves / smooth lighting / biome blend to
        // the mesher before any section is built, so a saved option is
        // honoured from the first mesh, not only after the user touches the
        // options screen.
        Render::Mesher::SyncMeshOptionsFromSettings();
        // One ClientLevel per dimension (chunk manager, mesh manager, chunk
        // renderer, block view, entity managers, portals). The overworld is
        // the session's first level; others appear when their packets do.
        if (!Client::ClientLevels::CreateSession(Game::DimensionId::Overworld)) {
            Log::Error("Failed to initialize the client level");
            return -7;
        }
        Log::Info("✓ Client systems initialized (client level: chunk manager, mesh manager, chunk renderer)");

        // MC ClientPacketListener.startWaitingForNewLevel, on entering a level.
        // Arms the client-side readiness watch; once the player's own section
        // compiles it sends PlayerLoadedC2S, which is what lifts the server's
        // interaction gate (PlayerSession::HasClientLoaded).
        Client::g_levelLoadTracker.StartClientLoad();

        // 5. Initialize player and controller
        Game::ClientPlayer player;
        player.color = playerColor;
#if ENABLE_IMMERSIVE_PORTALS
        // Last frame's eye, the start of the segment the immersive-portal
        // crossing test walks each frame (see ImmersivePortalTraveler).
        glm::dvec3 immersiveLastEye(0.0);
#endif // from --color CLI arg parsed earlier
        Game::ClientPlayerController playerController;
        playerController.SetPlayer(&player);
        Render::SetInventoryScreenPlayer(&player);
        // Interaction block reads come from the CLIENT chunk cache in both
        // modes, for the same reason physics and raycasting do (see the note
        // above `blockAccessForPhysics`). The host used to read the server's
        // `Game::World*` here, which is the OVERWORLD's and never follows the
        // player: in the Nether or the End every mining lookup resolved
        // against overworld chunks at the same coordinates, so the crosshair
        // block read back as Air and the dig was dropped on the floor
        // (`CreativeDestroy` bails on air; survival cached an air hardness).
        // Placement was unaffected because it goes through SendUseItemOn and
        // predicts into the client cache — hence "can place but not break".
        playerController.SetBlockAccess(Client::g_clientBlockAccess);

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

        // 7. Rendering systems — the chunk renderer is part of the level now.

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

        // Physics and raycasting read the CLIENT's chunk cache, in both modes.
        //
        // A host used to read the server's Game::World directly, which was
        // fine while there was exactly one of them and is a bug now that there
        // are three: that pointer is the OVERWORLD's and never follows the
        // player, so walking into the Nether or the End left the host
        // colliding with, and mining, overworld blocks at the same
        // coordinates. In the End — where the overworld is solid stone at the
        // arrival height — that presents as the player having no physics at
        // all.
        //
        // The client cache is dimension-correct by construction (the dimension
        // change wipes it and the destination refills it), it is already what
        // a remote client uses, it is what the renderer meshes from, and it is
        // what MC does: the client always reads its own ClientLevel and never
        // the server's. The one property it gives up is that the host no
        // longer sees chunks the server has but has not sent yet — which is
        // exactly the constraint every other client already lives under.
        // Both read the ACTIVE level's block view; ClientLevels rebinds the
        // raycast global on every level switch, and physics reads the global
        // each frame (see the physics step).
        Game::SetGlobalBlockAccess(Client::g_clientBlockAccess);
        
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
        // Set from the network I/O thread when the server drops us mid-session;
        // consumed by the frame loop below, which turns it into returnToTitle.
        auto serverDropped = std::make_shared<std::atomic<bool>>(false);
        // Why, for the DisconnectedScreen. Published under serverDropped's
        // release store — see the callback.
        auto dropReason = std::make_shared<std::string>();

        networkClient->SetOnConnected([connected, connectionComplete]() {
            Log::Info("✓ Connection established");
            *connected = true;
            *connectionComplete = true;
        });

        networkClient->SetOnError([connectionComplete](const std::string& error) {
            Log::Error("Connection failed: %s", error.c_str());
            *connectionComplete = true;
        });

        // Losing the connection ends the SESSION, not the process.
        //
        // MC ClientCommonPacketListenerImpl.onDisconnect (:348) calls
        //   this.minecraft.disconnect(this.createDisconnectScreen(details), ...)
        // and createDisconnectScreen (:368) falls back to `new TitleScreen()`.
        // Vanilla never closes the game when a server drops you.
        //
        // This used to call glfwSetWindowShouldClose(window, GLFW_TRUE), which
        // both broke the frame loop AND made the outer session loop's
        // `returnToTitle && !glfwWindowShouldClose(window)` test fail — so a
        // kicked client quit to desktop. Signal instead: the flag is polled by
        // the frame loop, which is also what keeps this off the GLFW API from
        // the network I/O thread this callback runs on.
        networkClient->SetOnDisconnected([serverDropped, dropReason](const std::string& reason) {
            Log::Info("Disconnected from server: %s — returning to title", reason.c_str());
            // Write the reason BEFORE publishing the flag; the frame loop reads
            // it only after an acquire on that flag, so the release/acquire
            // pair is what makes the plain string safe across the two threads.
            *dropReason = reason;
            serverDropped->store(true, std::memory_order_release);
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
        // Remembered for the debug panel: this is the one place that knows,
        // without asking the friends service again, whether this session is
        // being relayed.
        const bool sessionUsedRelay = !titleAction.relayTicket.empty();
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

        // === Debug detached free camera (F+C chord) =====================
        // A culling-verification tool: while active, `freeCam` is flown with
        // the normal look/move binds and the render phase swaps its pose into
        // `camera`, but EVERY cull decision (terrain frustum filter, occlusion
        // BFS origin, translucent sort origin, entity visible-section gating)
        // keeps coming from the frozen player view via
        // ChunkRenderer::SetCullOverride — fly outside the player's frustum
        // and see exactly what the culler kept. The player stands still and
        // its frozen position/rotation keep going out in PlayerMoveC2S, so
        // chunk loading (server-driven) and mesh scheduling (player-position-
        // driven) are untouched. Session-local: leaving the world drops it.
        bool freeCamActive = false;
        Render::Camera freeCam;   // physicsControlled=false → Update() flies it

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
        // What the server last heard of the player. Physics runs per FRAME
        // while the position goes out per TICK, so between two sends the
        // server's copy of the player trails the client's by up to 50 ms.
        // MC never has that gap on a click: its click and its position
        // packet come out of the same tick, so the server judges "is the
        // player standing in this block" against the same feet the client
        // predicted with. Here a jump-and-place clicked as soon as the feet
        // cleared the block was accepted by the client and refused by the
        // server, which still had the feet inside the cell — the block
        // appeared, then vanished. sendPlayerMove(false) closes the gap on
        // demand: the controller calls it right before every interaction
        // packet, and it sends nothing when the server is already current.
        glm::vec3 lastSentMovePos(1e30f);
        glm::vec2 lastSentMoveRot(0.0f);
        bool      lastSentOnGround = false;
        auto sendPlayerMove = [&](bool force) {
            // Same gate as the player physics, and for the same reason MC
            // puts sendPosition() inside its hasClientLoaded() guard
            // (LocalPlayer.tick:228): a position produced before the world
            // exists is not a position worth telling the server about.
            if (!networkClient || !networkClient->IsConnected() ||
                !Client::g_levelLoadTracker.IsLoaded()) {
                return;
            }
            const glm::vec3 playerPos = player.physics.position;
            const glm::vec2 rotation(camera.yaw, camera.pitch);
            if (!force && playerPos == lastSentMovePos && rotation == lastSentMoveRot &&
                player.physics.isOnGround == lastSentOnGround) {
                return;
            }
            Network::PlayerMoveC2SPacket movePacket;
            movePacket.position = playerPos;
            movePacket.rotation = rotation;
            movePacket.onGround = player.physics.isOnGround;
            movePacket.isCrouching = Input::IsDown(*Input::Binds::Sneak);
            movePacket.isSprinting = player.physics.isSprinting;
            movePacket.jumpedThisTick = player.jumpedSinceMoveSend;
            player.jumpedSinceMoveSend = false;
            movePacket.fallDistance = player.landedFallSinceMoveSend;
            player.landedFallSinceMoveSend = 0.0f;
            movePacket.sequenceNumber = ++playerMoveSequence;
            movePacket.dimensionId = static_cast<int8_t>(
                Game::DimensionToRaw(Client::ClientLevels::ActiveDimension()));
            movePacket.timestamp = std::chrono::steady_clock::now();
            lastSentMovePos  = playerPos;
            lastSentMoveRot  = rotation;
            lastSentOnGround = player.physics.isOnGround;
            networkClient->GetConnection()->SendPlayerMove(movePacket);
        };
        playerController.SetMovementFlush([&sendPlayerMove]() { sendPlayerMove(false); });
        // Is a world-stopping screen up right now? Read by the client tick
        // below as MC reads `Minecraft.pause` (Minecraft.java:1741,1757,1809).
        bool localPaused = false;
        // Last pause state reported to the server (PlayerPauseC2SPacket).
        // Session-scoped rather than static: a new session starts with the
        // server assuming "not paused", so the cache has to start there too or
        // the first pause after a rejoin would be swallowed as "no change".
        bool sentPaused = false;

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

        // Dev harness state (see the --exec / --quit-after flags in Run()).
        const auto harnessStart      = std::chrono::steady_clock::now();
        const bool harnessActive     = !devExecCommands.empty() || devQuitAfterSec > 0.0 ||
                                       !devExecAtCommands.empty() || !devReplayName.empty();
        // Pose recording / replay (src/client/dev/SessionReplay.hpp). The
        // CLI names are consumed here so a later session (quit to title,
        // load another world) starts clean; the chat commands can start
        // either at any time.
        Client::Dev::PoseRecorder poseRecorder;
        Client::Dev::PoseReplayer poseReplayer;
        std::string poseRecordPending = devRecordName;   // started once the level has loaded
        devRecordName.clear();
        // --replay quits the game when the recording ends (that is the
        // profiling run); a --quit-after on the command line wins, and a
        // "/replay" typed in chat never quits.
        bool poseReplayQuitWhenDone = false;
        if (!devReplayName.empty()) {
            poseReplayQuitWhenDone = poseReplayer.Load(devReplayName, devReplayHoldSec) &&
                                     devQuitAfterSec <= 0.0;
            devReplayName.clear();
        }
        const std::string poseWorldName = titleAction.worldName.empty()
            ? (isRemoteClient ? remoteServerAddress : std::string("world")) : titleAction.worldName;
        std::vector<bool> harnessAtSent(devExecAtCommands.size(), false);
        bool       harnessExecSent   = false;
        bool       harnessLateSent   = false;
        int        harnessLastLogSec = -1;
        int        harnessFrames     = 0;
        double     harnessWorstFrame = 0.0;
        auto       harnessPrevFrame  = std::chrono::steady_clock::now();

        while (!glfwWindowShouldClose(window) && !returnToTitle) {
            frameStartTime = std::chrono::high_resolution_clock::now();

            if (harnessActive) {
                const auto harnessNow = std::chrono::steady_clock::now();
                const double elapsed =
                    std::chrono::duration<double>(harnessNow - harnessStart).count();
                // Frame pacing, as the player experiences it — the zone stats
                // average away exactly the multi-second stalls that matter.
                ++harnessFrames;
                harnessWorstFrame = std::max(harnessWorstFrame,
                    std::chrono::duration<double, std::milli>(harnessNow - harnessPrevFrame).count());
                harnessPrevFrame = harnessNow;
                if (!harnessExecSent && elapsed >= devExecDelaySec &&
                    networkClient && networkClient->IsConnected()) {
                    if (auto conn = networkClient->GetConnection()) {
                        // Hover a few blocks up, flying: a creative flyer is
                        // excluded from explosion knockback (see
                        // Explosion.cpp HurtEntities), so the player stays put
                        // and keeps the pile's chunk loaded instead of being
                        // launched out of tracking range. The controller's
                        // dirty check ships the flag to the server.
                        if (player.physics.mayFly && !player.physics.isFlying) {
                            player.physics.isFlying = true;
                            player.physics.position.y += 4.0;
                            player.physics.velocity = glm::dvec3(0.0);
                            Log::Info("[Harness] flight enabled, hovering at y=%.1f",
                                      player.physics.position.y);
                        }
                        for (const std::string& cmd : devExecCommands) {
                            Log::Info("[Harness] t=%.2fs exec: %s", elapsed, cmd.c_str());
                            conn->SendChatMessage(cmd);
                        }
                        harnessExecSent = true;
                    }
                }
                if (networkClient && networkClient->IsConnected()) {
                    if (auto conn = networkClient->GetConnection()) {
                        for (size_t k = 0; k < devExecAtCommands.size(); ++k) {
                            if (harnessAtSent[k] || elapsed < devExecAtCommands[k].first) continue;
                            Log::Info("[Harness] t=%.2fs exec-at: %s", elapsed,
                                      devExecAtCommands[k].second.c_str());
                            conn->SendChatMessage(devExecAtCommands[k].second);
                            harnessAtSent[k] = true;
                        }
                    }
                }
                if (devRemeshAtSec > 0.0 && !devRemeshDone && elapsed >= devRemeshAtSec
                    && Render::g_clientMeshManager && Client::g_clientChunkManager) {
                    devRemeshDone = true;
                    // Collect first: MarkSectionDirty takes the chunk manager's
                    // locks and ForEachActiveSection holds the mesh manager's.
                    std::vector<Render::ClientMeshManager::SectionKey> keys;
                    Render::g_clientMeshManager->ForEachActiveSection(
                        [&keys](const Render::ClientMeshManager::SectionKey& key,
                                const Render::GPUSectionData*) { keys.push_back(key); });
                    for (const auto& key : keys)
                        Client::g_clientChunkManager->MarkSectionDirty(key.chunkPos, key.sectionY);
                    Log::Info("[Harness] t=%.2fs remesh-all: %zu sections marked dirty",
                              elapsed, keys.size());
                }
                const int sec = static_cast<int>(elapsed);
                if (sec != harnessLastLogSec) {
                    harnessLastLogSec = sec;
                    const size_t clientMobs =
                        (Client::g_clientMobManager ? Client::g_clientMobManager->Count() : 0) +
                        (Client::g_clientFallingBlocks ? Client::g_clientFallingBlocks->Count() : 0);
                    int serverTnt = -1, serverFalling = -1;
                    long serverItems = -1;
                    if (!isRemoteClient && Server::g_integratedServer) {
                        if (auto* mobs = Server::g_integratedServer->Overworld().Mobs()) {
                            serverTnt = mobs->CountForType(
                                static_cast<uint16_t>(Game::EntityTypeId::Tnt));
                            serverFalling = mobs->CountForType(
                                static_cast<uint16_t>(Game::EntityTypeId::FallingBlock));
                        }
                        if (auto* store = Server::g_integratedServer->Overworld().FallingBlocks()) {
                            serverFalling += static_cast<int>(store->Count());
                        }
                        if (auto* items = Server::g_integratedServer->Overworld().Items()) {
                            serverItems = static_cast<long>(items->Count());
                        }
                    }
                    Log::Info("[Harness] t=%ds clientMobs=%zu serverTnt=%d serverFalling=%d "
                              "serverItems=%ld fps=%d worstFrameMs=%.0f",
                              sec, clientMobs, serverTnt, serverFalling, serverItems,
                              harnessFrames, harnessWorstFrame);
                    poseReplayer.LogProgress();
                    // Chunk streaming health: where chunks are in the pipeline
                    // (server cache → sent to client → client cache → meshed).
                    {
                        size_t srvChunks = 0, srvPending = 0, sent = 0, genJobs = 0, libChunks = 0;
                        if (!isRemoteClient && Server::g_integratedServer) {
                            if (auto* w = Server::g_integratedServer->Overworld().World())
                                if (auto* cp = w->GetChunkProvider()) srvChunks = cp->GetLoadedChunkCount();
                            srvPending = Server::g_integratedServer->GetPendingChunkLoadCount();
                            if (auto* gen = Server::g_integratedServer->Overworld().TerrainGenerator()) {
                                libChunks = gen->LibraryChunkCount();
                                if (sec % 5 == 0) {
                                    auto d = gen->GetUnloadDiag();
                                    Log::Info("[HarnessLib] t=%ds holders=%zu aboveMax=%zu pendingUnload=%zu refHeld=%zu pinned=%zu",
                                              sec, libChunks, d.aboveMax, d.pendingUnload, d.refHeld, d.pinned);
                                }
                            }
                            if (auto sess = Server::g_integratedServer->GetPlayerSession())
                                sent = sess->GetSentChunkCount();
                            if (Threading::g_serverWorkerPool)
                                genJobs = Threading::g_serverWorkerPool->GetPendingJobCount();
                        }
                        const size_t cliChunks = Client::g_clientChunkManager
                            ? Client::g_clientChunkManager->GetLoadedChunkCount() : 0;
                        const size_t meshPending = Threading::g_clientWorkerPool
                            ? Threading::g_clientWorkerPool->GetPendingJobCount() : 0;
                        const size_t gpuSections = Render::g_clientMeshManager
                            ? Render::g_clientMeshManager->GetGPUDataCount() : 0;
                        size_t gpuOrphans = 0;   // active GPU sections whose chunk is not loaded on the client
                        if (Render::g_clientMeshManager && Client::g_clientChunkManager) {
                            Render::g_clientMeshManager->ForEachActiveSection([&](const auto& key, const auto*) {
                                if (!Client::g_clientChunkManager->IsChunkLoaded(key.chunkPos)) ++gpuOrphans;
                            });
                        }
                        size_t rssMb = 0;
#ifdef __APPLE__
                        {
                            mach_task_basic_info info{};
                            mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
                            if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                          reinterpret_cast<task_info_t>(&info), &n) == KERN_SUCCESS) {
                                rssMb = static_cast<size_t>(info.resident_size >> 20);
                            }
                        }
#endif
                        Log::Info("[HarnessChunks] t=%ds srvChunks=%zu srvPending=%zu genJobs=%zu sent=%zu "
                                  "cliChunks=%zu meshPending=%zu gpuSections=%zu rssMB=%zu libChunks=%zu retained=%zu retainedMB=%zu restored=%zu gpuOrphans=%zu",
                                  sec, srvChunks, srvPending, genJobs, sent,
                                  cliChunks, meshPending, gpuSections, rssMb, libChunks,
                                  Client::g_clientChunkManager ? Client::g_clientChunkManager->GetRetainedChunkCount() : size_t(0),
                                  Client::g_clientChunkManager ? (Client::g_clientChunkManager->GetRetainedBytes() >> 20) : size_t(0),
                                  Client::g_clientChunkManager ? Client::g_clientChunkManager->m_retainRestored : size_t(0), gpuOrphans);
                    }
                    harnessFrames = 0;
                    harnessWorstFrame = 0.0;

                    // Culling audit (harness runs only): hunt for sections
                    // whose occlusion answer is provably wrong or stuck.
                    //   MIRROR  — isAllAir mirror disagrees with the blocks
                    //             (would render an opaque invisible section).
                    //   stale   — GPU mesh/mask behind the content version
                    //             (normal DURING a cascade; must drain to ~0
                    //             once it settles).
                    //   STUCK   — stale but with no remesh possible: not
                    //             dirty and meshingVersion == version, so the
                    //             scheduler will never touch it again.
                    if (sec > 0 && sec % 15 == 0 && Client::g_clientChunkManager) {
                        static std::vector<std::pair<Game::Math::ChunkPos, Client::ClientChunk*>> auditChunks;
                        Client::g_clientChunkManager->SnapshotLoadedChunks(auditChunks);
                        int mirrorBad = 0, staleCount = 0, stuckNoJob = 0, printed = 0;
                        for (auto& [cpos, achunk] : auditChunks) {
                            if (!achunk->chunkData) continue;
                            for (int sy = 0; sy < 24; ++sy) {
                                const auto& si = achunk->sectionInfos[sy];
                                if (!si.hasCpuData) continue;
                                const auto* csec = achunk->chunkData->GetSection(sy);
                                const bool nowAir = (csec == nullptr) || csec->IsAllAir();
                                if (nowAir != si.isAllAir) {
                                    ++mirrorBad;
                                    if (printed < 8) { ++printed;
                                        Log::Info("[CullAudit] MIRROR chunk(%d,%d) sy=%d mirror=%d actual=%d dirty=%d ver=%u up=%u mesh=%u",
                                                  cpos.x, cpos.z, sy, (int)si.isAllAir, (int)nowAir,
                                                  (int)si.dirty, si.version, si.uploadedVersion, si.meshingVersion);
                                    }
                                }
                                // Air sections are never compiled, so their
                                // uploadedVersion stays 0 forever — count only
                                // sections a mesh is actually expected for, or
                                // the real signal drowns (measured: 24443
                                // air-section "stales" on bench-flat, constant
                                // from t=15 to t=90).
                                if (!si.isAllAir && si.version != si.uploadedVersion) {
                                    ++staleCount;
                                    if (!si.dirty && si.meshingVersion == si.version) {
                                        ++stuckNoJob;
                                        if (printed < 8) { ++printed;
                                            Log::Info("[CullAudit] STUCK chunk(%d,%d) sy=%d ver=%u up=%u mesh=%u air=%d built=%d",
                                                      cpos.x, cpos.z, sy, si.version, si.uploadedVersion,
                                                      si.meshingVersion, (int)si.isAllAir, (int)si.builtOnce);
                                        }
                                    }
                                }
                            }
                        }
                        Log::Info("[CullAudit] t=%ds mirrorBad=%d stale=%d stuckNoJob=%d chunks=%zu",
                                  sec, mirrorBad, staleCount, stuckNoJob, auditChunks.size());
                    }
                }
                if (!harnessLateSent && !devExecLateCommands.empty() && devQuitAfterSec > 0.0 &&
                    elapsed >= devQuitAfterSec - 5.0 && networkClient && networkClient->IsConnected()) {
                    if (auto conn = networkClient->GetConnection()) {
                        for (const std::string& cmd : devExecLateCommands) {
                            Log::Info("[Harness] t=%.2fs exec-late: %s", elapsed, cmd.c_str());
                            conn->SendChatMessage(cmd);
                        }
                        harnessLateSent = true;
                    }
                }
                if (devQuitAfterSec > 0.0 && elapsed >= devQuitAfterSec) {
                    Log::Info("[Harness] --quit-after reached; closing");
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
                if (poseReplayQuitWhenDone && poseReplayer.Finished()) {
                    poseReplayQuitWhenDone = false;
                    Log::Info("[Harness] t=%.2fs --replay finished; closing", elapsed);
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }

            // Server dropped us (kick, host quit, connection lost). Same exit
            // as "Save and Quit to Title": the teardown below runs and the
            // outer session loop shows the title screen again.
            if (serverDropped->load(std::memory_order_acquire)) {
                Log::Info("Connection lost — ending session and returning to title");
                // Queued rather than pushed: the title screen this belongs on
                // top of does not exist until RunTitleScreenPhase rebuilds the
                // stack, several hundred lines of teardown from here.
                Render::SetPendingDisconnectReason(*dropReason);
                returnToTitle = true;
                break;
            }

            // === PER-FRAME: Poll events and handle input (must be every frame for responsiveness) ===
            bool cursorEnabled;
            // Covers polling AND every UI branch (chat / inventory / pause) —
            // not just input. The children below say which part actually ran.
            { PROFILE_ZONE_N("InputUI");
            PROFILE_TIMER_START(input);
            { PROFILE_ZONE_N("PollEvents");
            glfwPollEvents();
            }
#ifdef __APPLE__
            MacTrackNativeFullscreen(window);
#endif
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
#ifdef __APPLE__
            {
                // macOS's own full-screen shortcut, Control-Command-F. On a
                // Mac keyboard F11 without Fn is Show Desktop and never
                // reaches the game, which is how a fullscreen test could run
                // without the window ever leaving windowed mode.
                static bool s_chordHeld = false;
                const bool cmd  = glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
                                  glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
                const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                                  glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
                const bool chord = cmd && ctrl && glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
                if (chord && !s_chordHeld) ToggleFullscreen(window);
                s_chordHeld = chord;
            }
#endif

            // ESC edge-detection is SHARED across every branch below. The
            // chat and inventory branches consume ESC to close themselves via
            // their own trackers; updating the shared held-state up here
            // guarantees that same physical press can't re-edge in the game
            // branch one frame later and pop the pause menu open.
            extern bool s_escKeyHeld;
            const bool escIsDown = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            // From the key callback, not the poll: a tap shorter than one
            // frame (easy at portal-view frame times) was missed outright
            // and had to be pressed again.
            const bool escPressedThisFrame = Input::ConsumeEscapePress();
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
                if (applied & (Render::ScreenManager::APPLY_RENDER_DISTANCE |
                               Render::ScreenManager::APPLY_SIMULATION_DISTANCE)) {
                    // Same path as the debug-UI slider: re-send client
                    // settings so the server retunes the watch set and the
                    // player's simulation ticket (MC Options.broadcastOptions).
                    const int newDist = Platform::g_gameSettings.GetRenderDistance();
                    const int newSimDist = Platform::g_gameSettings.GetSimulationDistance();
                    Log::Info("Render distance %d, simulation distance %d (options)",
                              newDist, newSimDist);
                    if (networkClient) {
                        if (auto conn = networkClient->GetConnection()) {
                            conn->SendClientSettings(
                                newDist,
                                newSimDist,
                                Platform::g_gameSettings.GetVSync(),
                                Platform::g_gameSettings.GetMouseSensitivity());
                        }
                    }
                }
                if (applied & Render::ScreenManager::APPLY_MESH_OPTIONS) {
                    // MC LevelRenderer.allChanged — every section is rebuilt
                    // under the new options (leaves, smooth lighting, biome
                    // blend all change what a mesh contains).
                    if (Render::Mesher::SyncMeshOptionsFromSettings()
                        && Render::g_clientMeshManager && Client::g_clientChunkManager) {
                        std::vector<Render::ClientMeshManager::SectionKey> keys;
                        Render::g_clientMeshManager->ForEachActiveSection(
                            [&keys](const Render::ClientMeshManager::SectionKey& key,
                                    const Render::GPUSectionData*) { keys.push_back(key); });
                        for (const auto& key : keys) {
                            Client::g_clientChunkManager->MarkSectionDirty(key.chunkPos, key.sectionY);
                        }
                        Log::Info("Mesh options changed: %zu sections queued for remesh", keys.size());
                    }
                }
                if (applied & Render::ScreenManager::APPLY_RESOURCE_PACKS) {
                    ReloadResources();
                }
                if (applied & Render::ScreenManager::APPLY_MIPMAPS) {
                    if (Render::g_atlasBuilder)
                        Render::g_atlasBuilder->SetMipmapLevels(Platform::g_gameSettings.GetMipmapLevels());
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
                // Paste and copy: Cmd on macOS, Ctrl elsewhere (both accepted
                // everywhere). GLFW sends no character event for a chorded
                // key, so the letter never lands in the box on its own.
                {
                    static bool pasteHeld = false, copyHeld = false;
                    const bool chord =
                        glfwGetKey(window, GLFW_KEY_LEFT_SUPER)   == GLFW_PRESS ||
                        glfwGetKey(window, GLFW_KEY_RIGHT_SUPER)  == GLFW_PRESS ||
                        glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                        glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
                    const bool pasteDown = chord && glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
                    const bool copyDown  = chord && glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS;
                    if (pasteDown && !pasteHeld) {
                        if (const char* clip = glfwGetClipboardString(window)) g_chatScreen.InsertText(clip);
                    }
                    if (copyDown && !copyHeld && !g_chatScreen.InputText().empty()) {
                        glfwSetClipboardString(window, g_chatScreen.InputText().c_str());
                    }
                    pasteHeld = pasteDown;
                    copyHeld  = copyDown;
                }
                // Keys typed here arrive through the char queue and the
                // polled keys below; the UI key queue would only hoard them
                // for the next screen. Drop them.
                Input::ClearUiKeyPresses();
                // Route key input (Enter, Escape, Backspace, arrows)
                if (escPressedThisFrame) {
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
                    } else if (cmd == "/record" || cmd == "/replay") {
                        // Pose recording / replay, client-side (see
                        // SessionReplay.hpp and the --record/--replay flags).
                        //   /record <name>   start (t=0 is now); /record stop saves
                        //   /replay <name> [holdSeconds]   /replay stop
                        std::string arg1, arg2;
                        {
                            std::istringstream rest(sp == std::string::npos
                                ? std::string() : submitted.substr(sp));
                            rest >> arg1 >> arg2;
                        }
                        auto say = [&](const std::string& text, uint32_t color = 0xFFAAAAAA) {
                            g_chatComponent.AddMessage(text, color);
                        };
                        if (cmd == "/record") {
                            if (arg1.empty()) {
                                say(poseRecorder.Active()
                                    ? "Recording \"" + poseRecorder.Name() + "\" - /record stop to save"
                                    : "Usage: /record <name> | /record stop");
                            } else if (arg1 == "stop") {
                                if (poseRecorder.Active()) {
                                    say("Saved recording \"" + poseRecorder.Name() + "\" (" +
                                        std::to_string(poseRecorder.SampleCount()) + " samples)");
                                    poseRecorder.Stop();
                                } else {
                                    say("Not recording");
                                }
                            } else if (poseRecorder.Start(arg1, poseWorldName)) {
                                say("Recording \"" + arg1 + "\" - /record stop to save", 0xFF55FF55);
                            }
                        } else {
                            if (arg1.empty()) {
                                say(poseReplayer.Active()
                                    ? "Replaying \"" + poseReplayer.Name() + "\" - /replay stop to end"
                                    : "Usage: /replay <name> [holdSeconds] | /replay stop");
                            } else if (arg1 == "stop") {
                                if (poseReplayer.Active()) { poseReplayer.Stop(); say("Replay stopped"); }
                                else say("Not replaying");
                            } else {
                                const double hold = arg2.empty() ? devReplayHoldSec : std::atof(arg2.c_str());
                                if (poseReplayer.Load(arg1, hold)) {
                                    poseReplayQuitWhenDone = false;
                                    say("Replaying \"" + arg1 + "\" (" +
                                        std::to_string(static_cast<int>(poseReplayer.Duration())) +
                                        " s, holding " + std::to_string(static_cast<int>(hold)) + " s first)",
                                        0xFF55FF55);
                                } else {
                                    say("Cannot load recording \"" + arg1 + "\" - see the log", 0xFFFF5555);
                                }
                            }
                        }
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
                static bool qHeld=false;
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
                // ESC uses the SHARED edge (s_escKeyHeld, sampled once per
                // frame above) for exactly the reason the note about `eHeld`
                // gives for E — it just never got the same treatment.
                //
                // With a private `escHeld` here, this branch and the
                // screen-manager branch each kept their own idea of whether ESC
                // was down. They are mutually exclusive, so the branch that did
                // not run held a stale `false`, and the next press it DID see
                // edged a second time. That is the "one press closes the pause
                // menu straight back up" report, and it showed up on the first
                // press because that is when the two flags are furthest apart.
                if (escPressedThisFrame) inv.OnKeyDown(GLFW_KEY_ESCAPE, mods);
                // Same as chat: the inventory takes its keys by its own route,
                // so the UI key queue must not hoard them for the next screen.
                Input::ClearUiKeyPresses();
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

                // Drain char queue when chat is closed (prevent buildup).
                // A typed '/' is remembered: on a keyboard whose slash is
                // not where the US layout has it, the command BINDING never
                // fires, but the character still arrives.
                bool slashTyped = false;
                while (Input::HasCharInput()) {
                    if (Input::PopCharInput() == static_cast<unsigned int>('/')) slashTyped = true;
                }

                // Open chat on T or /
                if (Input::ConsumeClick(*Input::Binds::Chat)) {
                    g_chatScreen.Open(false);
                } else if (Input::ConsumeClick(*Input::Binds::Command) || slashTyped) {
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

            // MC Minecraft.java:1284 — the pause flag. Vanilla reads it
            // straight off the embedded client and refuses to pause at all
            // once the world is published; we report it instead, so the server
            // can freeze the world only when EVERY player is paused.
            //
            // ScreenManager only, deliberately: chat and the container screens
            // are separate systems here and MC's isPauseScreen() is false for
            // both of them anyway — a furnace keeps smelting while you watch.
            //
            // Sent on CHANGE only. The server treats a client that never sends
            // it as playing, so a dropped packet can never freeze the world for
            // somebody else; the next transition re-syncs it.
            {
                const bool nowPaused = Render::GetScreenManager().IsPauseScreenOpen();
                localPaused = nowPaused;
                if (nowPaused != sentPaused) {
                    // `sentPaused` advances ONLY when the packet actually went
                    // out. It used to advance unconditionally, which made it a
                    // record of intent rather than of what the server was told:
                    // a transition that happened while disconnected (or before
                    // the connection existed) was marked as sent and, because
                    // this reports on CHANGE only, never sent again.
                    //
                    // The dangerous direction is a lost UN-pause. Every entity
                    // tick on the server sits behind SimulationRuns(), which is
                    // `runsNormally() && !m_paused` — so a server left believing
                    // a player is paused stops simulating entirely while still
                    // streaming chunks and answering commands. `/tick unfreeze`
                    // cannot clear it, because that only touches the other half
                    // of the condition.
                    if (networkClient && networkClient->IsConnected()) {
                        if (auto conn = networkClient->GetConnection()) {
                            Network::PlayerPauseC2SPacket pausePacket{};
                            pausePacket.paused = nowPaused;
                            auto data = Network::Serialization::Serialize(pausePacket);
                            conn->SendPacket(
                                static_cast<uint8_t>(Network::PacketId::PlayerPauseC2S), data);
                            sentPaused = nowPaused;
                        }
                    }
                }
            }
            {
                static bool s_prevUiActive = false;
                if (screenOpen && !s_prevUiActive) {
                    Input::ReleaseAll();
                } else if (!screenOpen && s_prevUiActive) {
                    Input::RestoreKeyboardState();
                }
                s_prevUiActive = screenOpen;
            }
            // ── Debug free camera: F+C chord (raw keys, edge-triggered) ──
            // Raw IsKeyDown rather than the bind layer on purpose: F is the
            // swap-offhand bind, and a chord has no press event of its own —
            // it fires on the frame BOTH keys are held after not both being
            // held. The F-press half of the chord is swallowed in
            // HandlePlayerInput (SwapOffhand is dropped while C is physically
            // down), so forming the chord C-first never swaps hands; F-first
            // still fires the swap on F's own press frame — that press
            // happened before any chord existed, and the event was consumed
            // then, so it cannot be recalled (noted limitation).
            //
            // Gated on !screenOpen so typing "f"/"c" into chat can't toggle
            // it, and any screen opening while detached re-attaches — same
            // rule as leaving the world (session-local state above).
            {
                static bool s_prevFreeCamChord = false;
                const bool chordHeld = !screenOpen &&
                                       Input::IsKeyDown(Input::Key::F) &&
                                       Input::IsKeyDown(Input::Key::C);
                const bool chordEdge = chordHeld && !s_prevFreeCamChord;
                s_prevFreeCamChord = chordHeld;

                if (chordEdge) {
                    freeCamActive = !freeCamActive;
                    if (freeCamActive) {
                        // Detach at the current eye pose. Velocity is frozen
                        // to zero so a flying player hovers where they were
                        // (a standing one just keeps standing under normal
                        // gravity — physics keeps ticking with zero input).
                        freeCam.position = player.GetEyePosition();
                        freeCam.yaw      = camera.yaw;
                        freeCam.pitch    = camera.pitch;
                        player.physics.velocity = glm::vec3(0.0f);
                        Log::Info("[FreeCam] Detached - culling stays at the player; F+C to return");
                    } else {
                        Log::Info("[FreeCam] Re-attached to player view");
                    }
                }
                if (freeCamActive && screenOpen) {
                    freeCamActive = false;
                    Log::Info("[FreeCam] Re-attached (screen opened)");
                }
                s_freeCamHudBanner = freeCamActive;
            }

            // MC Minecraft.tick:1778-1780 — refreshed every frame a screen
            // is open; ContinueAttack(false) clears it once the button is up.
            // The free camera pins it too: no attack may start while detached.
            if (cursorVisible || freeCamActive) playerController.SetMissTime(10000);

            // Drains attack/use and stops an in-progress break/place whenever
            // the cursor is up, which is what tears down held-RMB when a screen
            // takes over.
            { PROFILE_ZONE_N("PlayerInput");
            HandlePlayerInput(player, playerController, camera, cursorVisible,
                              freeCamActive);
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

            // Drain ALL queued packets ONCE PER FRAME, before the tick loop —
            // MC Minecraft.runTick:1223-1230, where processQueuedPackets() sits
            // inside `if (advanceGameTime)` and ahead of the
            // `for (i < min(10, ticksToDo)) tick()` loop.
            //
            // This used to live INSIDE the catch-up loop below, which coupled
            // drain cadence to tick debt and produced exactly the wrong
            // behaviour under load: during any single long frame the queue was
            // not drained at all, and then a frame that owed several ticks
            // drained several times in a row. Bursty arrival against a bursty
            // drain is what let the incoming queue pile up during world load,
            // back when piling up meant dropped packets. Draining per frame
            // decouples the two — a slow frame still drains once, and a fast
            // frame drains more often than the tick rate rather than less.
            { PROFILE_ZONE_N("Network");
            PROFILE_TIMER_START(network);
            if (networkClient) {
                networkClient->DrainIncomingPackets();
                // The active level may have changed during the drain
                // (ChangeDimensionS2C); everything that caches a level
                // pointer refreshes here, and empty far-side levels are
                // freed.
                playerController.SetBlockAccess(Client::g_clientBlockAccess);
                Client::ClientLevels::GarbageCollect();
            }
#if ENABLE_IMMERSIVE_PORTALS
            // The portals the player's physics collides across this frame,
            // from the active level (bound again after the drain).
            Client::g_immersivePortalCollision.Update(glm::dvec3(player.GetEyePosition()),
                                                     player.physics.GetAABB());
#endif
            PROFILE_TIMER_END(network, metrics.networkProcessingTime);
            }

            while (now >= nextClientTick && ticksThisFrame < MAX_TICKS_PER_FRAME) {
                PROFILE_ZONE_N("ClientTick");

                // MC ClientPacketListener.tick: levelLoadTracker.tickClientLoad()
                // then notifyPlayerLoaded() once the level is ready. Still per
                // TICK (MC drives it from the packet listener's tick, not from
                // runTick), and still after the drain — which now happened just
                // above, for the whole frame.
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
                // A pause screen freezes them for the same reason a /tick freeze
                // does — the server has stopped simulating, so an animating mob
                // is showing motion that is not happening.
                const bool entitiesFrozen =
                    Client::g_clientTickRate.IsEntityFrozen() || localPaused;

                if (Client::g_itemEntityManager && !entitiesFrozen) {
                    // Feet position, for pickup animations to fly toward.
                    Client::g_itemEntityManager->Tick(player.predictedPos);
                }
                if (Client::g_xpOrbManager && !entitiesFrozen) {
                    // Feet position again — the orbs' player pull and their
                    // pickup flights both aim at the local player.
                    Client::g_xpOrbManager->Tick(player.predictedPos);
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
                    if (entitiesFrozen) {
                        // No tick, but aiming still works in vanilla's frozen
                        // world: keep the picker's candidate list current so
                        // P / attacks / interactions reach mobs, including
                        // ones spawned during the freeze.
                        Client::g_clientMobManager->SetPickOrigin(
                            glm::dvec3(player.physics.position));
                        Client::g_clientMobManager->RefreshPickCandidates();
                    }
                    if (!entitiesFrozen) {
                        Client::g_clientMobManager->SetPickOrigin(
                            glm::dvec3(player.physics.position));
                        Client::g_clientMobManager->Tick();
                        if (Client::g_clientFallingBlocks) {
                            Client::g_clientFallingBlocks->SetBlockAccess(Client::g_clientBlockAccess);
                            Client::g_clientFallingBlocks->Tick();
                        }
                    }

                    // Far-side levels (seen through portals) tick their
                    // entities too, bound in turn, so what a portal shows
                    // moves between server updates instead of stuttering.
                    if (!entitiesFrozen) {
                        Client::ClientLevels::ForEach([&](Client::ClientLevel& level) {
                            if (&level == &Client::ClientLevels::Active()) return;
                            // The local player's position AS SEEN FROM this level:
                            // their image through the nearest portal leading here.
                            // It is what a pickup flies toward and what an orb
                            // homes on — the player's own coordinates belong to
                            // another world.
                            glm::dvec3 imagePos = player.predictedPos;
#if ENABLE_IMMERSIVE_PORTALS
                            {
                                const Game::Immersive::Portal* via = nullptr;
                                double best = 16.0;
                                Client::ClientLevels::Active().Portals().ForEach([&](const Game::Immersive::Portal& p) {
                                    if (p.IsMirror() || p.destDimension != level.Dimension()) return;
                                    glm::dvec3 mn, mx;
                                    p.BoundingBox(mn, mx, 0.0);
                                    const double d = glm::length(glm::clamp(player.predictedPos, mn, mx) - player.predictedPos);
                                    if (d < best) { best = d; via = &p; }
                                });
                                if (via) imagePos = via->TransformPoint(player.predictedPos);
                            }
#endif
                            Client::ClientLevels::WithLevel(level.Dimension(), [&]() {
                                if (Client::g_itemEntityManager) Client::g_itemEntityManager->Tick(imagePos);
                                if (Client::g_xpOrbManager)      Client::g_xpOrbManager->Tick(imagePos);
                                if (Client::g_clientMobManager) {
                                    Client::g_clientMobManager->SetBlockAccess(Client::g_clientBlockAccess);
                                    Client::g_clientMobManager->SetTime(
                                        Render::EnvironmentState::Get().GameTime(),
                                        Render::EnvironmentState::Get().DayTime());
                                    // Pick candidates are gathered around this
                                    // origin: the player's image in this level,
                                    // not their coordinates in another one.
                                    Client::g_clientMobManager->SetPickOrigin(imagePos);
                                    Client::g_clientMobManager->Tick();
                                }
                                if (Client::g_clientFallingBlocks) {
                                    Client::g_clientFallingBlocks->SetBlockAccess(Client::g_clientBlockAccess);
                                    Client::g_clientFallingBlocks->Tick();
                                }
                            });
                        });
                    }

                    // MC ClientLevel.animateTick — the ambient-particle sweep
                    // (Minecraft.java:1854 calls it once per client tick with
                    // the player's block position). This is the ONLY producer
                    // of block particles: without it the dust under
                    // unsupported sand never appears, and neither will torch
                    // flames or lava pops when those land.
                    //
                    // Gated on entitiesFrozen for the same reason the mob tick
                    // is: a frozen world should look frozen.
                    if (!entitiesFrozen && Client::g_clientBlockAccess) {
                        static Game::JavaRandom s_animateRandom{0};
                        const glm::dvec3& camPos = player.visualPos;
                        Client::AnimateTick(
                            glm::ivec3(static_cast<int>(std::floor(camPos.x)),
                                       static_cast<int>(std::floor(camPos.y)),
                                       static_cast<int>(std::floor(camPos.z))),
                            *Client::g_clientBlockAccess,
                            Client::g_clientMobManager->Level(),
                            s_animateRandom);
                    }
                }

                // 3b. Advance local world time (ClientLevel.tickTime mirror —
                //     smooth day/night between the server's 20-tick syncs).
                //     Gated exactly as MC gates it (ClientLevel.tick:269,
                //     `if (tickRateManager().runsNormally()) { … tickTime(); }`):
                //     the server stops advancing dayTime while frozen, so
                //     without this the sky would keep sliding toward a sunset
                //     the world never reaches, then snap back on the next
                //     TimeUpdate.
                // `!localPaused` is MC's own `if (this.level != null &&
                // !this.pause)` (Minecraft.java:1741) on top of the /tick
                // freeze gate. Without it the sky keeps sliding while the
                // server is paused and then snaps back on the next TimeUpdate.
                if (Client::g_clientTickRate.RunsNormally() && !localPaused) {
                    Render::EnvironmentState::Get().TickClient();
                }

                // Screen 20Hz tick (death-screen button delay, caret blink).
                // The title phase has its own pump; in-game screens only got
                // Update/Render before this.
                Render::GetScreenManager().Tick();

                // 4. Send player position to server (one packet per tick = 20 Hz,
                //    unconditionally — the on-demand flushes before interaction
                //    packets ride the same lambda; see its definition).
                sendPlayerMove(true);

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

            // Drop the excess instead of carrying it — MC DeltaTracker.Timer
            // .advanceGameTime:38-45 keeps only a SUB-TICK residual:
            //
            //     deltaTicks = (currentMs - lastMs) / msPerTick;
            //     lastMs = currentMs;                 // <- reset every frame
            //     deltaTickResidual += deltaTicks;
            //     int ticks = (int)deltaTickResidual;
            //     deltaTickResidual -= ticks;         // <- keeps only 0..1
            //
            // and Minecraft.runTick:1239 then runs `min(10, ticksToDo)`. A
            // frame that owed twenty ticks runs ten and the other ten are GONE;
            // the next frame measures from now, not from the old deadline.
            //
            // We accumulated an absolute deadline instead, and it was only ever
            // advanced one interval per tick actually executed. So a single
            // frame long enough to owe more than MAX_TICKS_PER_FRAME left
            // permanent debt: nextClientTick could never catch up, and the loop
            // ran its full cap of ten ticks EVERY frame for the rest of the
            // session. One hitch during world load and the client stayed in
            // catch-up forever, doing 10x the tick work per frame.
            if (ticksThisFrame >= MAX_TICKS_PER_FRAME && now > nextClientTick) {
                nextClientTick = now + CLIENT_TICK_INTERVAL;
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
            // A portal crossing (gun prediction or immersive traveler) moved
            // the player this frame. The pose recorder files the frame's
            // sample as a crossing, and the replayer's crossing handshake
            // reads it (see SessionReplay.hpp).
            bool poseCrossedThisFrame = false;
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
            //
            // MC LocalPlayer.tick:212 wraps the ENTIRE player tick — physics
            // and the position send both — in
            // `if (this.connection.hasClientLoaded())`. Until the client's own
            // section has been compiled there is nothing to stand on: the
            // block view answers "air" for every chunk that has not arrived
            // yet, so the player free-falls through the world for as long as
            // the terrain takes to stream in.
            //
            // That is not cosmetic, because the falling position is then SENT
            // and the server adopts it. Every rejoin saved a Y a few blocks
            // below the last one and the error compounded — measured at 3.9
            // and 4.7 blocks per session with x/z identical.
            //
            // g_levelLoadTracker is this engine's port of the same
            // LevelLoadTracker MC drives hasClientLoaded() from, and it has a
            // 30-second escape hatch, so this cannot strand the player.
            if (player.health <= 0 || !Client::g_levelLoadTracker.IsLoaded()) {
                player.physics.velocity = glm::vec3(0.0f);
            } else if (poseReplayer.Active()) {
                // Replay owns the pose: the recorded path is written in
                // place of the physics step. Everything downstream — the
                // portal crossing checks, the move packet, culling, mesh
                // scheduling — sees an ordinary moving player.
                poseReplayer.Apply(player, camera, dt,
                    static_cast<int8_t>(Game::DimensionToRaw(Client::ClientLevels::ActiveDimension())));
            } else {
                player.UpdatePhysics(dt, Client::g_clientBlockAccess);
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
                    poseCrossedThisFrame    = true;
                    poseReplayer.OnCrossing();
                    Log::Info("[PortalPredict] Client predicted teleport "
                              "to (%.2f,%.2f,%.2f) yaw %.1f pitch %.1f",
                              pred.newFeet.x, pred.newFeet.y, pred.newFeet.z,
                              pred.newYawDeg, pred.newPitchDeg);
                }
            }
#endif

#if ENABLE_IMMERSIVE_PORTALS
            // Immersive portals: the crossing is decided HERE, per frame, on
            // the eye's path between two frames (ImmersivePortalTraveler),
            // so no frame is rendered with the camera past a surface it has
            // not yet gone through. Skipped on a frame the gun's prediction
            // already moved the player.
            {
                const bool canCross = player.health > 0 && Client::g_levelLoadTracker.IsLoaded()
#if ENABLE_PORTAL_GUN
                                      && !justTeleportedThisFrame
#endif
                                      ;
                const glm::dvec3 eyeNow(player.GetEyePosition());
                if (canCross) {
                    // The velocity that goes through: the larger of the
                    // pre- and post-physics ones. A collision snap on the
                    // way in (a fast fall clipping the block under a floor
                    // portal) zeroes the post-physics velocity, and mapping
                    // that would have the player leave the far side at rest.
#if ENABLE_PORTAL_GUN
                    const glm::vec3 crossVel =
                        glm::length(prevVel) > glm::length(player.physics.velocity)
                            ? prevVel : player.physics.velocity;
#else
                    const glm::vec3 crossVel = player.physics.velocity;
#endif
                    if (auto crossing = Client::g_immersivePortalTraveler.Check(
                            immersiveLastEye, eyeNow, glm::dvec3(player.physics.position),
                            crossVel, camera.yaw, camera.pitch, player.physics.scale)) {
                        // Read off the record before Commit rebinds the level.
                        const bool crossedGlobal = [&] {
                            const Game::Immersive::Portal* p =
                                Client::GetClientImmersivePortals().Get(crossing->portalId);
                            return p && p->Has(Game::Immersive::PortalFlag::Global);
                        }();
                        player.physics.position = glm::vec3(crossing->newFeet);
                        player.physics.velocity = crossing->newVelocity;
                        player.physics.scale    = crossing->newScale;
                        camera.yaw              = crossing->newYaw;
                        camera.pitch            = crossing->newPitch;
                        Client::g_immersivePortalTraveler.Commit(*crossing);
                        poseCrossedThisFrame = true;
                        poseReplayer.OnCrossing();
                        // A crossing within one level (a wrap border) moves
                        // the camera a world's width in one frame; the chunk
                        // renderer's cached reachable sets are for where it
                        // was. See ChunkRenderer::OnCameraTeleport.
                        if (crossing->dimensionAfter == crossing->dimensionBefore &&
                            Render::g_chunkRenderer) {
                            Render::g_chunkRenderer->OnCameraTeleport();
                        }
                        // The arrival box against the far side's blocks.
                        // A player brushing a wall on the way in lands
                        // touching the far wall — or, through a frame not
                        // quite on the block grid, a hair inside it — and
                        // the physics refuses every horizontal move from
                        // an overlapping box. The smallest SIDEWAYS nudge
                        // that frees the box is taken before the position
                        // is committed anywhere; the far portal's own
                        // pass-through cells count as open, so the hooks
                        // are refreshed for the new spot first.
                        //
                        // Never vertical for an ordinary portal. A lift here
                        // is a visible step on arrival, and it was only ever
                        // covering for a box that had no business
                        // overlapping: the flipped twin's collision engaging
                        // while the body straddled the surface (see
                        // ImmersivePortalCollision::Update). A body whose
                        // feet do not fit the far opening cannot enter the
                        // near one either (the same fit test gates both), so
                        // there is no stair case left for a lift to solve.
                        {
                            Client::g_immersivePortalCollision.Update(glm::dvec3(player.GetEyePosition()),
                                                                     player.physics.GetAABB());
                            Game::PhysicsContext ctx;
                            ctx.blockAccess = Client::g_clientBlockAccess;
                            if (ctx.blockAccess &&
                                Game::CheckCollision(player.physics.position, player.physics, ctx)) {
                                const float bodyScale = std::max(player.physics.scale, 0.05f);
                                const float step      = 0.05f * bodyScale;
                                // A wall brush is a hair, not a block.
                                const float reachSide = 0.3f * bodyScale;
                                // Never back toward the surface just left:
                                // an eye nudged behind it would cross straight
                                // back next frame.
                                const glm::vec3 onward(crossing->arrivalDirection);
                                bool freed = false;
                                glm::vec3 best(0.0f);
                                auto search = [&](bool allowBack, bool allowVertical, float reach) {
                                    for (float r = step; r <= reach + 1e-4f && !freed; r += step) {
                                        // The four sides, then the diagonals;
                                        // the first free one wins. Vertical
                                        // tries only for the global net below.
                                        const glm::vec3 tries[] = {
                                            { r, 0.0f,  0.0f}, {-r, 0.0f,  0.0f},
                                            { 0.0f, 0.0f,  r }, { 0.0f, 0.0f, -r },
                                            { r, 0.0f,  r }, { r, 0.0f, -r }, {-r, 0.0f,  r }, {-r, 0.0f, -r },
                                            { 0.0f,  r,  0.0f},
                                            { r,  r,  0.0f}, {-r,  r,  0.0f}, { 0.0f,  r,  r }, { 0.0f,  r, -r },
                                        };
                                        for (const glm::vec3& t : tries) {
                                            if (!allowVertical && t.y != 0.0f) continue;
                                            if (!allowBack && glm::dot(t, onward) < -1e-4f) continue;
                                            if ((t.x != 0.0f || t.z != 0.0f) && r > reachSide) continue;
                                            if (!Game::CheckCollision(player.physics.position + t, player.physics, ctx)) {
                                                best = t; freed = true; break;
                                            }
                                        }
                                    }
                                };
                                search(/*allowBack=*/false, /*allowVertical=*/false, reachSide);
                                // A global seam (a stack floor, a wrap border)
                                // is one-faced and its reverse faces the far
                                // side, so a nudge back past the plane cannot
                                // re-cross it — and under a floor seam "back"
                                // is UP, out of the roof the far world may
                                // have where the crossing landed (the far
                                // floor normally holds the player above it;
                                // this is the net under that). Last resort
                                // only, and the only case a vertical nudge
                                // is allowed at all.
                                if (!freed && crossedGlobal) {
                                    search(/*allowBack=*/true, /*allowVertical=*/true,
                                           player.physics.GetEyeHeight() + 0.1f * bodyScale);
                                }
                                if (freed) {
                                    player.physics.position += best;
                                    Log::Info("[ImmersivePortals] Arrival nudged out of a block by (%.2f, %.2f, %.2f)",
                                              best.x, best.y, best.z);
                                } else {
                                    Log::Warning("[ImmersivePortals] Arrived inside a block and no sideways nudge within %.2f freed it",
                                                 reachSide);
                                }
                            }
                        }
                        player.predictedPos     = glm::dvec3(player.physics.position);
                        player.serverPos        = glm::dvec3(player.physics.position);
                        player.visualPos        = glm::dvec3(player.physics.position);
                        // The segment start follows any nudge, so the next
                        // frame's eye path starts where the eye now is.
                        immersiveLastEye = crossing->nextLastEye +
                                           (glm::dvec3(player.physics.position) - crossing->newFeet);
                    } else if (!Client::g_immersivePortalTraveler.OnCooldown()) {
                        // On cooldown the segment's start is kept: an eye
                        // that goes back through the surface during those
                        // frames is caught by the first check after, not
                        // left standing behind a surface it never crossed.
                        immersiveLastEye = eyeNow;
                    }
                } else {
                    immersiveLastEye = eyeNow;
                }
            }
#endif

            // Pose recording: the frame's final pose, after physics and any
            // crossing. Started here (not at session start) so t=0 is the
            // first frame with a world to stand in.
            if (!poseRecordPending.empty() && Client::g_levelLoadTracker.IsLoaded()) {
                poseRecorder.Start(poseRecordPending, poseWorldName);
                poseRecordPending.clear();
            }
            if (poseRecorder.Active()) {
                poseRecorder.Sample(player, camera,
                    static_cast<int8_t>(Game::DimensionToRaw(Client::ClientLevels::ActiveDimension())),
                    poseCrossedThisFrame);
            }

            camera.position = player.GetEyePosition();
            if (freeCamActive) {
                // Detached: the mouse belongs to the fly camera. The player
                // camera still runs its Update (unchanged code path) but with
                // mouse-look masked off for exactly this call, so the frozen
                // yaw/pitch — which PlayerMoveC2S keeps sending — never move.
                // Save/restore rather than latching false: HandleCursorToggle
                // owns enableMouseLook and only writes it on cursor
                // transitions, so a latched value would survive re-attach.
                const bool savedMouseLook = camera.enableMouseLook;
                camera.enableMouseLook = false;
                camera.Update(dt);
                camera.enableMouseLook = savedMouseLook;

                // Fly the free camera with the normal binds. Mouse-look obeys
                // the same cursor gate as the player camera (Tab cursor up =
                // no look), movement is Camera::Update's non-physics path —
                // WASD + jump/sneak, no collision, no gravity. Speed matches
                // NOCLIP, not creative flight: the player's live
                // noclipHorizontalSpeed (default 10 blocks/s, tunable from the
                // Player panel's noclip slider — which therefore tunes this
                // camera too) and the flat 50 blocks/s noclip sprint boost,
                // same as Physics.cpp's noclip branch.
                freeCam.enableMouseLook = savedMouseLook;
                freeCam.moveSpeed = Input::IsDown(*Input::Binds::Sprint)
                    ? Game::PlayerPhysics::NOCLIP_SPRINT_HORIZONTAL_SPEED
                    : player.physics.noclipHorizontalSpeed;
                freeCam.Update(dt);
            } else if (poseReplayer.Active()) {
                // The replay wrote yaw/pitch; the mouse must not add to it.
                // Same save/restore as the free camera, for the same reason.
                const bool savedMouseLook = camera.enableMouseLook;
                camera.enableMouseLook = false;
                camera.Update(dt);
                camera.enableMouseLook = savedMouseLook;
            } else {
                camera.Update(dt);
            }
            player.UpdateRaycast(camera);
#if ENABLE_IMMERSIVE_PORTALS
            // A crosshair that reaches through a portal targets a block of
            // the FAR level: dig timing, placement prediction and the local
            // block writes all have to read and write that level's chunks.
            if (player.lastBlockHit && player.lastBlockHitPortalId != 0 &&
                player.lastBlockHitDimension != Client::ClientLevels::ActiveDimension()) {
                Client::ClientLevels::WithLevel(player.lastBlockHitDimension, [&]() {
                    playerController.SetBlockAccess(Client::g_clientBlockAccess);
                    playerController.Tick(dt);
                });
                playerController.SetBlockAccess(Client::g_clientBlockAccess);
            } else
#endif
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
            // The free camera supersedes the F5 pull-back: it overwrites the
            // whole camera pose below, so deriving a third-person zoom from
            // it first would be wasted (and wrong — the raycast would run
            // from the fly position). The saves stay unconditional: they are
            // the FROZEN PLAYER VIEW the free camera culls from and the
            // forced third-person body render orients by.
            // ── Zoom and orbit, the Roblox way ──────────────────────────
            // I zooms in a step, O zooms out a step (held, they repeat);
            // zoomed all the way in is first person, and the first step
            // out of first person is the third-person view. The left and
            // right arrows swing the third-person camera around the player
            // while held; the player keeps facing where they look, so WASD
            // still moves them the way they face.
            {
                static float s_tpZoom   = 4.0f;    // body heights; MC's default distance
                static float s_orbitYaw = 0.0f;    // degrees around the player
                constexpr float kZoomStep = 1.25f, kMinZoom = 0.75f, kMaxZoom = 24.0f;
                constexpr float kRepeatDelay = 0.35f, kRepeatEvery = 0.08f;
                constexpr float kOrbitDegPerSec = 120.0f;
                static float s_zoomHeld[2] = {0.0f, 0.0f};   // in, out
                auto zoomClicks = [&](Input::KeyMapping* bind, int slot) {
                    int clicks = 0;
                    if (!bind) return clicks;
                    while (Input::ConsumeClick(*bind)) ++clicks;
                    if (Input::IsDown(*bind)) {
                        const float before = s_zoomHeld[slot];
                        s_zoomHeld[slot] += dt;
                        if (s_zoomHeld[slot] > kRepeatDelay) {
                            const int n0 = static_cast<int>((std::max(before, kRepeatDelay) - kRepeatDelay) / kRepeatEvery);
                            const int n1 = static_cast<int>((s_zoomHeld[slot] - kRepeatDelay) / kRepeatEvery);
                            clicks += std::max(0, n1 - n0);
                        }
                    } else {
                        s_zoomHeld[slot] = 0.0f;
                    }
                    return clicks;
                };
                if (!freeCamActive) {
                    for (int i = zoomClicks(Input::Binds::ZoomIn, 0); i > 0; --i) {
                        if (camera.IsFirstPerson()) break;
                        s_tpZoom /= kZoomStep;
                        if (s_tpZoom < kMinZoom) {
                            camera.perspective = Render::Perspective::FirstPerson;
                            s_tpZoom = 1.0f;   // the first step out again is close in
                        }
                    }
                    for (int i = zoomClicks(Input::Binds::ZoomOut, 1); i > 0; --i) {
                        if (camera.IsFirstPerson()) {
                            camera.perspective = Render::Perspective::ThirdBack;
                            s_tpZoom = 1.0f;
                        } else {
                            s_tpZoom = std::min(s_tpZoom * kZoomStep, kMaxZoom);
                        }
                    }
                    if (!camera.IsFirstPerson()) {
                        if (Input::Binds::CameraLeft  && Input::IsDown(*Input::Binds::CameraLeft))  s_orbitYaw -= kOrbitDegPerSec * dt;
                        if (Input::Binds::CameraRight && Input::IsDown(*Input::Binds::CameraRight)) s_orbitYaw += kOrbitDegPerSec * dt;
                        s_orbitYaw = std::fmod(s_orbitYaw, 360.0f);
                    } else {
                        // First person forgets the swing: the next third-
                        // person view starts behind the player again,
                        // however it is entered (I/O or F5).
                        s_orbitYaw = 0.0f;
                    }
                }
                s_thirdPersonZoom = s_tpZoom;
                s_thirdPersonOrbit = s_orbitYaw;
            }

            const bool tpActive = !camera.IsFirstPerson() && !freeCamActive;
            const glm::vec3 tpSavedPos  = camera.position;
            const float     tpSavedYaw  = camera.yaw;
            const float     tpSavedPitch = camera.pitch;
            if (tpActive) {
                if (camera.perspective == Render::Perspective::ThirdFront) {
                    camera.yaw   = tpSavedYaw + 180.0f;
                    camera.pitch = -tpSavedPitch;
                }
                // The arrow keys' swing around the player.
                camera.yaw += s_thirdPersonOrbit;
                const glm::vec3 back = -camera.GetForward();
                // With the body: four blocks is four body heights at
                // vanilla size. Left at four blocks, a small player's
                // camera ray started a few centimetres off the ground and
                // hit it at once, so F5 seemed to do nothing.
                const float bodyScale = std::max(player.physics.scale, 0.05f);
                float maxZoom = s_thirdPersonZoom * bodyScale;   // I/O zoom; MC's default is 4
                for (int i = 0; i < 8; ++i) {             // MC Camera.getMaxZoom
                    const glm::vec3 off(((i & 1) * 2 - 1) * 0.1f * bodyScale,
                                        ((i >> 1 & 1) * 2 - 1) * 0.1f * bodyScale,
                                        ((i >> 2 & 1) * 2 - 1) * 0.1f * bodyScale);
                    if (auto hit = Game::Raycast::CastRay(tpSavedPos + off, back, maxZoom)) {
                        const float d = glm::length(hit->hitPoint - tpSavedPos);
                        if (d < maxZoom) maxZoom = d;
                    }
                }
                camera.position = tpSavedPos + back * maxZoom;
            }

            // === Debug free camera: swap the fly pose in for the render
            // phase, exactly like the F5 detachment above (restored at the
            // same end-of-frame site). Everything downstream that reads
            // `camera` — sky/cloud passes, fog center, entity billboards and
            // their camera-distance culls, particles, the MVP — follows the
            // fly view, which is what "the fly camera renders" means; only
            // the cull inputs are pinned to the frozen player view via the
            // frustum override and SetCullOverride below.
            if (freeCamActive) {
                camera.position = freeCam.position;
                camera.yaw      = freeCam.yaw;
                camera.pitch    = freeCam.pitch;
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
            Render::MeshCensus::DumpIfDue();   // OBEY_MESH_CENSUS=1 only

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

            // Pipeline warm-up behind the world-load screen, once per
            // process: every pipeline any earlier session built is built
            // now, while the overlay is up and nothing the player sees
            // depends on the frame time. Frame two, not one, so the overlay
            // has been drawn before the main thread blocks for the compile.
            // See RenderBackend::WarmPipelines.
            if (Render::g_renderBackend && !Client::g_levelLoadTracker.IsLoaded()) {
                static int  s_loadingFrames = 0;
                static bool s_warmed = false;
                if (++s_loadingFrames >= 2 && !s_warmed) {
                    s_warmed = true;
                    Render::g_renderBackend->WarmPipelines();
                }
            }

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
            // Dropped items cull at half the view distance (see
            // ItemEntityRenderer::SetRenderDistanceChunks). Fed the EFFECTIVE
            // distance, not the raw client setting: the server clamps what it
            // sends, and an item that never arrived cannot be drawn however
            // far the renderer is willing to look.
            {
                // MC LevelRenderer:754 — Entity.setViewScale once per frame
                // from the effective render distance and the Entity Distance
                // option; every entity renderer's distance cull reads it.
                const float entityDistanceScaling = Platform::g_gameSettings.GetEntityDistanceScaling();
                Render::EntityCulling::SetViewScale(effectiveRenderDist, entityDistanceScaling);
                itemEntityRenderer.SetRenderDistanceChunks(effectiveRenderDist, entityDistanceScaling);
                Render::g_blockCubeEntityRenderer.SetRenderDistanceChunks(effectiveRenderDist, entityDistanceScaling);
            }
            {
                const auto nowForPartial = std::chrono::steady_clock::now();
                const float remaining =
                    std::chrono::duration<float>(nextClientTick - nowForPartial).count();
                const float tickSeconds =
                    std::chrono::duration<float>(CLIENT_TICK_INTERVAL).count();
                const float envPartialTick =
                    std::clamp(1.0f - remaining / tickSeconds, 0.0f, 1.0f);
                { PROFILE_ZONE_N("EnvUpdate");
                Render::EnvironmentState::Get().UpdateFrame(
                    envPartialTick, camera.GetForward(), camera.position.y,
                    effectiveRenderDist, Platform::g_gameSettings.GetFogEnabled());
                }
                // Where the viewer stands, for an OptiFine sky's biome and
                // height rules (OptiFine: the camera entity's block position).
                if (Client::g_clientChunkManager) {
                    const glm::ivec3 block(static_cast<int>(std::floor(camera.position.x)),
                                           static_cast<int>(std::floor(camera.position.y)),
                                           static_cast<int>(std::floor(camera.position.z)));
                    Render::g_skyRenderer.SetObserver(
                        block, Client::g_clientChunkManager->BiomeAtWorld(block.x, block.y, block.z));
                }
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
            // The near plane scales with the body: a small player must see
            // blocks closer than 5 cm, a large one may not. Published to the
            // chunk renderer, which builds the same projection itself.
            Render::ChunkRenderer::SetNearPlane(0.05f * std::max(player.physics.scale, 0.05f));
            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect,
                                              Render::ChunkRenderer::NearPlane(), farPlane);

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

            // Free camera: `frustum` — the one handed to RenderChunksAll AND
            // to every entity renderer below (MobRenderer, PlayerRenderer,
            // XpOrbRenderer, the portal pass gate) — is rebuilt from the
            // FROZEN player view, so all culling stays player-anchored while
            // proj/view above keep drawing from the fly camera. The same
            // player-view camera goes to the chunk renderer's cull override
            // (BFS origin, frustum filter, sort origins, IsSectionVisible).
            // Entity renderers still receive camera.position (the fly
            // position) for billboarding — sprites must face the real viewer
            // — which means their camera-DISTANCE culls also measure from the
            // fly position; accepted, it can only ever draw more, never cull
            // what the player view would keep.
            if (freeCamActive) {
                Render::Camera playerViewCam = camera;   // keeps fov + viewTilt
                playerViewCam.position = tpSavedPos;
                playerViewCam.yaw      = tpSavedYaw;
                playerViewCam.pitch    = tpSavedPitch;
                frustum = Frustum::FromMatrix(proj * playerViewCam.GetViewMatrix());
                if (Render::g_chunkRenderer) {
                    Render::g_chunkRenderer->SetCullOverride(playerViewCam, frustum);
                }
            }

            // Sky pass (MC addSkyPass): camera-centered, before terrain.
            // Own projection — the 512-radius sky disc would be clipped by
            // the main far plane at low render distances.
            {
                PROFILE_ZONE_N("SkyPass");
                glm::mat4 skyProj = glm::perspective(glm::radians(camera.fov), aspect, 0.05f, 2048.0f);
                glm::mat4 viewRotation = glm::mat4(glm::mat3(view));
                if (!Render::DevSkip("sky")) Render::g_skyRenderer.Render(skyProj, viewRotation);
            }

            // Main chunk rendering (includes frustum culling and all render passes)
            { PROFILE_ZONE_N("ChunkPass.Main");
            Render::RenderChunksAll(camera, frustum);
            }
            // Flicker diagnostics: the MAIN pass's numbers, read here and not
            // at frame end — a portal view into this same level re-enters
            // the renderer later in the frame and overwrites them.
            if (Render::FlickerDiag::Enabled() && Render::g_chunkRenderer) {
                auto* r = Render::g_chunkRenderer;
                Render::FlickerDiag::Record("main.visible", r->GetLastVisibleCount());
                Render::FlickerDiag::Record("main.reachable", r->GetLastReachableCount());
                Render::FlickerDiag::RecordState("main.bfsSource", static_cast<int64_t>(r->LastPrepareSource()));
                Render::FlickerDiag::Record("main.drawCalls", r->GetStats().totalDrawCalls);
            }

            // The cull override applies to the MAIN view only — clear it
            // before the portal see-through pass re-enters RenderChunksAll,
            // which must cull each recursion from its own virtual camera.
            if (freeCamActive && Render::g_chunkRenderer) {
                Render::g_chunkRenderer->ClearCullOverride();
            }

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
                // Frame boundary for the entity renderers' streaming buffers:
                // they alternate buffer sets per frame and append per call
                // (main pass, then each portal recursion below) — see
                // EntityFrame.hpp. Once, before the first of them runs.
                Render::EntityFrame::Begin();
                g_playerDrawDiag.Reset();
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
                        auto gC = portals.GetStraddlingGhost(renderPosCheck, 1.8f * rp.scale);
                        if (gC.valid && BodyStraddlesEntryPlane(renderPosCheck,
                                                                gC.entryClipPlane, 1.8f * rp.scale)) {
                            straddlingIds.insert(id);
                        }
                    }
                }
                const std::unordered_set<uint32_t>* bulkSkipIds = &straddlingIds;
#else
                const std::unordered_set<uint32_t>* bulkSkipIds = nullptr;
#endif
                Client::g_remotePlayerManager->UpdateBubbles(dt);

                // Dropped items, drawn with the same partialTick the remote
                // players use so everything in the world moves on one clock.
                auto drawWorldEntities = [&]() {
                    // Remote players go through the same cut as everything
                    // else below: a friend stepping into a nether portal is
                    // drawn up to the surface here and from it in the
                    // portal view, instead of whole on this side with the
                    // front half out the back of the frame.
                    if (!Render::DevSkip("players")) { PROFILE_ZONE_N("Render.RemotePlayers");
                    playerRenderer.Render(proj, view, camera.position, frustum,
                                          *Client::g_remotePlayerManager, partialTick, bulkSkipIds,
                                          Render::ChunkRenderer::PortalClipPlane());
                    g_playerDrawDiag.Add(g_playerDrawDiag.main, playerRenderer.LastTally()); }
                    if (!Render::DevSkip("items")) {
                    { PROFILE_ZONE_N("Render.ItemEntities");
                    itemEntityRenderer.Render(proj, view, camera.position, partialTick); }
                    { PROFILE_ZONE_N("Render.BlockCubeEntities");
                    Render::g_blockCubeEntityRenderer.Render(proj, view, camera.position,
                                                             partialTick); }
                    { PROFILE_ZONE_N("Render.XpOrbs");
                    xpOrbRenderer.Render(proj, view, camera.position, frustum, partialTick); }
                    }
                    if (Client::g_clientMobManager && !Render::DevSkip("mobs")) {
                        // The main frustum: MC extractVisibleEntities culls
                        // against the same frustum the chunk pass used.
                        PROFILE_ZONE_N("Render.Mobs");
                        mobRenderer.Render(proj, view, camera.position, frustum,
                                           *Client::g_clientMobManager, partialTick);
                    }
                };
#if ENABLE_IMMERSIVE_PORTALS
                // An entity half-way through a surface is CUT at it on this
                // side — the mod's cross-portal rendering. The part beyond
                // the plane is the portal view's (renderLevelView draws the
                // near level's crossers there, on the far side); drawn whole
                // here as well, that part stuck out behind the frame, where
                // a nether portal has open air: a cow walking through seemed
                // to come out the back in the same world. Everything not in
                // a surface draws as before; each surface then draws what is
                // in it, clipped to its own front. A two-faced portal cuts
                // with the face the entity's centre is in front of.
                {
                    PROFILE_ZONE_N("EntityCut");
                    std::vector<const Game::Immersive::Portal*> cutPortals;
                    const glm::dvec3 here(player.physics.position);
                    Client::ClientLevels::Active().Portals().ForEach([&](const Game::Immersive::Portal& p) {
                        if (!p.Has(Game::Immersive::PortalFlag::Visible)) return;
                        // Only a portal whose far side was drawn last frame:
                        // one culled, too small on screen or over the view
                        // budget shows nothing of its far side, so nothing cut
                        // at it is visible either. Each portal here costs an
                        // entity pass or two, and a dozen within 64 blocks
                        // were ~1.2 ms a frame of passes nobody could see.
                        if (!Render::g_immersivePortalRenderer.DrewLastFrame(p.id)) return;
                        glm::dvec3 mn, mx;
                        p.BoundingBox(mn, mx, 0.0);
                        if (glm::length(glm::clamp(here, mn, mx) - here) < 64.0) cutPortals.push_back(&p);
                    });
                    if (cutPortals.empty()) {
                        drawWorldEntities();
                    } else {
                        // The first pass visits every entity box against every
                        // cut portal anyway; remember which portals actually
                        // had something in them, so the per-portal near-side
                        // passes below run only for those.
                        std::vector<char> hasStraddler(cutPortals.size(), 0);
                        Render::EntityCulling::g_crossingFilter =
                            [&cutPortals, &hasStraddler](const glm::vec3& lo, const glm::vec3& hi) {
                                bool clear = true;
                                for (size_t i = 0; i < cutPortals.size(); ++i) {
                                    if (cutPortals[i]->IntersectsBox(glm::dvec3(lo), glm::dvec3(hi), 0.25)) {
                                        hasStraddler[i] = 1;
                                        clear = false;
                                    }
                                }
                                return clear;
                            };
                        drawWorldEntities();
                        // Each surface the VIEWER is in front of draws what
                        // is in it, clipped to this side. The face is the
                        // viewer's, not the entity's: chosen by the entity's
                        // centre, a body whose centre had just passed the
                        // plane went to the face behind — its far half drawn
                        // behind the surface (and covered by the portal
                        // view), its near half drawn by nobody, and the
                        // player vanished for the last step of a crossing.
                        for (size_t i = 0; i < cutPortals.size(); ++i) {
                            const Game::Immersive::Portal* p = cutPortals[i];
                            if (!hasStraddler[i]) continue;
                            if (!p->IsInFront(glm::dvec3(camera.position))) continue;
                            Render::EntityCulling::g_crossingFilter =
                                [p](const glm::vec3& lo, const glm::vec3& hi) {
                                    return p->IntersectsBox(glm::dvec3(lo), glm::dvec3(hi), 0.25);
                                };
                            Render::ChunkRenderer::SetPortalClipPlane(p->OuterClipPlane().AsClipPlane());
                            drawWorldEntities();
                        }
                        // The far level's entities that stick BACK through
                        // a surface — a player who has just arrived on the
                        // far side, half of them still on this side of the
                        // plane. Drawn here mapped back through the portal,
                        // clipped to the part behind the far surface; the
                        // portal view draws the other half. A crossing body
                        // is then whole on every frame from either side, the
                        // teleport itself invisible.
                        for (const Game::Immersive::Portal* p : cutPortals) {
                            if (p->IsMirror()) continue;
                            // Only the face the viewer stands in front of. A
                            // two-faced portal's other face maps the far
                            // world's front to the space BEHIND this one —
                            // drawn there, an emerging body stuck out the
                            // back of the frame.
                            if (!p->IsInFront(glm::dvec3(camera.position))) continue;
                            Client::ClientLevel* farLevel = Client::ClientLevels::Get(p->destDimension);
                            if (!farLevel) continue;
                            const Game::Immersive::Portal* r = farLevel->Portals().Get(p->reversePortalId);
                            if (!r) continue;
                            const glm::mat4 Minv(glm::inverse(p->TransformMatrix()));
                            const glm::mat4 backView = view * Minv;
                            const glm::vec3 farEye(p->TransformPoint(glm::dvec3(camera.position)));
                            const Frustum backFrustum = Frustum::FromMatrix(proj * backView);
                            const Game::Immersive::HalfSpace behind{ r->origin, -r->Normal() };
                            Render::ChunkRenderer::SetPortalClipPlane(behind.AsClipPlane());
                            Render::EntityCulling::g_crossingFilter =
                                [r](const glm::vec3& lo, const glm::vec3& hi) {
                                    return r->IntersectsBox(glm::dvec3(lo), glm::dvec3(hi), 0.25);
                                };
                            Client::ClientLevels::WithLevel(p->destDimension, [&]() {
                                if (Client::g_remotePlayerManager && !Render::DevSkip("players")) {
                                    // A player's copy on this client lags the
                                    // real one by a few ticks: just after a
                                    // crossing it is carried into the far
                                    // level still a step short of the far
                                    // surface, i.e. wholly on THIS side of the
                                    // plane. It belongs here too, so players
                                    // get a longer reach than the surface
                                    // test above.
                                    Render::EntityCulling::g_crossingFilter =
                                        [r](const glm::vec3& lo, const glm::vec3& hi) {
                                            // The box grown 1.5 m along the surface's
                                            // normal: a body that far behind the
                                            // surface still counts as in it.
                                            const glm::dvec3 ext = glm::abs(r->Normal()) * 1.5;
                                            return r->IntersectsBox(glm::dvec3(lo) - ext, glm::dvec3(hi) + ext, 0.25);
                                        };
                                    playerRenderer.Render(proj, backView, farEye, backFrustum,
                                                          *Client::g_remotePlayerManager, partialTick, nullptr,
                                                          behind.AsClipPlane());
                                    g_playerDrawDiag.Add(g_playerDrawDiag.reverse, playerRenderer.LastTally());
                                    Render::EntityCulling::g_crossingFilter =
                                        [r](const glm::vec3& lo, const glm::vec3& hi) {
                                            return r->IntersectsBox(glm::dvec3(lo), glm::dvec3(hi), 0.25);
                                        };
                                }
                                if (!Render::DevSkip("items")) {
                                    itemEntityRenderer.Render(proj, backView, farEye, partialTick);
                                    Render::g_blockCubeEntityRenderer.Render(proj, backView, farEye, partialTick);
                                    xpOrbRenderer.Render(proj, backView, farEye, backFrustum, partialTick);
                                }
                                if (Client::g_clientMobManager && !Render::DevSkip("mobs")) {
                                    mobRenderer.Render(proj, backView, farEye, backFrustum,
                                                       *Client::g_clientMobManager, partialTick);
                                }
                            });
                        }
                        Render::ChunkRenderer::SetPortalClipPlane(glm::vec4(0.0f));
                        Render::EntityCulling::g_crossingFilter = nullptr;
                    }
                }
#else
                drawWorldEntities();
#endif

                // Third person: the local player becomes visible (MC renders
                // the camera entity when the camera is detached). Uses the
                // SAVED eye yaw/pitch — the ThirdFront flip is a camera-only
                // transform; the body still faces where the player looks.
                // The free camera is a detached camera too, so the same rule
                // applies: the frozen body must be visible from the fly view.
                if (tpActive || freeCamActive) {
                    playerRenderer.RenderSingle(
                        proj, view, glm::vec3(player.visualPos),
                        tpSavedYaw, tpSavedYaw, tpSavedPitch,
                        player.physics.isSneaking,
                        static_cast<uint8_t>(player.color),
                        BodyScaleModel(glm::vec3(player.visualPos), player.physics.scale), glm::vec4(0.0f),
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
                        auto g = portals.GetStraddlingGhost(renderPos, 1.8f * rp.scale);
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
                                                     g.entryClipPlane, 1.8f * rp.scale)) {
                            continue;
                        }
                        const glm::mat4 bodyScale = BodyScaleModel(renderPos, rp.scale);
                        // 1) Entry-side body, clipped to the half on the
                        //    source side of the source portal plane. Uses
                        //    identity model — same world position as the
                        //    bulk render would have produced.
                        playerRenderer.RenderSingle(
                            proj, view, renderPos,
                            headYaw, bodyYaw, pitch, rp.isCrouching,
                            static_cast<uint8_t>(rp.color),
                            bodyScale, g.entryClipPlane);
                        // 2) Exit-side ghost, transformed through the
                        //    portal pair matrix and clipped to the
                        //    emerged half on the destination side.
                        playerRenderer.RenderSingle(
                            proj, view, renderPos,
                            headYaw, bodyYaw, pitch, rp.isCrouching,
                            static_cast<uint8_t>(rp.color),
                            g.transform * bodyScale, g.exitClipPlane);
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
#if ENABLE_IMMERSIVE_PORTALS
            if (player.lastBlockHitPortalId == 0)
#endif
            {
                RenderBlockHighlight(player, proj, view,
                                     playerController.PickEntity() != 0);
                RenderBlockBreakOverlay(playerController, proj, view);
            }
            // The fill tool's box, from its mark to the crosshair.
            {
                glm::ivec3 fillLo, fillHi;
                Game::BlockState fillState;
                if (playerController.FillPreview(player.lastBlockHit, fillLo, fillHi, fillState)) {
                    Render::g_fillPreviewRenderer.Render(proj, view, camera.position, fillState,
                                                         fillLo, fillHi);
                }
            }


            // BlockEntity per-frame render pass. Iterates every loaded
            // client chunk's BE map, frustum/distance-culls each BE, and
            // dispatches to its registered renderer. Runs after the chunk
            // solid + cutout passes (so BEs sit on top of the cube voxels)
            // but before portals (so they're masked correctly when seen
            // through a portal).
            if (Render::g_blockEntityRenderDispatcher && Client::g_clientChunkManager
                && !Render::DevSkip("blockentities")) {
                PROFILE_ZONE_N("BlockEntities");
                Render::g_blockEntityRenderDispatcher->RenderAll(
                    Client::g_clientChunkManager,
                    proj, view, camera.position, /*partialTick=*/0.0f);
            }

            // End portals — same slot in the frame as the block entities
            // above (opaque world geometry, before the portal pass), but off
            // a per-chunk position index rather than the BE map.
            Render::g_endPortalRenderer.Render(
                Client::g_clientChunkManager,
                proj, view, camera.position, /*partialTick=*/0.0f);
#if ENABLE_IMMERSIVE_PORTALS
            // Immersive portals: every portal of the active level, drawn
            // see-through, recursively. The renderer manages stencil masks,
            // depth, the camera transform and which level is bound; this
            // lambda draws one level from one viewpoint — what a frame's
            // main pass does, minus the things that belong to the viewer
            // (nametags, the block highlight, the held item).
            {
                PROFILE_ZONE_N("ImmersivePortalRender");
                const auto nowForImm = std::chrono::steady_clock::now();
                const float remainingImm =
                    std::chrono::duration<float>(nextClientTick - nowForImm).count();
                const float tickSecondsImm =
                    std::chrono::duration<float>(CLIENT_TICK_INTERVAL).count();
                const float partialTickImm =
                    Client::g_clientTickRate.IsEntityFrozen()
                        ? 1.0f
                        : std::clamp(1.0f - remainingImm / tickSecondsImm, 0.0f, 1.0f);
                const bool localCrouching = Input::IsKeyDown(Input::Key::LeftShift);

                auto renderLevelView = [&](const Render::ImmersivePortalRenderer::ViewContext& ctx) {
                    // Sky first, depth off, inside the mask: the far
                    // dimension's sky from the far camera's rotation.
                    {
                        const glm::mat4 skyProjIm =
                            glm::perspective(glm::radians(ctx.camera.fov), aspect, 0.05f, 2048.0f);
                        const glm::mat4 viewRotIm = glm::mat4(glm::mat3(ctx.view));
                        Render::g_skyRenderer.RenderForDimension(
                            Game::DimensionToRaw(ctx.dimension), skyProjIm, viewRotIm);
                    }
                    // Terrain of the bound level. For a level the player is
                    // not in, this view is its main view: record it so its
                    // mesh scheduler compiles what the portal reveals.
                    if (Render::g_chunkRenderer) {
                        Render::g_chunkRenderer->SetRecordMainView(
                            ctx.dimension != Client::ClientLevels::ActiveDimension());
                    }
                    Render::RenderChunksAll(ctx.camera, ctx.frustum, ctx.projection);
                    if (Render::g_chunkRenderer) Render::g_chunkRenderer->SetRecordMainView(false);

                    // Entities of the bound level, gated by the same frustum.
                    itemEntityRenderer.Render(ctx.projection, ctx.view, ctx.camera.position,
                                              ctx.partialTick);
                    {
                        // Only under OBEY_PORTAL_DIAG=1, like the other
                        // [PortalDiag] lines. It used to fire on its own
                        // whenever a far store held items, which after the
                        // immersive-portal work is just every session with a
                        // dropped item in another level.
                        static const bool kPortalDiag = std::getenv("OBEY_PORTAL_DIAG") != nullptr;
                        static auto lastItemDiag = std::chrono::steady_clock::now();
                        const auto nowDiag = std::chrono::steady_clock::now();
                        if (kPortalDiag && nowDiag - lastItemDiag >= std::chrono::seconds(2)) {
                            lastItemDiag = nowDiag;
                            const auto& t = itemEntityRenderer.LastTally();
                            std::string where;
                            if (Client::g_itemEntityManager) {
                                int n = 0;
                                for (const auto& [id, ce] : Client::g_itemEntityManager->GetEntities()) {
                                    if (n++ >= 3) break;
                                    char b[96];
                                    std::snprintf(b, sizeof(b), " #%d(%.1f,%.1f,%.1f)", id, ce.sim.pos.x, ce.sim.pos.y, ce.sim.pos.z);
                                    where += b;
                                }
                            }
                            Log::Info("[PortalDiag] items in %s: store=%d drawn=%d cull dist=%d frustum=%d section=%d farCam=(%.1f,%.1f,%.1f)%s",
                                      std::string(Game::DimensionName(ctx.dimension)).c_str(),
                                      t.entities, t.drawn, t.cullDistance, t.cullFrustum, t.cullSection,
                                      ctx.camera.position.x, ctx.camera.position.y, ctx.camera.position.z, where.c_str());
                        }
                    }
                    Render::g_blockCubeEntityRenderer.Render(ctx.projection, ctx.view,
                                                             ctx.camera.position, ctx.partialTick);
                    xpOrbRenderer.Render(ctx.projection, ctx.view, ctx.camera.position,
                                         ctx.frustum, ctx.partialTick);
                    if (Client::g_clientMobManager) {
                        mobRenderer.Render(ctx.projection, ctx.view, ctx.camera.position,
                                           ctx.frustum, *Client::g_clientMobManager,
                                           ctx.partialTick);
                    }
                    // Entities of the NEAR level that stick through this
                    // portal (an item lying on the plane, a block mid-fall):
                    // their far half belongs to this view. They are drawn
                    // from the outer camera (view · M⁻¹ · M) with the far
                    // clip plane pulled back into near space, so exactly the
                    // part behind the surface survives — the mod renders its
                    // crossing entities the same way. Every renderer here
                    // honours the clip plane (the mob shader through
                    // uEntityClipPlane).
                    if (ctx.through && !ctx.through->IsMirror()) {
                        const Game::Immersive::Portal& thr = *ctx.through;
                        const glm::mat4 M(thr.TransformMatrix());
                        const glm::mat4 crossView = ctx.view * M;
                        const glm::vec3 nearEye(thr.InverseTransformPoint(glm::dvec3(ctx.camera.position)));
                        const glm::vec4 farClip = Render::ChunkRenderer::PortalClipPlane();
                        Render::ChunkRenderer::SetPortalClipPlane(glm::transpose(M) * farClip);
                        const Frustum crossFrustum = Frustum::FromMatrix(ctx.projection * crossView);
                        // Only what is IN the surface — see EntityCulling::g_crossingFilter.
                        Render::EntityCulling::g_crossingFilter =
                            [&thr](const glm::vec3& lo, const glm::vec3& hi) {
                                return thr.IntersectsBox(glm::dvec3(lo), glm::dvec3(hi), 0.25);
                            };
                        Client::ClientLevels::WithLevel(thr.dimension, [&]() {
                            if (Client::g_remotePlayerManager) {
                                // The entity plane (the surface plus the mark
                                // band's margin), as the mobs use: the exact
                                // plane left the band the mark wiped undrawn.
                                playerRenderer.Render(ctx.projection, crossView, nearEye, crossFrustum,
                                                      *Client::g_remotePlayerManager, ctx.partialTick, nullptr,
                                                      Render::ChunkRenderer::PortalEntityClipPlane());
                                g_playerDrawDiag.Add(g_playerDrawDiag.crossers, playerRenderer.LastTally());
                            }
                            itemEntityRenderer.Render(ctx.projection, crossView, nearEye, ctx.partialTick);
                            Render::g_blockCubeEntityRenderer.Render(ctx.projection, crossView, nearEye,
                                                                     ctx.partialTick);
                            xpOrbRenderer.Render(ctx.projection, crossView, nearEye, crossFrustum,
                                                 ctx.partialTick);
                            if (Client::g_clientMobManager) {
                                mobRenderer.Render(ctx.projection, crossView, nearEye, crossFrustum,
                                                   *Client::g_clientMobManager, ctx.partialTick);
                            }
                        });
                        Render::EntityCulling::g_crossingFilter = nullptr;
                        Render::ChunkRenderer::SetPortalClipPlane(farClip);
                    }
                    // Clipped at the surface like the terrain: a player who
                    // has just arrived stands half behind it, and that half
                    // — between the surface and the far camera — filled
                    // the view for a frame.
                    if (Client::g_remotePlayerManager) {
                        playerRenderer.Render(ctx.projection, ctx.view, ctx.camera.position,
                                              ctx.frustum, *Client::g_remotePlayerManager,
                                              ctx.partialTick, nullptr,
                                              Render::ChunkRenderer::PortalEntityClipPlane());
                        g_playerDrawDiag.Add(g_playerDrawDiag.far, playerRenderer.LastTally());
                    }
                    // The viewer's own body, when the view shows the level
                    // they stand in (a portal looking back, or a same-
                    // dimension portal) — the mod's renderYourselfInPortal.
                    if (ctx.dimension == Client::ClientLevels::ActiveDimension() &&
                        ctx.through && ctx.through->Has(Game::Immersive::PortalFlag::RenderPlayer)) {
                        playerRenderer.RenderSingle(
                            ctx.projection, ctx.view, player.physics.position,
                            camera.yaw, camera.yaw, camera.pitch, localCrouching,
                            static_cast<uint8_t>(player.color),
                            BodyScaleModel(player.physics.position, player.physics.scale), glm::vec4(0.0f));
                    }
                    // Block entities and End portals of the bound level.
                    if (Render::g_blockEntityRenderDispatcher) {
                        Render::g_blockEntityRenderDispatcher->RenderAll(
                            Client::g_clientChunkManager, ctx.projection, ctx.view,
                            ctx.camera.position, ctx.partialTick);
                    }
                    Render::g_endPortalRenderer.Render(
                        Client::g_clientChunkManager, ctx.projection, ctx.view,
                        ctx.camera.position, ctx.partialTick);
                    // The particles that live in this level, billboarded to
                    // the far camera.
                    if (!Render::DevSkip("particles")) {
                        Render::g_mobParticleSystem.Render(ctx.projection, ctx.view,
                                                           ctx.camera.position, ctx.dimension);
                    }
#if ENABLE_PORTAL_GUN
                    // The gun's rims and sparks that live in this level —
                    // a pair with one end in the Nether shows that end's
                    // rim through the nether portal, not only once you are
                    // standing there. In immersive mode the renderer draws
                    // rims only, so the scene callback never fires.
                    // Not the rim (nor the sparks) of the surface this view
                    // comes out of: that surface is never drawn in its own
                    // view, and its rim lies on the view's clip plane — the
                    // other colour's ring around the edge at grazing angles.
                    const glm::dvec3 viewExit = ctx.through
                        ? (ctx.through->IsMirror() ? ctx.through->origin : ctx.through->destination)
                        : glm::dvec3(0.0);
                    const glm::vec3 viewExitF(viewExit);
                    Render::g_portalRenderer.Render(
                        ctx.projection, ctx.view, ctx.camera, ctx.frustum, aspect, farPlane,
                        static_cast<int8_t>(Game::DimensionToRaw(ctx.dimension)),
                        [](const Render::Camera&, const Frustum&, const glm::mat4&) {},
                        ctx.through ? &viewExit : nullptr);
                    Render::g_portalParticleSystem.Render(ctx.projection, ctx.view,
                                                          ctx.camera.position, ctx.dimension,
                                                          ctx.through ? &viewExitF : nullptr);
#endif
                    // The block under the crosshair, when the crosshair got
                    // there through THIS portal: outline and crack overlay
                    // drawn inside the view, in the far level's space.
                    if (ctx.through && ctx.through->id == player.lastBlockHitPortalId &&
                        ctx.dimension == player.lastBlockHitDimension) {
                        RenderBlockHighlight(player, ctx.projection, ctx.view,
                                             playerController.PickEntity() != 0);
                        RenderBlockBreakOverlay(playerController, ctx.projection, ctx.view);
                    }
                };

                // The far side's entities that stick out of a portal into the
                // world being drawn (see ImmersivePortalRenderer::Render).
                auto renderCrossers = [&](const Render::ImmersivePortalRenderer::ViewContext& ctx) {
                    // Far-space entities: bring the box back to the near side
                    // (corner-wise, then its AABB) and ask the surface.
                    const Game::Immersive::Portal* thr = ctx.through;
                    Render::EntityCulling::g_crossingFilter =
                        [thr](const glm::vec3& lo, const glm::vec3& hi) {
                            if (!thr) return true;
                            glm::dvec3 mn(1e300), mx(-1e300);
                            for (int i = 0; i < 8; ++i) {
                                const glm::dvec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
                                const glm::dvec3 nearC = thr->InverseTransformPoint(c);
                                mn = glm::min(mn, nearC);
                                mx = glm::max(mx, nearC);
                            }
                            return thr->IntersectsBox(mn, mx, 0.25);
                        };
                    itemEntityRenderer.Render(ctx.projection, ctx.view, ctx.camera.position,
                                              ctx.partialTick);
                    Render::g_blockCubeEntityRenderer.Render(ctx.projection, ctx.view,
                                                             ctx.camera.position, ctx.partialTick);
                    xpOrbRenderer.Render(ctx.projection, ctx.view, ctx.camera.position,
                                         ctx.frustum, ctx.partialTick);
                    if (Client::g_clientMobManager) {
                        mobRenderer.Render(ctx.projection, ctx.view, ctx.camera.position, ctx.frustum,
                                           *Client::g_clientMobManager, ctx.partialTick);
                    }
                    Render::EntityCulling::g_crossingFilter = nullptr;
                };
                Render::g_immersivePortalRenderer.Render(proj, view, camera, frustum, aspect,
                                                         effectiveRenderDist, partialTickImm, renderLevelView,
                                                         renderCrossers);
                // OBEY_PORTAL_DIAG=1: one line per frame for each remote
                // player within a few blocks of a surface — where they are
                // filed, and which pass drew them or what culled them.
                if (g_portalDiag && Client::g_remotePlayerManager) {
                    for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                        bool nearPortal = false;
                        if (Client::ClientLevel* lvl = Client::ClientLevels::Get(rp.dimension)) {
                            lvl->Portals().ForEach([&](const Game::Immersive::Portal& p) {
                                glm::dvec3 mn, mx;
                                p.BoundingBox(mn, mx, 0.0);
                                const glm::dvec3 c(rp.position);
                                if (glm::length(glm::clamp(c, mn, mx) - c) < 4.0) nearPortal = true;
                            });
                        }
                        if (!nearPortal) continue;
                        const auto& d = g_playerDrawDiag;
                        Log::Info("[PortalDiag] player %u %s (%.2f,%.2f,%.2f) lerp=%d | main in=%d drawn=%d cull(d/f/x)=%d/%d/%d"
                                  " | crossers in=%d drawn=%d cull=%d/%d/%d | reverse in=%d drawn=%d cull=%d/%d/%d"
                                  " | far in=%d drawn=%d cull=%d/%d/%d",
                                  id, std::string(Game::DimensionName(rp.dimension)).c_str(),
                                  rp.position.x, rp.position.y, rp.position.z, rp.lerpSteps,
                                  d.main[0], d.main[1], d.main[2], d.main[3], d.main[4],
                                  d.crossers[0], d.crossers[1], d.crossers[2], d.crossers[3], d.crossers[4],
                                  d.reverse[0], d.reverse[1], d.reverse[2], d.reverse[3], d.reverse[4],
                                  d.far[0], d.far[1], d.far[2], d.far[3], d.far[4]);
                    }
                }
            }
#endif
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
                    static_cast<int8_t>(Game::DimensionToRaw(Client::ClientLevels::ActiveDimension())),
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
                        Render::g_blockCubeEntityRenderer.Render(
                            obliqueProj, virtView, virtCam.position, partialTickPortal);
                        // The virtual camera's frustum — the one the chunk
                        // pass above was culled with, so the entity gate and
                        // the terrain agree on what this recursion sees.
                        xpOrbRenderer.Render(obliqueProj, virtView,
                                             virtCam.position, virtFrust,
                                             partialTickPortal);
                        if (Client::g_clientMobManager) {
                            mobRenderer.Render(obliqueProj, virtView, virtCam.position,
                                               virtFrust,
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
                                                  virtCam.position, virtFrust,
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
            { PROFILE_ZONE_N("PortalParticles");
            Render::g_portalParticleSystem.Update(dt, Client::GetClientPortalManager());
            Render::g_portalParticleSystem.Render(proj, view, camera.position,
                                                  Client::ClientLevels::ActiveDimension());
            }

#endif

            // Mob/world particles — drain the spawns the client mobs queued
            // this tick (ClientLevelBridge), step the 20 Hz simulation, and
            // draw. After the world pass (depth-tested, no depth write),
            // before the clouds — MC's particle pass sits the same way.
            { PROFILE_ZONE_N("Particles.Update");
            Render::g_mobParticleSystem.Update(dt, camera.position);
            }
            if (!Render::DevSkip("particles")) {
                PROFILE_ZONE_N("Particles.Render");
                Render::g_mobParticleSystem.Render(proj, view, camera.position,
                                                   Client::ClientLevels::ActiveDimension());
            }

            // Clouds — MC's cloud pass runs after terrain and particles
            // (translucent, depth-tested against the world, no depth write).
            //
            // Overworld only. MC gives the Nether and the End a cloud level of
            // Float.NaN (DimensionSpecialEffects.NetherEffects / EndEffects),
            // which is its way of saying "no cloud layer" — a band of white
            // clouds across the Nether's ceiling would be very obviously wrong.
            if (!Render::g_skyRenderer.SkyHidden()
                && Render::g_skyRenderer.CurrentSkyboxIsEnd() == false
                && !Render::DevSkip("clouds")) {
                PROFILE_ZONE_N("CloudPass");
                const auto nowForPartial = std::chrono::steady_clock::now();
                const float remaining =
                    std::chrono::duration<float>(nextClientTick - nowForPartial).count();
                const float tickSeconds =
                    std::chrono::duration<float>(CLIENT_TICK_INTERVAL).count();
                const float cloudPartialTick =
                    std::clamp(1.0f - remaining / tickSeconds, 0.0f, 1.0f);
                // A chosen sky pack's own clouds.png (usually blank: the
                // clouds are painted into its sky). Empty = vanilla.
                Render::g_cloudRenderer.SetCloudTexture(Render::g_skyRenderer.PackCloudTexture());
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
            if (camera.IsFirstPerson() && !freeCamActive &&
                player.inventory.GetSelectedItem() == Game::Items::PortalGun) {
                const float fbAspect = (height > 0)
                    ? static_cast<float>(width) / static_cast<float>(height)
                    : 16.0f / 9.0f;
                PROFILE_ZONE_N("GunViewmodel");
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
            // No viewmodel while the free camera is detached — the hands
            // belong to the frozen player, whose body is drawn in the world
            // instead (the forced third-person render above).
            const bool drawHeldItem = camera.IsFirstPerson() && !freeCamActive &&
                (renderMainHand ||
                 !player.inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN).IsEmpty());
            if (drawHeldItem && !Render::DevSkip("helditem")) {
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
            g_hudRenderer.SetExperience(player.xpProgress, player.xpLevel);
            // MC Gui gates the survival stat block on gameMode.canHurtPlayer()
            // (Gui.java:524). The extra !gameModeKnown term covers a window MC
            // doesn't have: our render loop spins from the moment the socket
            // connects, so without it a creative joiner gets a frame or two of
            // hearts and hunger drawn off the survival default before the first
            // abilities packet lands.
            g_hudRenderer.SetStatsHidden(!player.gameModeKnown ||
                                         player.IsCreative() || player.IsSpectator());
            if (!Render::DevSkip("hud")) {
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
                !freeCamActive &&           // detached camera cannot interact
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
                // Transport: relay is knowable from the session's own setup —
                // a relayed join is exactly the one that carried a relay
                // ticket, and the preamble is set from it at connect time.
                // Singleplayer is the loopback case (which also skips
                // compression, matching MC's isMemoryConnection).
                if (!isRemoteClient) {
                    netSnap.transport =
                        Debug::NetworkMetricsSnapshot::Transport::Singleplayer;
                } else if (sessionUsedRelay) {
                    netSnap.transport =
                        Debug::NetworkMetricsSnapshot::Transport::Relay;
                } else {
                    netSnap.transport =
                        Debug::NetworkMetricsSnapshot::Transport::Direct;
                }
                netSnap.peerAddress = connectHost + ":" + std::to_string(serverPort);
                if (networkClient) {
                    if (auto conn = networkClient->GetConnection()) {
                        netSnap.compressionEnabled = conn->CompressionEnabled();
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
                    wi.targetStateIndex = hit.state.Index();
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
                        for (const auto& [k, v] : def.PropertiesOf(hit.state.Index())) {
                            if (!wi.targetStateProps.empty()) wi.targetStateProps += ", ";
                            wi.targetStateProps += k + "=" + v;
                        }
                    }

                    static const char* kFaceNames[] = {
                        "+X east", "-X west", "+Y up", "-Y down", "+Z south", "-Z north" };
                    wi.targetFace = (hit.hitFace >= 0 && hit.hitFace < 6)
                                      ? kFaceNames[hit.hitFace] : "?";

                    const auto& shape =
                        Game::BlockRegistry::GetBlockShape(hit.state);
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

            // OBEY_FLICKER_DIAG=1: the main view's per-frame numbers, then
            // one line for anything that went A -> B -> A this frame (the
            // portal renderer recorded its own above). See FlickerDiag.hpp.
            if (Render::FlickerDiag::Enabled() && Client::ClientLevels::HasSession()) {
                Client::ClientLevel& active = Client::ClientLevels::Active();
                if (auto* c = active.Chunks()) {
                    Render::FlickerDiag::Record("main.chunks", static_cast<int64_t>(c->GetLoadedChunkCount()));
                }
                Render::FlickerDiag::RecordState("main.dim", static_cast<int64_t>(Game::DimensionToRaw(active.Dimension())));
                Render::FlickerDiag::RecordState("levels", static_cast<int64_t>(Client::ClientLevels::Count()));
                {
                    const auto& env = Render::EnvironmentState::Get().Frame();
                    const int64_t fog = (static_cast<int64_t>(env.fogColor.r * 255.0f) << 16) |
                                        (static_cast<int64_t>(env.fogColor.g * 255.0f) << 8) |
                                         static_cast<int64_t>(env.fogColor.b * 255.0f);
                    Render::FlickerDiag::Record("env.fog", fog);
                    Render::FlickerDiag::Record("env.sky", static_cast<int64_t>(env.skyBrightness * 1000.0f));
                }
                char cam[128];
                std::snprintf(cam, sizeof(cam), "(%.1f,%.1f,%.1f) yaw=%.0f pitch=%.0f",
                              camera.position.x, camera.position.y, camera.position.z, camera.yaw, camera.pitch);
                Render::FlickerDiag::Note("cam", cam);
                Render::FlickerDiag::EndFrame();
            }
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
                            Platform::g_gameSettings.GetSimulationDistance(),
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

            // Max Framerate option (Video Settings). 260 = Unlimited. Applied
            // whether or not VSync is on, as MC does (Minecraft.renderFrame:
            // 1342 limits whenever framerateLimit < 260): with VSync at 60 Hz
            // and a 120 cap the swap already ate the budget and this never
            // sleeps; with VSync at 120 Hz and a 60 cap, the player asked for
            // 60. sleep_until keeps the cap steady without burning a core.
            //
            // The deadline is advanced by one budget per frame and dropped
            // to `now` when the frame overran it — a frame that took longer
            // than the budget must NOT then sleep a further budget (the
            // previous "resync" did exactly that, which is why the limiter
            // used to be disabled under VSync).
            {
                const int maxFps = Platform::g_gameSettings.GetMaxFPS();
                if (maxFps > 0 && maxFps < 260) {
                    static auto s_nextFrameDeadline = std::chrono::steady_clock::now();
                    const auto frameBudget = std::chrono::nanoseconds(1'000'000'000LL / maxFps);
                    const auto nowClock = std::chrono::steady_clock::now();
                    s_nextFrameDeadline += frameBudget;
                    if (s_nextFrameDeadline < nowClock) {
                        s_nextFrameDeadline = nowClock; // overran the budget: no sleep
                    } else {
                        std::this_thread::sleep_until(s_nextFrameDeadline);
                    }
                }
            }

            // Restore the logical eye camera after the third-person render
            // frame (see the F5 block above the render phase). The free
            // camera's swap restores through the same site — next frame's
            // input/tick phases (raycast, PlayerMoveC2S) must read the frozen
            // player pose, never the fly pose.
            if (tpActive || freeCamActive) {
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
        // teardown, and the socket closing below must not be mistaken for the
        // server dropping us. Harmless now that the callback only sets a flag
        // the loop has already left, but it was load-bearing when that callback
        // closed the window — which is what made "Save and Quit to Title" need
        // this line, and what made an UNSOLICITED disconnect quit to desktop.
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

        // A recording still running is written out now, while the session's
        // player is still the one it sampled.
        poseRecorder.Stop();

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
                        entry.difficulty      = Server::g_integratedServer->GetDifficulty();   // /difficulty during the session
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
        // Back to the global resource pack selection (options.txt) now that
        // the world's own, if it had one, is no longer in play.
        Resources::SelectFromOptionLists(
            Resources::ParsePackList(Platform::g_gameSettings.GetResourcePacks()),
            Resources::ParsePackList(Platform::g_gameSettings.GetIncompatibleResourcePacks()));
        ReloadResources();

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
        Client::ClientLevels::DestroySession();
        Log::Info("✓ Client systems shutdown");

        // 7. Shutdown server systems (host only)
        if (!isRemoteClient) {
            Log::Info("Shutting down server systems...");
            Server::ShutdownIntegratedServer();
            Log::Info("✓ Server systems shutdown");
        }

        // 8. Shutdown rendering systems (the chunk renderer went with the levels)

        // 8a. Session-scoped render resources
        playerRenderer.Shutdown();
        Client::g_remotePlayerManager.reset();
        itemEntityRenderer.Shutdown();
        Render::g_blockCubeEntityRenderer.Shutdown();
        Render::g_fillPreviewRenderer.Shutdown();
        xpOrbRenderer.Shutdown();
        mobRenderer.Shutdown();
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
        Render::g_endPortalRenderer.Shutdown();
#if ENABLE_IMMERSIVE_PORTALS
        Render::g_immersivePortalRenderer.Shutdown();
#endif
        Render::SkyboxThumbnails::Get().Shutdown();   // preview cards, before the backend goes
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
            Core::ShutdownTickParallel();
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

        // 13. Sentry LAST, but still inside Run() — never from atexit. Joining
        // its HTTP worker can block while that worker finishes a request, and
        // that has to happen while the C runtime is intact.
        Log::Info("Closing crash reporting...");
        CloseSentryOnce();
        Log::Info("✓ Crash reporting closed");

        // Stamp the log as ending deliberately. Without this marker a truncated
        // log and a clean one look identical, so you cannot tell "crashed" from
        // "player quit" — which matters most in the case the crash handler
        // never ran (see CrashHandler.hpp on crashpad winning the race).
        Log::CloseLogFile();

        return 0;
    }

} // namespace PlatformMain