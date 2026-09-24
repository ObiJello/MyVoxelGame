// File: src/client/renderer/gui/screens/PauseScreen.cpp
#include "PauseScreen.hpp"
#include "OptionsScreens.hpp"
#include "FriendsScreen.hpp"
#include "WorldOptionsScreen.hpp"
#include "TitleScreen.hpp"   // TitleAction (quit signal to the host loop)
#include <GLFW/glfw3.h>

#include "../GuiGraphics.hpp"

namespace Render {

    void PauseScreen::Init() {
        // Vanilla game-menu layout: full-width rows are 200px, split rows are
        // two 98px halves with a 4px gutter, stacked at 24px pitch starting
        // just below the centered title.
        const int cx    = m_width / 2;
        const int fullX = cx - 100;
        int y = m_height / 4 + 32;

        AddWidget(new Button(fullX, y, WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Back to Game", [this] { OnClose(); }));
        y += 24;

        {
            // Vanilla's Advancements slot → Friends (see FriendsScreen).
            const bool loggedIn = Client::g_friendsClient != nullptr;
            Button* friendsBtn = AddWidget(new Button(fullX, y, 98,
                WidgetDims::BUTTON_HEIGHT, "Friends",
                loggedIn ? Button::OnPress([this] {
                    m_manager->Push(std::make_unique<FriendsScreen>());
                }) : Button::OnPress(nullptr)));
            if (!loggedIn) {
                friendsBtn->active = false;
                friendsBtn->SetTooltip({"Log in from the ObeyCraft",
                                        "launcher to use friends."});
            }
            Button* stats = AddWidget(new Button(cx + 2, y, 98, WidgetDims::BUTTON_HEIGHT,
                "Statistics", nullptr));
            stats->active = false;
            stats->SetTooltip({"Not available."});
        }
        y += 24;

        AddWidget(new Button(fullX, y, 98, WidgetDims::BUTTON_HEIGHT,
            "Options...", [this] {
                m_manager->Push(std::make_unique<OptionsScreen>());
            }));
        // MC 26.3: "World Options..." (options.worldOptions.button) took
        // the Open to LAN slot; LAN lives inside it as the Joinable switch.
        AddWidget(new Button(cx + 2, y, 98, WidgetDims::BUTTON_HEIGHT,
            "World Options...", [this] {
                m_manager->Push(std::make_unique<WorldOptionsScreen>());
            }));
        y += 24;

        AddWidget(new Button(fullX, y, WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Save and Quit to Title", [] {
                TitleAction a;
                a.kind = TitleAction::Kind::QuitToTitle;
                SetTitleAction(std::move(a));
            }));
    }

    void PauseScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, m_height / 4 + 8, 0xFFFFFFFF);
    }

} // namespace Render
