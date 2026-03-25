// File: src/client/renderer/gui/ChatComponent.hpp
// Chat message storage and HUD rendering, matching MC's ChatComponent.java.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace Render {
    class GuiGraphics;

    struct ChatMessage {
        std::string text;
        float addedTime = 0.0f;
        uint32_t color = 0xFFFFFFFF;
    };

    class ChatComponent {
    public:
        static constexpr int MAX_MESSAGES = 100;
        static constexpr float FADE_DURATION = 10.0f;   // Seconds fully visible
        static constexpr float FADE_TIME = 1.0f;         // Seconds to fade out
        static constexpr int MAX_VISIBLE_LINES = 10;
        static constexpr int LINE_HEIGHT = 9;
        static constexpr int MESSAGE_INDENT = 4;             // MC: MESSAGE_INDENT = 4
        static constexpr int CHAT_BOTTOM_MARGIN = 40;        // MC: screenHeight - 40
        static constexpr int CHAT_WIDTH = 320;               // MC: floor(1.0 * 280 + 40) = 320

        void AddMessage(const std::string& text, uint32_t color = 0xFFFFFFFF);
        void Render(GuiGraphics& graphics, float gameTime, bool chatOpen);
        void Clear();
        void Update(float deltaTime);

        float GetGameTime() const { return m_gameTime; }

    private:
        std::vector<ChatMessage> m_messages;
        float m_gameTime = 0.0f;
    };

} // namespace Render
