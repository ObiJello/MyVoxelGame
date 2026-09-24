// File: src/client/renderer/gui/debug/DebugKeyHandler.cpp
#include "DebugKeyHandler.hpp"
#include "DebugScreenEntries.hpp"
#include "DebugScreenOverlay.hpp"
#include "DebugOptionsScreen.hpp"
#include "GameModeSwitcherScreen.hpp"
#include "client/entity/Player.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/input/Input.hpp"
#include "client/input/KeyMapping.hpp"
#include "client/input/PlayerController.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/core/Camera.hpp"
#include "client/renderer/debug/DebugRenderer.hpp"
#include "client/renderer/debug/DebugSystem.hpp"
#include "client/renderer/gui/GuiGraphics.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/level/DimensionId.hpp"
#include "client/resource/ResourcePacks.hpp"
#include "platform/GameDirectory.hpp"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

// The frame loop keeps the player and camera; the handler reads them
// through these (set by PlatformMain each frame, see DebugScreen::Context).
namespace Render::DebugScreen {
    namespace {
        const Context* g_context = nullptr;
        int64_t NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        template <typename... Args>
        std::string Fmt(const char* fmt, Args... args) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), fmt, args...);
            return buf;
        }
        const char* GameModeName(int mode) {
            switch (mode) { case 1: return "creative"; case 2: return "adventure"; case 3: return "spectator"; default: return "survival"; }
        }
    }

    void SetDebugContext(const Context* ctx) { g_context = ctx; }

    DebugKeyHandler& KeyHandler() {
        static DebugKeyHandler s_handler;
        return s_handler;
    }

    void SilentPauseScreen::Render(GuiGraphics& g, int, int, float) {
        // MC PauseScreen.init: StringWidget(width/2 - textWidth/2, 10, …, title).
        g.DrawCenteredString("Game Paused", m_width / 2, 10, 0xFFFFFFFF);
    }

    void DebugKeyHandler::Feedback(const std::string& message) {
        if (m_cb.showChat) m_cb.showChat("\xC2\xA7" "e\xC2\xA7" "l[Debug]:\xC2\xA7r " + message);
        Log::Info("[Debug] %s", message.c_str());
    }

    void DebugKeyHandler::FeedbackWithFile(const std::string& before, const std::string& linkText,
                                           const std::string& openPath) {
        if (m_cb.showChatFileLink) {
            m_cb.showChatFileLink("\xC2\xA7" "e\xC2\xA7" "l[Debug]:\xC2\xA7r " + before, linkText, openPath);
        } else if (m_cb.showChat) {
            m_cb.showChat("\xC2\xA7" "e\xC2\xA7" "l[Debug]:\xC2\xA7r " + before + "\xC2\xA7n" + linkText);
        }
        Log::Info("[Debug] %s%s", before.c_str(), linkText.c_str());
    }

    void DebugKeyHandler::Warning(const std::string& message) {
        if (m_cb.showChat) m_cb.showChat("\xC2\xA7" "c\xC2\xA7" "l[Debug]:\xC2\xA7r " + message);
        Log::Warning("[Debug] %s", message.c_str());
    }

    void DebugKeyHandler::SetClipboard(const std::string& text) {
        if (!text.empty()) Input::SetClipboardText(text);
    }

    bool DebugKeyHandler::Matches(const Input::KeyMapping* mapping, int glfwKey) const {
        return mapping && mapping->key == Input::BoundKey::Keyboard(glfwKey);
    }

    void DebugKeyHandler::NotifyGameMode(int mode) {
        if (mode == m_currentGameMode) return;
        if (m_currentGameMode >= 0) m_previousGameMode = m_currentGameMode;
        m_currentGameMode = mode;
    }

    // MC KeyboardHandler.copyRecreateCommand: /setblock for the targeted
    // block (SetBlockCommand runs it), /summon for the targeted entity.
    // There is no NBT query here, so the data-carrying variants copy the
    // same commands.
    void DebugKeyHandler::CopyRecreateCommand(bool addNbt, bool pullFromServer) {
        (void)addNbt; (void)pullFromServer;
        if (!g_context || !g_context->player) return;
        if (g_context->controller) {
            const int32_t id = g_context->controller->PickEntity();
            if (id != 0) {
                std::string name;
                glm::dvec3 pos(0.0);
                if (Client::ClientLevels::HasSession()) {
                    if (Client::ClientMobManager* mobs = Client::ClientLevels::Active().Mobs()) {
                        auto it = mobs->All().find(id);
                        if (it != mobs->All().end() && it->second.mob) {
                            name = std::string(Game::GetEntityTypeInfo(it->second.mob->GetType()).slug);
                            pos = it->second.mob->position;
                        }
                    }
                }
                if (name.empty() && Client::g_remotePlayerManager) {
                    auto it = Client::g_remotePlayerManager->GetPlayers().find(static_cast<uint32_t>(id));
                    if (it != Client::g_remotePlayerManager->GetPlayers().end()) { name = "player"; pos = it->second.position; }
                }
                if (!name.empty()) {
                    SetClipboard(Fmt("/summon minecraft:%s %.2f %.2f %.2f", name.c_str(), pos.x, pos.y, pos.z));
                    Feedback("Copied client-side entity data to clipboard");
                    return;
                }
            }
        }
        const auto& hit = g_context->player->lastBlockHit;
        if (!hit || !Client::g_clientBlockAccess) return;
        const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(hit->blockPos.x, hit->blockPos.y, hit->blockPos.z);
        // BlockStateParser.serialize: minecraft:id[prop=value,...].
        std::string desc = "minecraft:" + Game::BlockRegistry::Get(state.Block()).registrySlug;
        const uint16_t n = Game::BlockStates::PropertyCount(state.Block());
        if (n > 0) {
            desc += '[';
            for (uint16_t slot = 0; slot < n; ++slot) {
                const Game::PropertyId prop = Game::BlockStates::PropertyAt(state.Block(), slot);
                if (slot) desc += ',';
                desc += std::string(Game::BlockStates::PropertyName(prop)) + "=" + std::string(state.GetName(prop));
            }
            desc += ']';
        }
        SetClipboard(Fmt("/setblock %d %d %d %s", hit->blockPos.x, hit->blockPos.y, hit->blockPos.z, desc.c_str()));
        Feedback("Copied client-side block data to clipboard");
    }

    // MC VersionCommand.dumpVersion.
    void DebugKeyHandler::DumpVersion() {
        if (!m_cb.showChat) return;
        const std::string full = Render::GetScreenManager().GetVersionString();   // "MyVoxelGame 0.1.N"
        std::string number = full;
        if (const size_t sp = full.rfind(' '); sp != std::string::npos) number = full.substr(sp + 1);
        Feedback("Client version info:");
        m_cb.showChat("Version ID: " + number);
        m_cb.showChat("Version name: " + full);
        m_cb.showChat("Data version: " + std::to_string(Game::Save::kDefaultDataVersion));
        m_cb.showChat("Series: main");
        m_cb.showChat(Fmt("Protocol version: %d (0x%x)", 1, 1));
        m_cb.showChat(std::string("Build time: ") + __DATE__ + " " + __TIME__);
        m_cb.showChat("Resource pack format: 76.0");
        m_cb.showChat("Data pack format: 76.0");
        m_cb.showChat(std::string("Renderer: ") + (Render::g_renderBackend ? Render::g_renderBackend->GetName() : "none"));
#ifdef NDEBUG
        m_cb.showChat("Stable release");
#else
        m_cb.showChat("Unstable snapshot");
#endif
    }

    bool DebugKeyHandler::HandleDebugKeys(int glfwKey, int mods) {
        // MC: while the crash countdown runs every other chord is swallowed.
        if (m_crashKeyTime > 0 && m_crashKeyTime < NowMs() - 100) return true;

        using namespace Input;
        EntryList& entries = Entries();
        DebugScreenOverlay& overlay = Overlay();
        bool debugAction = false;
        const bool reduced = EntryList::ShowOnlyReducedInfo();
        const bool hasLevel = m_cb.hasLevel ? m_cb.hasLevel() : false;

        if (Matches(Binds::DebugReloadChunk, glfwKey)) {
            if (m_cb.reloadChunks) m_cb.reloadChunks();
            Feedback("Reloading all chunks");
            debugAction = true;
        }
        if (Matches(Binds::DebugShowHitboxes, glfwKey) && hasLevel && !reduced) {
            const bool shown = entries.ToggleStatus(Ids::EntityHitboxes);
            Feedback(shown ? "Hitboxes: shown" : "Hitboxes: hidden");
            debugAction = true;
        }
        if (Matches(Binds::DebugClearChat, glfwKey)) {
            if (m_cb.clearChat) m_cb.clearChat();
            debugAction = true;
        }
        if (Matches(Binds::DebugShowChunkBorders, glfwKey) && hasLevel && !reduced) {
            const bool shown = entries.ToggleStatus(Ids::ChunkBorders);
            Feedback(shown ? "Chunk borders: shown" : "Chunk borders: hidden");
            debugAction = true;
        }
        if (Matches(Binds::DebugShowAdvancedTooltips, glfwKey)) {
            auto& s = Platform::g_gameSettings;
            s.SetAdvancedItemTooltips(!s.GetAdvancedItemTooltips());
            Feedback(s.GetAdvancedItemTooltips() ? "Advanced tooltips: shown" : "Advanced tooltips: hidden");
            s.Save();
            debugAction = true;
        }
        if (Matches(Binds::DebugCopyRecreateCommand, glfwKey)) {
            if (hasLevel && !reduced) CopyRecreateCommand(true, !(mods & GLFW_MOD_SHIFT));
            debugAction = true;
        }
        if (Matches(Binds::DebugSpectate, glfwKey)) {
            if (hasLevel && m_cb.sendCommand) {
                if (m_currentGameMode != 3) {
                    m_cb.sendCommand("/gamemode spectator");
                } else {
                    const int back = m_previousGameMode >= 0 && m_previousGameMode != 3 ? m_previousGameMode : 1;
                    m_cb.sendCommand(std::string("/gamemode ") + GameModeName(back));
                }
            } else {
                Feedback("Unable to switch game mode; no permission");
            }
            debugAction = true;
        }
        if (Matches(Binds::DebugSwitchGameMode, glfwKey) && hasLevel) {
            auto& screens = Render::GetScreenManager();
            if (auto* switcher = dynamic_cast<GameModeSwitcherScreen*>(screens.Current())) {
                switcher->SelectNext();
            } else if (screens.Empty()) {
                const int current = m_currentGameMode >= 0 ? m_currentGameMode : 0;
                screens.Push(std::make_unique<GameModeSwitcherScreen>(current, m_previousGameMode, [this](int mode) {
                    if (m_cb.sendCommand) m_cb.sendCommand(std::string("/gamemode ") + GameModeName(mode));
                }));
            }
            debugAction = true;
        }
        if (Matches(Binds::DebugDebugOptions, glfwKey)) {
            auto& screens = Render::GetScreenManager();
            if (dynamic_cast<DebugOptionsScreen*>(screens.Current())) {
                screens.Current()->OnClose();
            } else {
                if (screens.Current()) screens.Current()->OnClose();
                screens.Push(std::make_unique<DebugOptionsScreen>());
            }
            debugAction = true;
        }
        if (Matches(Binds::DebugFocusPause, glfwKey)) {
            auto& s = Platform::g_gameSettings;
            s.SetPauseOnLostFocus(!s.GetPauseOnLostFocus());
            s.Save();
            Feedback(s.GetPauseOnLostFocus() ? "Pause on lost focus: enabled" : "Pause on lost focus: disabled");
            debugAction = true;
        }
        if (Matches(Binds::DebugDumpDynamicTextures, glfwKey)) {
            // MC: "Saved dynamic textures to %s", the path underlined with a
            // ClickEvent.OpenFile on the folder.
            std::string shown, absolute;
            if (m_cb.dumpDynamicTextures && m_cb.dumpDynamicTextures(shown, absolute)) {
                FeedbackWithFile("Saved dynamic textures to ", shown, absolute);
            } else {
                Warning("Could not save dynamic textures");
            }
            debugAction = true;
        }
        if (Matches(Binds::DebugReloadResourcePacks, glfwKey)) {
            Feedback("Reloaded resource packs");
            if (m_cb.reloadResourcePacks) m_cb.reloadResourcePacks();
            debugAction = true;
        }
        if (Matches(Binds::DebugProfiling, glfwKey)) {
            if (m_cb.toggleProfiling && m_cb.toggleProfiling()) {
                Feedback(Fmt("Profiling started for %d seconds. Use %s + %s to stop early", 10,
                             Binds::DebugModifier ? Binds::DebugModifier->key.DisplayName().c_str() : "F3",
                             Binds::DebugProfiling ? Binds::DebugProfiling->key.DisplayName().c_str() : "L"));
            }
            debugAction = true;
        }
        if (Matches(Binds::DebugCopyLocation, glfwKey) && hasLevel && !reduced && g_context && g_context->player && g_context->camera) {
            Feedback("Copied location to clipboard");
            const glm::vec3 p = g_context->player->physics.position;
            SetClipboard(Fmt("/execute in minecraft:%s run tp @s %.2f %.2f %.2f %.2f %.2f",
                             std::string(Game::DimensionName(Client::ClientLevels::ActiveDimension())).c_str(),
                             p.x, p.y, p.z, g_context->camera->yaw, g_context->camera->pitch));
            debugAction = true;
        }
        if (Matches(Binds::DebugDumpVersion, glfwKey)) {
            DumpVersion();
            debugAction = true;
        }
        if (Matches(Binds::DebugProfilingChart, glfwKey)) { overlay.ToggleProfilerChart(); debugAction = true; }
        if (Matches(Binds::DebugFpsCharts, glfwKey))      { overlay.ToggleFpsCharts();     debugAction = true; }
        if (Matches(Binds::DebugNetworkCharts, glfwKey))  { overlay.ToggleNetworkCharts(); debugAction = true; }
        if (Matches(Binds::DebugLightmapTexture, glfwKey)) { overlay.ToggleLightmapTexture(); debugAction = true; }
        if (Matches(Binds::DebugImGuiPanels, glfwKey)) {
            Debug::DebugSystem::ToggleDebugUI();
            Feedback(Debug::DebugSystem::IsDebugUIEnabled() ? "Debug panels: shown" : "Debug panels: hidden");
            debugAction = true;
        }
        if (Matches(Binds::DebugSwitchTranslucencyMode, glfwKey)) {
            // No improved-transparency (order-independent) pipeline exists in
            // this engine; the chord reports that instead of pretending.
            Feedback("Improved transparency: not available in this renderer");
            debugAction = true;
        }
        return debugAction;
    }

    void DebugKeyHandler::ProcessFrame() {
        using namespace Input;
        m_consumedThisFrame = false;
        const KeyMapping* modifier = Binds::DebugModifier;
        const KeyMapping* overlayKey = Binds::DebugOverlay;
        const bool modifierAndOverlaySame = modifier && overlayKey && modifier->key == overlayKey->key;
        const KeyMapping* crash = Binds::DebugCrash;

        RawKeyEvent e;
        while (PopRawKeyEvent(e)) {
            const bool press = e.action == GLFW_PRESS;
            if (Matches(modifier, e.glfwKey)) m_modifierDown = press;

            // MC: the crash timer starts when both F3 and C are physically
            // down, and clears the moment either lifts.
            const bool crashDown = crash && crash->key.IsBound() && IsGlfwKeyDown(crash->key.code);
            if (m_crashKeyTime > 0) {
                if (!crashDown || !m_modifierDown) m_crashKeyTime = -1;
            } else if (crashDown && m_modifierDown && press) {
                m_usedDebugKeyAsModifier = modifierAndOverlaySame;
                m_crashKeyTime = NowMs();
                m_crashKeyReportedTime = NowMs();
                m_crashKeyReportedCount = 0;
            }

            auto& screens = Render::GetScreenManager();
            Screen* screen = screens.Current();

            // The game-mode switcher commits on the modifier's release.
            if (!press && Matches(modifier, e.glfwKey)) {
                if (auto* switcher = dynamic_cast<GameModeSwitcherScreen*>(screen)) {
                    switcher->CommitAndClose();
                    m_usedDebugKeyAsModifier = false;
                    continue;
                }
            }

            if (modifierAndOverlaySame && Matches(modifier, e.glfwKey) && !press) {
                if (m_usedDebugKeyAsModifier) m_usedDebugKeyAsModifier = false;
                else Entries().ToggleDebugOverlay();
            } else if (!modifierAndOverlaySame && Matches(overlayKey, e.glfwKey) && press) {
                Entries().ToggleDebugOverlay();
            }

            if (!press) continue;

            bool didDebugAction = false;
            const bool handlesGlobalInput = screen == nullptr || dynamic_cast<GameModeSwitcherScreen*>(screen) != nullptr;
            if (handlesGlobalInput && e.glfwKey == GLFW_KEY_ESCAPE && m_modifierDown) {
                if (m_cb.pauseWithoutMenu) m_cb.pauseWithoutMenu();
                didDebugAction = true;
            } else if (m_modifierDown && !Matches(modifier, e.glfwKey)) {
                didDebugAction = HandleDebugKeys(e.glfwKey, e.mods);
                if (didDebugAction) {
                    if (auto* options = dynamic_cast<DebugOptionsScreen*>(screen)) options->RefreshEntries();
                }
            }
            if (modifierAndOverlaySame) m_usedDebugKeyAsModifier |= didDebugAction;

            // Digits steer the profiler pie while it is up and F3 is not held.
            if (Overlay().ShowProfilerChart() && !m_modifierDown && e.glfwKey >= GLFW_KEY_0 && e.glfwKey <= GLFW_KEY_9) {
                Overlay().ProfilerKeyPress(e.glfwKey - GLFW_KEY_0);
            }

            if (didDebugAction) {
                // MC KeyMapping.set(key, false): the chord's key is not also
                // its gameplay action this press.
                CancelBoundKey(BoundKey::Keyboard(e.glfwKey));
                m_consumedThisFrame = true;
            }
        }
    }

    void DebugKeyHandler::Tick() {
        if (m_crashKeyTime <= 0) return;
        const int64_t now = NowMs();
        const int64_t remaining = 10000 - (now - m_crashKeyTime);
        const int64_t reported = now - m_crashKeyReportedTime;
        if (remaining < 0) {
            Log::Error("Manually triggered debug crash (F3+C held for 10 seconds)");
            std::abort();
        }
        if (reported >= 1000) {
            if (m_crashKeyReportedCount == 0) {
                Feedback(Fmt("%s + %s is held down. This will crash the game unless released.",
                             Input::Binds::DebugModifier ? Input::Binds::DebugModifier->key.DisplayName().c_str() : "F3",
                             Input::Binds::DebugCrash ? Input::Binds::DebugCrash->key.DisplayName().c_str() : "C"));
            } else {
                Warning(Fmt("Crashing in %lld...", static_cast<long long>((remaining + 999) / 1000)));
            }
            m_crashKeyReportedTime = now;
            ++m_crashKeyReportedCount;
        }
    }

} // namespace Render::DebugScreen
