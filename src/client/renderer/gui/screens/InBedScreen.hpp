// File: src/client/renderer/gui/screens/InBedScreen.hpp
//
// The screen you get while lying in a bed — C++ counterpart of MC's
// InBedChatScreen, minus the chat line (chat is its own overlay here):
//   • one "Leave Bed" button at (w/2-100, h-40)
//   • no background of its own — the darkening is the HUD's sleep fade
//     (HudRenderer::RenderSleepOverlay), exactly as in vanilla
//   • ESC does not close it: MC's onClose sends the wake-up request and
//     the screen goes away when the server confirms the player is up
//   • not a pause screen — the sleep clock has to run underneath it
//
// Opened by the client tick when the local player is sleeping and no other
// screen is up (MC Gui.tick), popped when the player is no longer sleeping.
#pragma once

#include "Screen.hpp"

namespace Render {

    class InBedScreen : public Screen {
    public:
        InBedScreen();
        ~InBedScreen() override;

        void Init() override;
        void RenderBackground(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool IsPauseScreen() const override { return false; }
        // ESC → OnClose → the wake-up request (MC InBedChatScreen.onClose).
        bool ShouldCloseOnEsc() const override { return true; }
        void OnClose() override;
    };

    // ── Host-loop hooks ───────────────────────────────────────────────────
    // Push the screen if not already shown (idempotent).
    void ShowInBedScreen();
    // True while the screen is up (pushed or pending). PlatformMain routes
    // text input to the chat rather than the screen while this holds.
    bool IsInBedScreenOpen();
    // Pop it if it is the current screen (the player got up).
    void DismissInBedScreen();
    // True once when "Leave Bed" (or ESC) was pressed — PlatformMain drains
    // this and sends PlayerAction::STOP_SLEEPING.
    bool ConsumeWakeUpRequest();

} // namespace Render
