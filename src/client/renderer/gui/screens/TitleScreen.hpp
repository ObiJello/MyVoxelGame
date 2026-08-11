// File: src/client/renderer/gui/screens/TitleScreen.hpp
//
// The main menu — C++ counterpart of MC gui/screens/TitleScreen plus the
// direct-connect flavour of JoinMultiplayerScreen. Layout metrics mirror
// vanilla: logo at height/4-ish, 200×20 menu buttons spaced 24px starting
// at height/4+48, Options/Quit split row, version string bottom-left,
// copyright bottom-right, splash text pulsing at -20° beside the logo.
//
// The title flow talks back to PlatformMain through TitleAction: button
// handlers fill it in, the host loop polls ConsumeTitleAction() and leaves
// the menu phase when it's no longer None.
#pragma once

#include "Screen.hpp"
#include <cstdint>
#include <string>

namespace Render {

    // ── Title-phase outcome (polled by PlatformMain) ───────────────────────
    struct TitleAction {
        // QuitToTitle: pause-menu "Save and Quit to Title" — ends the current
        // world session and returns to the title screen (Quit exits the app).
        enum class Kind : uint8_t { None, Singleplayer, Multiplayer, Quit, QuitToTitle };
        Kind        kind = Kind::None;
        std::string host;          // Multiplayer only
        uint16_t    port = 25565;  // Multiplayer only

        // Singleplayer world selection (from SelectWorldScreen):
        //  useMinecraftSave=true  → legacy behavior: load the auto-detected
        //                           Anvil save from saves/world.
        //  useMinecraftSave=false → procedural world regenerated from `seed`
        //                           (created via CreateWorldScreen; only the
        //                           metadata persists at this stage).
        bool        useMinecraftSave = true;
        std::string worldName;
        int64_t     seed     = 0;
        int         gameMode = 1;   // 0 = Survival, 1 = Creative
        long long   dayTime  = 6000;          // world time restored from worlds.json (6000 = noon)
        bool        doDaylightCycle = false;  // gamerule restored from worlds.json
        std::string skybox = "vanilla";       // per-world sky (see WorldEntry::skybox)
        int         skyboxMode = 2;           // 0 static, 1 darken, 2 darken+celestials

        // Relayed join (friends service): host/port above point at the
        // friends service rather than the game host, and this ticket is
        // presented before the game handshake so the relay can pair us with
        // the host's outbound tunnel. Empty for direct connections.
        std::string relayTicket;
    };
    void        SetTitleAction(TitleAction action);
    TitleAction ConsumeTitleAction();

    class TitleScreen : public Screen {
    public:
        // fadeIn: play the 2-second widget fade (used on boot, like MC's
        // post-loading fade).
        explicit TitleScreen(bool fadeIn = true);

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void RenderBackground(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool ShouldCloseOnEsc() const override { return false; }

    private:
        void RenderLogo(GuiGraphics& g, float alpha);
        void RenderSplash(GuiGraphics& g, float alpha);
        float CurrentFadeAlpha() const;

        bool        m_fading;
        double      m_fadeStartSeconds = -1.0;
        bool        m_easterEggLogo = false;   // "Minceraft" (1-in-10000)
        bool        m_logoResolved  = false;
        std::string m_splash;
    };

    // ── Direct Connect (MC DirectJoinServerScreen) ─────────────────────────
    class JoinMultiplayerScreen : public Screen {
    public:
        JoinMultiplayerScreen();
        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool KeyPressed(int glfwKey, int glfwMods) override;   // Enter joins

    private:
        void Join();
        EditBox* m_addressBox = nullptr;
        Button*  m_joinButton = nullptr;
        // Persisted last-used address (settings key "lastServerAddress").
        std::string m_initialAddress;
    };

} // namespace Render
