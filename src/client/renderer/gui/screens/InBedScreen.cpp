// File: src/client/renderer/gui/screens/InBedScreen.cpp
#include "InBedScreen.hpp"

#include "../GuiGraphics.hpp"

namespace Render {

    namespace {
        bool s_inBedScreenOpen = false;
        bool s_wakeUpRequested = false;
    }

    InBedScreen::InBedScreen() : Screen("") {
        s_inBedScreenOpen = true;
    }

    InBedScreen::~InBedScreen() {
        s_inBedScreenOpen = false;
    }

    void InBedScreen::Init() {
        // MC InBedChatScreen.init: Button.builder(multiplayer.stopSleeping)
        //   .bounds(width / 2 - 100, height - 40, 200, 20) → sendWakeUp().
        AddWidget(new Button(m_width / 2 - 100, m_height - 40,
                             WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
                             "Leave Bed", [] { s_wakeUpRequested = true; }));
    }

    void InBedScreen::RenderBackground(GuiGraphics&, int, int, float) {
        // Nothing: the world shows through, darkened by the HUD's sleep fade.
    }

    void InBedScreen::OnClose() {
        // MC InBedChatScreen.onClose: `this.sendWakeUp()` — the request goes
        // out and the screen stays until the server has the player up
        // (Gui.tick pops it once isSleeping() is false).
        s_wakeUpRequested = true;
    }

    // ── Host-loop hooks ───────────────────────────────────────────────────

    void ShowInBedScreen() {
        if (s_inBedScreenOpen) return;
        s_wakeUpRequested = false;
        GetScreenManager().Push(std::make_unique<InBedScreen>());
        // Push is deferred to the next Update(); mark now so two ticks in a
        // row cannot double-push.
        s_inBedScreenOpen = true;
    }

    void DismissInBedScreen() {
        if (!s_inBedScreenOpen) return;
        if (dynamic_cast<InBedScreen*>(GetScreenManager().Current()) != nullptr) {
            GetScreenManager().Pop();
        }
        s_inBedScreenOpen = false;
    }

    bool IsInBedScreenOpen() {
        return s_inBedScreenOpen;
    }

    bool ConsumeWakeUpRequest() {
        const bool r = s_wakeUpRequested;
        s_wakeUpRequested = false;
        return r;
    }

} // namespace Render
