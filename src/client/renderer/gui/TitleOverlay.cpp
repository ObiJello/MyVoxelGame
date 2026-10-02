// File: src/client/renderer/gui/TitleOverlay.cpp
#include "TitleOverlay.hpp"
#include "GuiGraphics.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace Render {

    namespace {

        // One styled run of a component: its text with the font's style
        // codes in front (bold, italic, underline, strikethrough,
        // obfuscated — what Font draws from a Style), and its colour.
        struct Run {
            std::string text;
            uint32_t    rgb = 0xFFFFFF;
        };

        std::vector<Run> RunsOf(const Game::Text::Component& component) {
            std::vector<Run> runs;
            Game::Text::Visit(component, Game::Text::Style{},
                              [&runs](const Game::Text::Style& style, std::string_view text) {
                                  if (text.empty()) return true;
                                  std::string prefix;
                                  if (style.IsObfuscated())    prefix += "\xC2\xA7k";
                                  if (style.IsBold())          prefix += "\xC2\xA7l";
                                  if (style.IsStrikethrough()) prefix += "\xC2\xA7m";
                                  if (style.IsUnderlined())    prefix += "\xC2\xA7n";
                                  if (style.IsItalic())        prefix += "\xC2\xA7o";
                                  runs.push_back({prefix + std::string(text),
                                                  style.color ? style.color->rgb : 0xFFFFFFu});
                                  return true;
                              });
            return runs;
        }

        int WidthOf(GuiGraphics& g, const std::vector<Run>& runs) {
            int width = 0;
            for (const Run& run : runs) width += g.GetStringWidth(run.text);
            return width;
        }

        // MC GuiGraphics.textWithBackdrop(font, component, x, y, width,
        // color): the component's runs side by side, each in its own colour
        // with the caller's alpha (the backdrop itself is off by default —
        // see GuiGraphics::DrawStringWithBackdrop).
        void DrawRuns(GuiGraphics& g, const std::vector<Run>& runs, int x, int y, int width, uint32_t alpha) {
            int cursor = x;
            for (const Run& run : runs) {
                const int w = g.GetStringWidth(run.text);
                g.DrawStringWithBackdrop(run.text, cursor, y, w, (alpha << 24) | (run.rgb & 0xFFFFFFu));
                cursor += w;
            }
            (void)width;
        }

    } // namespace

    TitleOverlay& GetTitleOverlay() {
        static TitleOverlay overlay;
        return overlay;
    }

    void TitleOverlay::SetTitle(const Game::Text::Component& title) {
        m_title = title;
        m_titleTime = static_cast<float>(m_fadeIn + m_stay + m_fadeOut);
    }

    void TitleOverlay::SetSubtitle(const Game::Text::Component& subtitle) {
        m_subtitle = subtitle;
    }

    void TitleOverlay::SetTimes(int fadeIn, int stay, int fadeOut) {
        if (fadeIn >= 0)  m_fadeIn  = fadeIn;
        if (stay >= 0)    m_stay    = stay;
        if (fadeOut >= 0) m_fadeOut = fadeOut;
        if (m_titleTime > 0.0f) m_titleTime = static_cast<float>(m_fadeIn + m_stay + m_fadeOut);
    }

    void TitleOverlay::Clear(bool resetTimes) {
        m_title.reset();
        m_subtitle.reset();
        m_titleTime = 0.0f;
        if (resetTimes) {
            m_fadeIn  = kDefaultFadeIn;
            m_stay    = kDefaultStay;
            m_fadeOut = kDefaultFadeOut;
        }
    }

    void TitleOverlay::Render(GuiGraphics& graphics, float deltaSeconds) {
        // Gui.tick: `if (titleTime > 0 && --titleTime <= 0) { title =
        // subtitle = null; }`.
        if (m_titleTime > 0.0f) {
            m_titleTime -= deltaSeconds * 20.0f;
            if (m_titleTime <= 0.0f) {
                m_titleTime = 0.0f;
                m_title.reset();
                m_subtitle.reset();
                return;
            }
        }
        if (!m_title || m_titleTime <= 0.0f) return;

        // Hud.extractTitle: fading in over the first fadeIn ticks, out over
        // the last fadeOut.
        const float t = m_titleTime;
        int alpha = 255;
        if (t > static_cast<float>(m_fadeOut + m_stay)) {
            const float time = static_cast<float>(m_fadeIn + m_stay + m_fadeOut) - t;
            alpha = m_fadeIn > 0 ? static_cast<int>(time * 255.0f / static_cast<float>(m_fadeIn)) : 255;
        }
        if (t <= static_cast<float>(m_fadeOut)) {
            alpha = m_fadeOut > 0 ? static_cast<int>(t * 255.0f / static_cast<float>(m_fadeOut)) : 0;
        }
        alpha = std::clamp(alpha, 0, 255);
        if (alpha <= 0) return;
        const uint32_t a = static_cast<uint32_t>(alpha);

        graphics.NextStratum();
        graphics.PushMatrix();
        graphics.Translate(static_cast<float>(graphics.GuiWidth() / 2), static_cast<float>(graphics.GuiHeight() / 2));

        // The title at 4× above the centre line…
        graphics.PushMatrix();
        graphics.Scale(4.0f, 4.0f);
        const std::vector<Run> title = RunsOf(*m_title);
        const int titleWidth = WidthOf(graphics, title);
        DrawRuns(graphics, title, -titleWidth / 2, -10, titleWidth, a);
        graphics.PopMatrix();

        // …the subtitle at 2× below it.
        if (m_subtitle) {
            graphics.PushMatrix();
            graphics.Scale(2.0f, 2.0f);
            const std::vector<Run> subtitle = RunsOf(*m_subtitle);
            const int subtitleWidth = WidthOf(graphics, subtitle);
            DrawRuns(graphics, subtitle, -subtitleWidth / 2, 5, subtitleWidth, a);
            graphics.PopMatrix();
        }

        graphics.PopMatrix();
    }

} // namespace Render
