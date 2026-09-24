// File: src/client/renderer/gui/debug/DebugKeyHandler.hpp
//
// The F3 chords — MC 26.3's KeyboardHandler.keyPress / handleDebugKeys /
// tick, on this engine's raw key stream (Input::PopRawKeyEvent):
//
//   F3 (tap)     toggle the overlay (only if no chord fired while held)
//   F3+A         reload chunks           F3+B  hitboxes        F3+C  copy location (hold 10 s: crash)
//   F3+D         clear chat              F3+G  chunk borders   F3+H  advanced tooltips
//   F3+I         copy /setblock or /summon for the target      F3+N  spectator <-> previous mode
//   F3+F4        game-mode switcher      F3+F6 debug options   F3+P  pause on lost focus
//   F3+S         dump dynamic textures   F3+T  reload packs    F3+L  start/stop profiling
//   F3+V         version info            F3+1/2/3/4 charts     F3+X  improved transparency
//   F3+Esc       pause without the pause menu
//
// Every key that fired a chord is cancelled as a gameplay binding for that
// press (MC KeyMapping.set(key,false)), and F3's own release does not
// toggle the overlay when it was used as a modifier.
//
// The pieces that live in PlatformMain (chat, resource reload, the
// profiler recorder, pausing) come in through DebugKeyCallbacks.
#pragma once

#include "../screens/Screen.hpp"
#include "DebugScreenEntries.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace Input { struct KeyMapping; }

namespace Render::DebugScreen {

    struct DebugKeyCallbacks {
        std::function<void()> reloadChunks;
        std::function<void()> reloadResourcePacks;
        std::function<void()> clearChat;
        // A client-local chat line (MC Minecraft.showDebugChat); the text
        // carries § formatting.
        std::function<void(const std::string&)> showChat;
        // A client-local chat line whose tail is an UNDERLINED, clickable
        // path (MC ClickEvent.OpenFile — the OS reveals it). `linkText` is
        // what the line shows (MC prints the path relative to the game
        // directory), `openPath` what a click opens.
        std::function<void(const std::string& before, const std::string& linkText,
                           const std::string& openPath)> showChatFileLink;
        // F3+Esc: pause the world without opening the pause menu.
        std::function<void()> pauseWithoutMenu;
        // F3+S: write the atlases to disk. Fills the folder as shown to the
        // player (relative to the game directory) and as an absolute path
        // for the click; false on failure.
        std::function<bool(std::string& displayPath, std::string& absolutePath)> dumpDynamicTextures;
        // F3+L: start (true) or stop (false) the 10-second frame profile.
        std::function<bool()> toggleProfiling;
        // A chat command, sent to the server ("/gamemode spectator").
        std::function<void(const std::string&)> sendCommand;
        // Whether a level and a connection exist (MC canSwitchGameMode).
        std::function<bool()> hasLevel;
    };

    class DebugKeyHandler {
    public:
        void SetCallbacks(DebugKeyCallbacks callbacks) { m_cb = std::move(callbacks); }

        // Drain and act on this frame's raw key events.
        void ProcessFrame();
        // Once per client tick (50 ms): the F3+C crash countdown.
        void Tick();

        // The player's current game mode byte, so F3+N / F3+F4 know what
        // "previous" is (MC MultiPlayerGameMode.previousPlayerMode).
        void NotifyGameMode(int mode);
        int  PreviousGameMode() const { return m_previousGameMode; }

        bool IsDebugModifierDown() const { return m_modifierDown; }
        // Tells the world pass the frame limiter that a key was hit.
        bool ConsumedAnyThisFrame() const { return m_consumedThisFrame; }

        // MC's yellow "[Debug]: ..." line.
        void Feedback(const std::string& message);
        // "[Debug]: <before><linkText>" with the link clickable (OpenFile).
        void FeedbackWithFile(const std::string& before, const std::string& linkText,
                              const std::string& openPath);
        void Warning(const std::string& message);

    private:
        bool HandleDebugKeys(int glfwKey, int mods);
        bool Matches(const Input::KeyMapping* mapping, int glfwKey) const;
        void CopyRecreateCommand(bool addNbt, bool pullFromServer);
        void DumpVersion();
        void SetClipboard(const std::string& text);

        DebugKeyCallbacks m_cb;
        bool m_modifierDown = false;
        bool m_usedDebugKeyAsModifier = false;
        bool m_consumedThisFrame = false;
        int  m_currentGameMode = -1;
        int  m_previousGameMode = -1;
        // MC debugCrashKeyTime & friends, in milliseconds of steady time.
        int64_t m_crashKeyTime = -1;
        int64_t m_crashKeyReportedTime = -1;
        int64_t m_crashKeyReportedCount = -1;
    };

    DebugKeyHandler& KeyHandler();

    // The frame's Context (player, camera, controller) for the chords that
    // read the world — set by PlatformMain before ProcessFrame.
    void SetDebugContext(const Context* ctx);

    // F3+Esc: MC's "pause without pause menu" — PauseScreen(false): a screen
    // that pauses the world (IsPauseScreen), draws no background and no
    // buttons, only its title "Game Paused" (menu.paused) as a StringWidget
    // at y = 10. ESC closes it.
    class SilentPauseScreen : public Screen {
    public:
        SilentPauseScreen() : Screen("Game Paused") {}
        void Init() override {}
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void RenderBackground(GuiGraphics&, int, int, float) override {}
    };

} // namespace Render::DebugScreen
