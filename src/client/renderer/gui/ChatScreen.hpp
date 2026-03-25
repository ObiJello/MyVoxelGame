// File: src/client/renderer/gui/ChatScreen.hpp
// Chat input field matching MC's ChatScreen.java.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace Render {
    class GuiGraphics;

    class ChatScreen {
    public:
        static constexpr int MAX_MESSAGE_LENGTH = 256;
        static constexpr int INPUT_HEIGHT = 12;

        void Open(bool withSlash = false);
        void Close();
        bool IsOpen() const { return m_open; }

        // Input handling
        void OnCharInput(unsigned int codepoint);
        bool OnKeyDown(int glfwKey);  // Returns true if key was consumed

        // Update cursor blink
        void Update(float deltaTime);

        // Render the input field
        void Render(GuiGraphics& graphics);

        // Get submitted message (empty if none pending)
        std::string ConsumeSubmittedMessage();

    private:
        bool m_open = false;
        std::string m_inputText;
        std::string m_submittedMessage;
        float m_cursorTimer = 0.0f;
        bool m_cursorVisible = true;

        // Chat history
        std::vector<std::string> m_history;
        int m_historyIndex = -1;
    };

} // namespace Render
