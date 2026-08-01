// File: src/client/renderer/gui/screens/DeathScreen.hpp
//
// The "You Died!" screen — C++ counterpart of MC's DeathScreen.java:
//   • red fillGradient background over the live world
//   • title at 2× scale, score line, Respawn + Title Screen buttons
//   • both buttons disabled for the first 20 ticks (1 s)
//   • ESC does nothing (shouldCloseOnEsc = false)
//
// Opened by ClientPacketHandler::handleSetHealth when the server-synced
// health hits 0 (that IS MC's death signal — no separate packet), and popped
// when a respawn brings health back above 0.
#pragma once

#include "Screen.hpp"

namespace Render {

    class DeathScreen : public Screen {
    public:
        DeathScreen();
        ~DeathScreen() override;

        void Init() override;
        void Tick() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void RenderBackground(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool ShouldCloseOnEsc() const override { return false; }

    private:
        void SetButtonsActive(bool active);

        Button* m_respawnBtn = nullptr;
        Button* m_titleBtn   = nullptr;
        int     m_delayTicker = 0;   // MC: buttons enable when this reaches 20
    };

    // ── Host-loop / packet-handler hooks ──────────────────────────────────
    // Push the death screen if not already shown (idempotent).
    void ShowDeathScreen();
    // Pop it if it is the current screen (health went back above 0).
    void DismissDeathScreen();
    // True once when the Respawn button was pressed — PlatformMain drains
    // this and sends PlayerAction::PERFORM_RESPAWN.
    bool ConsumeDeathRespawnRequest();

} // namespace Render
