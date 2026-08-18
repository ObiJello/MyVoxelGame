// File: src/client/renderer/gui/screens/DisconnectedScreen.cpp
#include "DisconnectedScreen.hpp"

#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "Widgets.hpp"

namespace Render {

    namespace {
        constexpr uint32_t kWhite = 0xFFFFFFFFu;
        constexpr uint32_t kGrey  = 0xFFA0A0A0u;   // ChatFormatting.GRAY, for the reason

        // MC Font.lineHeight — the spacing MultiLineTextWidget uses per line.
        constexpr int kLineHeight = FontRenderer::LINE_HEIGHT;   // 9

        // MC: `.setMaxWidth(this.width - 50)` on the MultiLineTextWidget.
        constexpr int kReasonMargin = 50;

        // LinearLayout cell padding from DisconnectedScreen.init():
        //   defaultCellSetting().alignHorizontallyCenter().padding(10)  <- title, reason
        //   defaultCellSetting().padding(2)                             <- button
        // Padding is applied on every side of a cell, so a cell's contribution
        // to the column height is content + 2*padding, and the visible gap
        // between two cells is the sum of their facing paddings.
        constexpr int kTextPadding   = 10;
        constexpr int kButtonPadding = 2;

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
        // Wrapping needs font metrics, which live on GuiGraphics and are not
        // reachable here. Both the wrap and the layout that depends on its line
        // count are done on the first Render(); Init() just invalidates them so
        // a resize redoes both.
        m_reasonLines.clear();
        m_laidOut = false;

        // MC labels this button gui.toTitle -> "Back to Title Screen" whenever
        // the destination is the title screen (DisconnectedScreen.init's
        // non-multiplayer branch). Ours always is.
        m_backBtn = AddWidget(new Button(0, 0,
            WidgetDims::BUTTON_WIDTH, WidgetDims::BUTTON_HEIGHT,
            "Back to Title Screen", [this] {
                // MC sets the screen to `parent` (a TitleScreen). Ours is
                // already underneath, so pop back to it.
                if (m_manager) m_manager->Pop();
            }));
    }

    void DisconnectedScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        if (!m_laidOut) {
            m_reasonLines = WrapText(g, m_reason, m_width - kReasonMargin);

            // MC arranges title / reason / button in a vertical LinearLayout and
            // then FrameLayout.centerInRectangle's the WHOLE block in the screen
            // — so the group is centred, not pinned to fixed offsets. Reproduce
            // that: measure the column, then place from its top.
            const int reasonHeight = static_cast<int>(m_reasonLines.size()) * kLineHeight;
            const int titleCell  = kLineHeight   + kTextPadding   * 2;
            const int reasonCell = reasonHeight  + kTextPadding   * 2;
            const int buttonCell = WidgetDims::BUTTON_HEIGHT + kButtonPadding * 2;

            const int top = (m_height - (titleCell + reasonCell + buttonCell)) / 2;
            m_titleY  = top + kTextPadding;
            m_reasonY = top + titleCell + kTextPadding;

            if (m_backBtn) {
                m_backBtn->SetPosition(m_width / 2 - WidgetDims::BUTTON_WIDTH / 2,
                                       top + titleCell + reasonCell + kButtonPadding);
            }
            m_laidOut = true;
        }

        Screen::Render(g, mouseX, mouseY, partialTick);   // background + widgets

        g.DrawCenteredString(m_title, m_width / 2, m_titleY, kWhite);

        int y = m_reasonY;
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
