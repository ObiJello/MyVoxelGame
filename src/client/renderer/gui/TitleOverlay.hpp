// File: src/client/renderer/gui/TitleOverlay.hpp
//
// The title and subtitle in the middle of the screen — MC Hud's title half
// (fields title / subtitle / titleTime / titleFadeInTime / titleStayTime /
// titleFadeOutTime, setTitle / setSubtitle / setTimes / clearTitles /
// resetTitleTimes, and extractTitle). /title drives it through TitlesS2C.
//
// Times are ticks, as in MC. MC counts titleTime down in Gui.tick and
// subtracts the partial tick when drawing; here the count is a float that
// Render decrements by the frame's delta at 20 ticks a second, which is the
// same curve sampled per frame.
#pragma once

#include "common/text/TextComponent.hpp"

#include <optional>

namespace Render {

    class GuiGraphics;

    class TitleOverlay {
    public:
        // MC Hud.resetTitleTimes: 10 / 70 / 20 ticks.
        static constexpr int kDefaultFadeIn  = 10;
        static constexpr int kDefaultStay    = 70;
        static constexpr int kDefaultFadeOut = 20;

        // MC Hud.setTitle: shows the title and restarts the timer.
        void SetTitle(const Game::Text::Component& title);
        // MC Hud.setSubtitle: replaces the subtitle; it shows with the next
        // (or current) title.
        void SetSubtitle(const Game::Text::Component& subtitle);
        // MC Hud.setTimes: a negative value keeps the current one; a title
        // on screen restarts with the new total.
        void SetTimes(int fadeIn, int stay, int fadeOut);
        // MC Hud.clearTitles (+ resetTitleTimes when `resetTimes`).
        void Clear(bool resetTimes);

        // MC Hud.extractTitle, plus the countdown Gui.tick runs. Called
        // from the HUD every frame, above the action bar.
        void Render(GuiGraphics& graphics, float deltaSeconds);

    private:
        std::optional<Game::Text::Component> m_title;
        std::optional<Game::Text::Component> m_subtitle;
        float m_titleTime = 0.0f;   // ticks left
        int   m_fadeIn  = kDefaultFadeIn;
        int   m_stay    = kDefaultStay;
        int   m_fadeOut = kDefaultFadeOut;
    };

    // The one overlay the HUD draws and TitlesS2C writes.
    TitleOverlay& GetTitleOverlay();

} // namespace Render
