// File: src/client/renderer/gui/ChatComponent.cpp
#include "ChatComponent.hpp"
#include "GuiGraphics.hpp"
#include "FontRenderer.hpp"
#include <algorithm>
#include <cmath>

namespace Render {

    namespace {
        std::function<void(const std::string&)> g_clipboardHandler;
    }

    void SetClipboardHandler(std::function<void(const std::string&)> handler) {
        g_clipboardHandler = std::move(handler);
    }

    void CopyToClipboard(const std::string& text) {
        if (g_clipboardHandler) g_clipboardHandler(text);
    }

    void ChatComponent::Update(float deltaTime) {
        m_gameTime += deltaTime;
    }

    // Split a message into multiple lines — MC's StringSplitter.LineBreakFinder algorithm:
    // Walk character by character, accumulate width. When width > maxWidth,
    // break at last space. If no space found, break at current char (mid-word).
    //
    // Returns [start, end) character ranges rather than substrings, because the
    // caller needs to map each character back to the styled segment it came
    // from in order to colour runs and place click rectangles.
    static std::vector<std::pair<int, int>> SplitLineRanges(const std::string& text, int maxWidth,
                                                            const FontRenderer* font) {
        std::vector<std::pair<int, int>> lines;
        const int size = static_cast<int>(text.size());
        if (!font || maxWidth <= 0 || text.empty()) {
            lines.emplace_back(0, size);
            return lines;
        }

        int start = 0;
        while (start < size) {
            float width = 0.0f;
            int lastSpace = -1;
            int pos = start;

            for (; pos < size; pos++) {
                char c = text[pos];
                if (c == ' ') lastSpace = pos;

                int glyphW = font->GetCharWidth(static_cast<unsigned char>(c));
                width += static_cast<float>(glyphW + 1);

                if (width > static_cast<float>(maxWidth) && pos > start) {
                    int breakPos = (lastSpace > start) ? lastSpace : pos;
                    lines.emplace_back(start, breakPos);
                    start = (lastSpace > start && breakPos == lastSpace) ? breakPos + 1 : breakPos;
                    break;
                }
            }

            if (pos >= size) {
                lines.emplace_back(start, size);
                break;
            }
        }

        if (lines.empty()) lines.emplace_back(0, 0);
        return lines;
    }

    void ChatComponent::AddMessage(const std::string& text, uint32_t color) {
        ChatSegment seg;
        seg.text = text;
        seg.color = color;
        AddMessage(std::vector<ChatSegment>{std::move(seg)});
    }

    void ChatComponent::AddMessage(std::vector<ChatSegment> segments) {
        ChatMessage msg;
        msg.segments = std::move(segments);
        msg.addedTime = m_gameTime;
        m_messages.push_back(std::move(msg));

        // Trim to max
        if (static_cast<int>(m_messages.size()) > MAX_MESSAGES) {
            m_messages.erase(m_messages.begin());
        }
    }

