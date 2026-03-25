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
        m_cursorTimer = 0.0f;
        m_cursorVisible = true;
        m_historyIndex = -1;
    }

    void ChatScreen::Close() {
        m_open = false;
        m_inputText.clear();
    }

    void ChatScreen::OnCharInput(unsigned int codepoint) {
        if (!m_open) return;
        if (static_cast<int>(m_inputText.size()) >= MAX_MESSAGE_LENGTH) return;

        // Only accept printable ASCII for now
        if (codepoint >= 32 && codepoint < 127) {
            m_inputText += static_cast<char>(codepoint);
            m_cursorTimer = 0.0f;
            m_cursorVisible = true;
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
            if (!m_inputText.empty()) {
                m_inputText.pop_back();
                m_cursorTimer = 0.0f;
                m_cursorVisible = true;
            }
            return true;
        }

        if (glfwKey == GLFW_KEY_UP) {
            // Navigate history (older)
            if (!m_history.empty()) {
                if (m_historyIndex < 0) {
                    m_historyIndex = static_cast<int>(m_history.size()) - 1;
                } else if (m_historyIndex > 0) {
                    m_historyIndex--;
                }
                m_inputText = m_history[m_historyIndex];
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

        // Render input text
        std::string displayText = m_inputText;
        if (m_cursorVisible) {
            displayText += "_";
        }
        graphics.DrawString(displayText, 4, inputY + 2, 0xFFFFFFFF, true);
    }

    std::string ChatScreen::ConsumeSubmittedMessage() {
        std::string msg = m_submittedMessage;
        m_submittedMessage.clear();
        return msg;
    }

} // namespace Render
