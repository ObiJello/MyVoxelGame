// File: src/client/renderer/gui/ChatScreen.cpp
#include "ChatScreen.hpp"
#include "GuiGraphics.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>

namespace Render {

    void ChatScreen::Open(bool withSlash) {
        m_open = true;
        m_inputText = withSlash ? "/" : "";
        m_submittedMessage.clear();
        m_cursorPos = static_cast<int>(m_inputText.size()); // Caret at end (after '/' if present)
        ResetCursorBlink();
        m_historyIndex = -1;
    }

    void ChatScreen::Close() {
        m_open = false;
        m_inputText.clear();
        m_cursorPos = 0;
    }

    void ChatScreen::SetCursorPosition(int pos) {
        // MC: Mth.clamp(pos, 0, value.length())
        const int n = static_cast<int>(m_inputText.size());
        m_cursorPos = std::clamp(pos, 0, n);
        ResetCursorBlink();
    }

    void ChatScreen::MoveCursor(int dir) { SetCursorPosition(m_cursorPos + dir); }
    void ChatScreen::MoveCursorToStart() { SetCursorPosition(0); }
    void ChatScreen::MoveCursorToEnd()   { SetCursorPosition(static_cast<int>(m_inputText.size())); }

    void ChatScreen::OnCharInput(unsigned int codepoint) {
        if (!m_open) return;
        if (static_cast<int>(m_inputText.size()) >= MAX_MESSAGE_LENGTH) return;

        // Only accept printable ASCII for now
        if (codepoint >= 32 && codepoint < 127) {
            // MC: EditBox.insertText splices at cursor and advances by length inserted.
            m_inputText.insert(m_inputText.begin() + m_cursorPos, static_cast<char>(codepoint));
            SetCursorPosition(m_cursorPos + 1);
        }
    }

    bool ChatScreen::OnKeyDown(int glfwKey) {
        if (!m_open) return false;

        if (glfwKey == GLFW_KEY_ENTER || glfwKey == GLFW_KEY_KP_ENTER) {
            // Submit message
            if (!m_inputText.empty()) {
                m_submittedMessage = m_inputText;
                // Add to history
                m_history.push_back(m_inputText);
                if (static_cast<int>(m_history.size()) > 50) {
                    m_history.erase(m_history.begin());
                }
            }
            Close();
            return true;
        }

        if (glfwKey == GLFW_KEY_ESCAPE) {
            Close();
            return true;
        }

        if (glfwKey == GLFW_KEY_BACKSPACE) {
            // MC: EditBox.deleteText(-1) — remove the char before the cursor and step back.
            if (m_cursorPos > 0) {
                m_inputText.erase(m_inputText.begin() + (m_cursorPos - 1));
                SetCursorPosition(m_cursorPos - 1);
            }
            return true;
        }

        if (glfwKey == GLFW_KEY_DELETE) {
            // MC: EditBox.deleteText(+1) — remove the char at the cursor.
            if (m_cursorPos < static_cast<int>(m_inputText.size())) {
                m_inputText.erase(m_inputText.begin() + m_cursorPos);
                ResetCursorBlink();
            }
            return true;
        }

        if (glfwKey == GLFW_KEY_LEFT)  { MoveCursor(-1); return true; }
        if (glfwKey == GLFW_KEY_RIGHT) { MoveCursor(+1); return true; }
        if (glfwKey == GLFW_KEY_HOME)  { MoveCursorToStart(); return true; }
        if (glfwKey == GLFW_KEY_END)   { MoveCursorToEnd();   return true; }

        if (glfwKey == GLFW_KEY_UP) {
            // Navigate history (older)
            if (!m_history.empty()) {
                if (m_historyIndex < 0) {
                    m_historyIndex = static_cast<int>(m_history.size()) - 1;
                } else if (m_historyIndex > 0) {
                    m_historyIndex--;
                }
                m_inputText = m_history[m_historyIndex];
                MoveCursorToEnd();
            }
            return true;
        }

        if (glfwKey == GLFW_KEY_DOWN) {
            // Navigate history (newer)
            if (m_historyIndex >= 0) {
                m_historyIndex++;
                if (m_historyIndex >= static_cast<int>(m_history.size())) {
                    m_historyIndex = -1;
                    m_inputText.clear();
                } else {
                    m_inputText = m_history[m_historyIndex];
                }
                MoveCursorToEnd();
            }
            return true;
        }

        return true; // Consume all keys when chat is open
    }

    void ChatScreen::Update(float deltaTime) {
        if (!m_open) return;
        m_cursorTimer += deltaTime;
        if (m_cursorTimer >= 0.5f) {
            m_cursorTimer -= 0.5f;
            m_cursorVisible = !m_cursorVisible;
        }
    }

    void ChatScreen::Render(GuiGraphics& graphics) {
        if (!m_open) return;

        int guiWidth = graphics.GuiWidth();
        int guiHeight = graphics.GuiHeight();

        // Dark background bar at bottom (MC: full width, 12px tall)
        int inputY = guiHeight - INPUT_HEIGHT - 2;
        graphics.Fill(0, inputY, guiWidth, guiHeight, 0x80000000);

        // Render the full input text (no trailing underscore — cursor is drawn separately)
        graphics.DrawString(m_inputText, 4, inputY + 2, 0xFFFFFFFF, true);

        // Cursor: draw at the actual caret position. MC uses a vertical bar at end-of-text
        // and a blinking underscore mid-text; we use a vertical bar in both cases for clarity.
        if (m_cursorVisible) {
            std::string before = m_inputText.substr(0, m_cursorPos);
            int cursorX = 4 + graphics.GetStringWidth(before);
            // 1px wide, 9px tall white bar (MC font glyphs are 8px tall + 1px shadow)
            graphics.Fill(cursorX, inputY + 1, cursorX + 1, inputY + 1 + 9, 0xFFFFFFFF);
        }
    }

    std::string ChatScreen::ConsumeSubmittedMessage() {
        std::string msg = m_submittedMessage;
        m_submittedMessage.clear();
        return msg;
    }

} // namespace Render
