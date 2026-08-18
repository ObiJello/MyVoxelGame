// File: src/client/renderer/gui/screens/DisconnectedScreen.hpp
//
// The "Connection Lost" screen — C++ counterpart of MC's
// DisconnectedScreen.java:
//   • standard opaque menu background (MC uses Screen's default here)
//   • title, then the disconnect reason as centred, wrapped text
//     (MultiLineTextWidget with maxWidth = width - 50)
//   • one 200x20 button back to the parent screen
//   • ESC does nothing (shouldCloseOnEsc = false)
//
// MC reaches this from ClientCommonPacketListenerImpl.onDisconnect (:348):
//   this.minecraft.disconnect(this.createDisconnectScreen(details), ...)
// and createDisconnectScreen (:368) uses `new TitleScreen()` as the parent
// when there is no server list to go back to. Ours is pushed ON TOP of the
// title screen, so the button is a plain pop — same destination, and the
// title screen keeps its own state.
#pragma once

#include "Screen.hpp"
#include <string>
#include <vector>

namespace Render {

    class DisconnectedScreen : public Screen {
    public:
        explicit DisconnectedScreen(std::string reason);

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        // MC DisconnectedScreen.shouldCloseOnEsc() -> false. The player has to
        // acknowledge with the button; ESC would otherwise drop them onto the
        // title screen with no idea what happened.
        bool ShouldCloseOnEsc() const override { return false; }

    private:
        std::string              m_reason;
        std::vector<std::string> m_reasonLines;   // wrapped on first Render()
        class Button*            m_backBtn = nullptr;
        // Layout depends on the wrapped line count, which needs font metrics —
        // so it happens on the first Render() rather than in Init().
        bool m_laidOut = false;
        int  m_titleY  = 0;
        int  m_reasonY = 0;
    };

    // ── PlatformMain hooks ────────────────────────────────────────────────
    // Record why the session ended. Called during session teardown, from the
    // main thread. The screen itself cannot be pushed at that point — the
    // title screen it belongs on top of does not exist yet.
    void SetPendingDisconnectReason(std::string reason);
    // Drained by the title phase once the title screen is up: pushes the
    // screen if a reason is pending. No-op otherwise.
    void ShowPendingDisconnectScreen();

} // namespace Render