    void ChatComponent::Render(GuiGraphics& graphics, float gameTime, bool chatOpen) {
        int guiHeight = graphics.GuiHeight();

        // MC: chatBottom = floor((screenHeight - 40) / scale), scale defaults to 1.0
        int baseY = guiHeight - CHAT_BOTTOM_MARGIN;

        // MC: maxWidth = ceil(chatWidth / scale), chatWidth = floor(pct * 280 + 40) = 320 at default
        int maxWidth = CHAT_WIDTH;

        // MC: background extends from -4 to maxWidth + 4 + 4 (with 4px translate)
        // In screen coords: from 0 to maxWidth + 4 + 4 + 4 = maxWidth + 12
        int bgLeft = 0;
        int bgRight = MESSAGE_INDENT + maxWidth + MESSAGE_INDENT + MESSAGE_INDENT;

        int maxLines = chatOpen ? MAX_MESSAGES : MAX_VISIBLE_LINES;
        int linesRendered = 0;

        const FontRenderer* font = graphics.GetFontRenderer();
        m_clickRegions.clear();

        // The hover tooltip is drawn after every line so it isn't painted over
        // by a later line's background.
        std::string pendingTooltip;

        // Render bottom-up (newest at bottom)
        for (int i = static_cast<int>(m_messages.size()) - 1; i >= 0 && linesRendered < maxLines; i--) {
            const auto& msg = m_messages[i];

            // Calculate fade alpha
            float age = gameTime - msg.addedTime;
            float alpha = 1.0f;

            if (!chatOpen) {
                if (age > FADE_DURATION + FADE_TIME) {
                    continue; // Fully faded
                }
                if (age > FADE_DURATION) {
                    // MC: t = 1 - (age/200ticks); alpha = clamp(t*10, 0, 1)^2
                    float t = 1.0f - (age - FADE_DURATION) / FADE_TIME;
                    t = std::clamp(t, 0.0f, 1.0f);
                    alpha = t * t;
                }
            }

            if (alpha <= 0.01f) continue;

            // Flatten the styled runs into one string for wrapping, keeping a
            // per-character map back to the segment it came from. Wrapping has
            // to see the whole message at once — a segment boundary is not a
            // line boundary.
            std::string full;
            std::vector<int> segOf;
            for (int s = 0; s < static_cast<int>(msg.segments.size()); ++s) {
                full += msg.segments[s].text;
                segOf.resize(full.size(), s);
            }

            auto ranges = SplitLineRanges(full, maxWidth, font);

            // Render wrapped lines bottom-up (last line of the wrap at the bottom)
            for (int li = static_cast<int>(ranges.size()) - 1; li >= 0 && linesRendered < maxLines; li--) {
                int lineY = baseY - (linesRendered + 1) * LINE_HEIGHT;

                // Background
                uint8_t bgAlpha = static_cast<uint8_t>(alpha * 128.0f);
                uint32_t bgColor = (static_cast<uint32_t>(bgAlpha) << 24);
                graphics.Fill(bgLeft, lineY, bgRight, lineY + LINE_HEIGHT, bgColor);

                const uint8_t textAlpha = static_cast<uint8_t>(alpha * 255.0f);
                const auto [lineStart, lineEnd] = ranges[li];

                // Draw the line one styled run at a time, advancing x by the
                // measured width of each run so runs stay flush.
                int x = MESSAGE_INDENT;
                int runStart = lineStart;
                while (runStart < lineEnd) {
                    const int segIdx = segOf.empty() ? 0 : segOf[runStart];
                    int runEnd = runStart;
                    while (runEnd < lineEnd && (segOf.empty() ? 0 : segOf[runEnd]) == segIdx) {
                        runEnd++;
                    }

                    const ChatSegment& seg = msg.segments[segIdx];
                    const std::string runText = full.substr(runStart, runEnd - runStart);
                    const int runWidth = font ? font->GetStringWidth(runText) : 0;

                    uint32_t textColor = (static_cast<uint32_t>(textAlpha) << 24) |
                                         (seg.color & 0x00FFFFFF);

                    const bool hovered =
                        chatOpen && seg.click != ChatClickAction::None &&
                        m_mouseX >= x && m_mouseX < x + runWidth &&
                        m_mouseY >= lineY && m_mouseY < lineY + LINE_HEIGHT;

                    // MC draws no underline or highlight on a hovered chat
                    // component — the hover tooltip is the only feedback. The
                    // cursor change is driven separately, off
                    // IsHoveringClickable().
                    if (hovered && !seg.hoverText.empty()) {
                        pendingTooltip = seg.hoverText;
                    }

                    graphics.DrawString(runText, x, lineY + 1, textColor, true);

                    if (chatOpen && seg.click != ChatClickAction::None) {
                        m_clickRegions.push_back(ClickRegion{
                            x, lineY, x + runWidth, lineY + LINE_HEIGHT,
                            seg.click, seg.clickValue, seg.hoverText});
                    }

                    // +1 for the inter-character gap. FontRenderer accumulates
                    // (glyphWidth + 1) per character and then subtracts the
                    // trailing 1, so measuring runs separately drops one pixel
                    // at every run boundary — the brackets ended up hugging the
                    // seed instead of sitting where a single DrawString of the
                    // whole line would have put them.
                    x += runWidth + 1;
                    runStart = runEnd;
                }

                linesRendered++;
            }
        }

        if (!pendingTooltip.empty() && font) {
            // Own stratum. Within one stratum GuiRenderer draws ALL fills before
            // ALL text (the sort is by zOrder, and fills are pushed into the
            // command list ahead of texts), so a tooltip background submitted
            // after the chat lines would still end up UNDER their text.
            graphics.NextStratum();

            // Same frame MC's Screen.renderTooltip draws (and the same one
            // AbstractContainerScreen::RenderTooltip already uses here): a 0xF0100010
            // body inset on all four sides plus the 0x505000FF violet border
            // down the left and right edges.
            const int tw = font->GetStringWidth(pendingTooltip);
            const int th = LINE_HEIGHT - 1;
            int tx = m_mouseX + 12;
            int ty = m_mouseY - 12;
            if (tx + tw + 4 > graphics.GuiWidth()) tx = graphics.GuiWidth() - tw - 4;
            if (ty < 4) ty = 4;

            const uint32_t bg     = 0xF0100010;
            const uint32_t border = 0x505000FF;
            graphics.Fill(tx - 3, ty - 4,      tx + tw + 3, ty - 3,      bg);
            graphics.Fill(tx - 3, ty + th + 3, tx + tw + 3, ty + th + 4, bg);
            graphics.Fill(tx - 3, ty - 3,      tx + tw + 3, ty + th + 3, bg);
            graphics.Fill(tx - 4, ty - 3,      tx - 3,      ty + th + 3, bg);
            graphics.Fill(tx + tw + 3, ty - 3, tx + tw + 4, ty + th + 3, bg);
            graphics.Fill(tx - 3,      ty - 2, tx - 2,      ty + th + 2, border);
            graphics.Fill(tx + tw + 2, ty - 2, tx + tw + 3, ty + th + 2, border);

            // And again so the label lands above its own background.
            graphics.NextStratum();
            graphics.DrawString(pendingTooltip, tx, ty, 0xFFFFFFFF, true);
        }
    }

    bool ChatComponent::IsHoveringClickable() const {
        for (const auto& r : m_clickRegions) {
            if (m_mouseX >= r.x0 && m_mouseX < r.x1 &&
                m_mouseY >= r.y0 && m_mouseY < r.y1) {
                return true;
            }
        }
        return false;
    }

    bool ChatComponent::HandleClick() {
        for (const auto& r : m_clickRegions) {
            if (m_mouseX < r.x0 || m_mouseX >= r.x1) continue;
            if (m_mouseY < r.y0 || m_mouseY >= r.y1) continue;
            if (r.action == ChatClickAction::CopyToClipboard) {
                CopyToClipboard(r.value);
                return true;
            }
        }
        return false;
    }

    void ChatComponent::Clear() {
        m_messages.clear();
        m_clickRegions.clear();
    }

} // namespace Render
