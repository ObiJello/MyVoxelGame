// File: src/client/renderer/gui/ChatComponent.cpp
#include "ChatComponent.hpp"
#include "GuiGraphics.hpp"
#include "FontRenderer.hpp"
#include <algorithm>
#include <cmath>

namespace Render {

    void ChatComponent::Update(float deltaTime) {
        m_gameTime += deltaTime;
    }

    // Split a message into multiple lines — MC's StringSplitter.LineBreakFinder algorithm:
    // Walk character by character, accumulate width. When width > maxWidth,
    // break at last space. If no space found, break at current char (mid-word).
    static std::vector<std::string> SplitLines(const std::string& text, int maxWidth,
                                                const FontRenderer* font) {
        std::vector<std::string> lines;
        if (!font || maxWidth <= 0 || text.empty()) {
            lines.push_back(text);
            return lines;
        }

        int start = 0;
        int size = static_cast<int>(text.size());

        while (start < size) {
            float width = 0.0f;
            int lastSpace = -1;
            int pos = start;

            for (; pos < size; pos++) {
                char c = text[pos];
                if (c == ' ') lastSpace = pos;

                // Get character width (glyph width + 1px spacing)
                int glyphW = font->GetCharWidth(static_cast<unsigned char>(c));
                width += static_cast<float>(glyphW + 1);

                if (width > static_cast<float>(maxWidth) && pos > start) {
                    // Break at last space, or at current char if no space
                    int breakPos = (lastSpace > start) ? lastSpace : pos;
                    lines.push_back(text.substr(start, breakPos - start));
                    // Skip the space character if breaking at a space
                    start = (lastSpace > start && breakPos == lastSpace) ? breakPos + 1 : breakPos;
                    break;
                }
            }

            if (pos >= size) {
                // Remaining text fits on one line
                lines.push_back(text.substr(start));
                break;
            }
        }

        if (lines.empty()) {
            lines.push_back("");
        }
        return lines;
    }

    void ChatComponent::AddMessage(const std::string& text, uint32_t color) {
        ChatMessage msg;
        msg.text = text;
        msg.addedTime = m_gameTime;
        msg.color = color;
        m_messages.push_back(msg);

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
        // MC translate(4) + fill(-4, ..., maxWidth+8, ...) = screen [0, maxWidth+12]
        int bgLeft = 0;
        int bgRight = MESSAGE_INDENT + maxWidth + MESSAGE_INDENT + MESSAGE_INDENT;

        int maxLines = chatOpen ? MAX_MESSAGES : MAX_VISIBLE_LINES;
        int linesRendered = 0;

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

            // Split message into wrapped lines
            std::vector<std::string> wrappedLines = SplitLines(msg.text, maxWidth,
                                                                graphics.GetFontRenderer());

            // Render wrapped lines bottom-up (last line of the wrap at the bottom)
            for (int li = static_cast<int>(wrappedLines.size()) - 1; li >= 0 && linesRendered < maxLines; li--) {
                int lineY = baseY - (linesRendered + 1) * LINE_HEIGHT;

                // Background
                uint8_t bgAlpha = static_cast<uint8_t>(alpha * 128.0f);
                uint32_t bgColor = (static_cast<uint32_t>(bgAlpha) << 24);
                graphics.Fill(bgLeft, lineY, bgRight, lineY + LINE_HEIGHT, bgColor);

                // Text
                uint8_t textAlpha = static_cast<uint8_t>(alpha * 255.0f);
                uint32_t textColor = (static_cast<uint32_t>(textAlpha) << 24) |
                                     (msg.color & 0x00FFFFFF);
                graphics.DrawString(wrappedLines[li], MESSAGE_INDENT, lineY + 1, textColor, true);

                linesRendered++;
            }
        }
    }

    void ChatComponent::Clear() {
        m_messages.clear();
    }

} // namespace Render
