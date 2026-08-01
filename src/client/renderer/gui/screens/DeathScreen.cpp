// File: src/client/renderer/gui/screens/DeathScreen.cpp
#include "DeathScreen.hpp"
#include "TitleScreen.hpp"   // TitleAction (quit-to-title signal)

#include "../GuiGraphics.hpp"

namespace Render {

    namespace {
        // MC DeathScreen.renderDeathBackground: fillGradient(0, 0, w, h,
        // 1615855616, -1602211792) → ARGB 0x60500000 → 0xA0803030.
        constexpr uint32_t kGradientTop    = 0x60500000u;
        constexpr uint32_t kGradientBottom = 0xA0803030u;
        constexpr uint32_t kWhite  = 0xFFFFFFFFu;
        constexpr uint32_t kYellow = 0xFFFFFF55u;  // ChatFormatting.YELLOW

        bool s_deathScreenOpen  = false;
        bool s_respawnRequested = false;
    }

    DeathScreen::DeathScreen() : Screen("You Died!") {
        s_deathScreenOpen = true;
    }

    DeathScreen::~DeathScreen() {
        s_deathScreenOpen = false;
    }

    void DeathScreen::Init() {
        // MC DeathScreen.init: Respawn at (w/2-100, h/4+72), Title Screen at
        // (w/2-100, h/4+96), both 200×20, disabled until delayTicker == 20.
        const int x = m_width / 2 - 100;

        m_respawnBtn = AddWidget(new Button(x, m_height / 4 + 72,
            WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Respawn", [this] {
                // MC disables the button on click so it can't double-fire.
                if (m_respawnBtn) m_respawnBtn->active = false;
                s_respawnRequested = true;
            }));

        m_titleBtn = AddWidget(new Button(x, m_height / 4 + 96,
            WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Title Screen", [] {
                TitleAction a;
                a.kind = TitleAction::Kind::QuitToTitle;
                SetTitleAction(std::move(a));
            }));

        // Init() reruns on resize — keep the current delay gating.
        SetButtonsActive(m_delayTicker >= 20);
    }

    void DeathScreen::Tick() {
        // MC DeathScreen.tick: `if (delayTicker++ == 20) setButtonsActive(true)`.
        ++m_delayTicker;
        if (m_delayTicker == 20) {
            SetButtonsActive(true);
        }
    }

    void DeathScreen::SetButtonsActive(bool active) {
        if (m_respawnBtn) m_respawnBtn->active = active;
        if (m_titleBtn)   m_titleBtn->active   = active;
    }

    void DeathScreen::RenderBackground(GuiGraphics& g, int, int, float) {
        g.FillGradient(0, 0, m_width, m_height, kGradientTop, kGradientBottom);
    }

    void DeathScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);  // background + widgets

        // Title at 2× scale — MC: poseStack.scale(2,2) then drawCenteredString
        // at (width/2/2, 30), i.e. screen-space (width/2, 60).
        g.PushMatrix();
        g.Scale(2.0f, 2.0f);
        g.DrawCenteredString(m_title, m_width / 4, 30, kWhite);
        g.PopMatrix();

        // Score line at y=100 — "Score: <value>" with the value in yellow
        // (no XP system yet, so the value is always 0, same as a fresh MC
        // death without levels).
        const std::string label = "Score: ";
        const std::string value = "0";
        const int totalW = g.GetStringWidth(label + value);
        const int x0 = m_width / 2 - totalW / 2;
        g.DrawString(label, x0, 100, kWhite);
        g.DrawString(value, x0 + g.GetStringWidth(label), 100, kYellow);
    }

    // ── Host-loop / packet-handler hooks ──────────────────────────────────

    void ShowDeathScreen() {
        if (s_deathScreenOpen) return;
        s_respawnRequested = false;
        GetScreenManager().Push(std::make_unique<DeathScreen>());
        // Push is deferred to the next Update(); mark now so a second
        // SetHealth(0) in the same tick can't double-push.
        s_deathScreenOpen = true;
    }

    void DismissDeathScreen() {
        if (!s_deathScreenOpen) return;
        // The death screen never has sub-screens above it (ESC is inert and
        // the pause menu only opens when the stack is empty), so a Pop of the
        // current screen is exactly "close the death screen".
        if (dynamic_cast<DeathScreen*>(GetScreenManager().Current()) != nullptr) {
            GetScreenManager().Pop();
        }
        s_deathScreenOpen = false;
    }

    bool ConsumeDeathRespawnRequest() {
        const bool r = s_respawnRequested;
        s_respawnRequested = false;
        return r;
    }

} // namespace Render
