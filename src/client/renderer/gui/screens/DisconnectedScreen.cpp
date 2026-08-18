// File: src/client/renderer/gui/screens/DisconnectedScreen.cpp
#include "DisconnectedScreen.hpp"

#include "../GuiGraphics.hpp"
#include "Widgets.hpp"

namespace Render {

    namespace {
        constexpr uint32_t kWhite = 0xFFFFFFFFu;
        constexpr uint32_t kGrey  = 0xFFA0A0A0u;   // ChatFormatting.GRAY, for the reason

        // MC font line height (9px glyphs + 1px spacing).
        constexpr int kLineHeight = 10;

        // MC: `.setMaxWidth(this.width - 50)` on the MultiLineTextWidget.
        constexpr int kReasonMargin = 50;

        // Set during teardown, drained by the title phase. Plain string, not
        // atomic: both ends run on the main thread — the network callback that
        // originates the reason hands it over via PlatformMain, which is what
        // owns the cross-thread part.
        std::string s_pendingReason;
        bool        s_hasPendingReason = false;

        // Greedy word wrap against the real measured width. Stand-in for MC's
        // MultiLineTextWidget, which wraps through the same font metrics.
        std::vector<std::string> WrapText(GuiGraphics& g, const std::string& text, int maxWidth) {
            std::vector<std::string> lines;
            if (text.empty() || maxWidth <= 0) {
                return lines;
            }

            std::string current;
            std::string word;
            auto flushWord = [&](bool hardBreak) {
                if (!word.empty()) {
                    const std::string candidate = current.empty() ? word : current + " " + word;
                    if (g.GetStringWidth(candidate) <= maxWidth || current.empty()) {
                        current = candidate;
                    } else {
                        lines.push_back(current);
                        current = word;
                    }
                    word.clear();
                }
                if (hardBreak) {
                    lines.push_back(current);
                    current.clear();
                }
            };

            for (char c : text) {
                if (c == '\n')      flushWord(true);
                else if (c == ' ')  flushWord(false);
                else                word.push_back(c);
            }
            flushWord(false);
            if (!current.empty()) lines.push_back(current);

            return lines;
        }
    }

    DisconnectedScreen::DisconnectedScreen(std::string reason)
        : Screen("Connection Lost")
        , m_reason(std::move(reason)) {
        if (m_reason.empty()) {
            // MC always has a reason Component; ours can come from a bare
            // socket error, so give the player something rather than a blank.
            m_reason = "Connection to the server was lost.";
        }
    }

    void DisconnectedScreen::Init() {
        // Wrapping needs font metrics, which live on GuiGraphics — not
        // available here. Defer to the first Render(), keyed on the line list
        // being empty. Init() clears it so a resize re-wraps.
        m_reasonLines.clear();

        // MC lays the whole thing out with a centred LinearLayout. Ours is a
        // fixed layout around the vertical centre: title, reason block, then
        // the button below it.
        const int x = m_width / 2 - WidgetDims::BUTTON_WIDTH / 2;
        AddWidget(new Button(x, m_height / 2 + 40,
            WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Back to Title", [this] {
                // MC sets the screen to `parent` (a TitleScreen). Ours is
                // already underneath, so pop back to it.
                if (m_manager) m_manager->Pop();
            }));
    }

    void DisconnectedScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        if (m_reasonLines.empty()) {
            m_reasonLines = WrapText(g, m_reason, m_width - kReasonMargin);
        }

        Screen::Render(g, mouseX, mouseY, partialTick);   // background + widgets

        // Title, then the reason block centred above the button.
        const int titleY = m_height / 2 - 40;
        g.DrawCenteredString(m_title, m_width / 2, titleY, kWhite);

        int y = titleY + kLineHeight * 2;
        for (const auto& line : m_reasonLines) {
            g.DrawCenteredString(line, m_width / 2, y, kGrey);
            y += kLineHeight;
        }
    }

    // ── PlatformMain hooks ────────────────────────────────────────────────

    void SetPendingDisconnectReason(std::string reason) {
        s_pendingReason    = std::move(reason);
        s_hasPendingReason = true;
    }

    void ShowPendingDisconnectScreen() {
        if (!s_hasPendingReason) return;
        s_hasPendingReason = false;
        GetScreenManager().Push(std::make_unique<DisconnectedScreen>(std::move(s_pendingReason)));
        s_pendingReason.clear();
    }

} // namespace Render
