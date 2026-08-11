// File: src/client/renderer/gui/ChatComponent.hpp
// Chat message storage and HUD rendering, matching MC's ChatComponent.java.
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace Render {
    class GuiGraphics;

    // What clicking a chat segment does. MC's ClickEvent has several actions
    // (open URL, run command, suggest command, …); we only need the one
    // /seed uses, and the enum leaves room for the rest.
    enum class ChatClickAction : uint8_t {
        None = 0,
        CopyToClipboard,   // MC ClickEvent.CopyToClipboard
    };

    // One styled run within a message — our stand-in for MC's Component tree.
    // A flat list is enough because nothing here nests styles.
    struct ChatSegment {
        std::string text;
        uint32_t    color = 0xFFFFFFFF;
        ChatClickAction click = ChatClickAction::None;
        std::string clickValue;   // payload for the action (the text to copy)
        std::string hoverText;    // shown while the cursor is over this run
    };

    struct ChatMessage {
        std::vector<ChatSegment> segments;
        float addedTime = 0.0f;
    };

    // Installed by the platform layer, which owns the GLFW window. Kept as a
    // callback so the GUI doesn't have to reach for a GLFWwindow* it has no
    // other reason to know about.
    void SetClipboardHandler(std::function<void(const std::string&)> handler);
    void CopyToClipboard(const std::string& text);

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

        // Plain-text message — the shape every existing caller uses.
        void AddMessage(const std::string& text, uint32_t color = 0xFFFFFFFF);
        // Styled message with per-run colour / click / hover.
        void AddMessage(std::vector<ChatSegment> segments);

        void Render(GuiGraphics& graphics, float gameTime, bool chatOpen);
        void Clear();
        void Update(float deltaTime);

        // Cursor position in GUI coordinates, for hover highlighting and click
        // hit-testing. Only meaningful while chat is open.
        void SetMousePos(int x, int y) { m_mouseX = x; m_mouseY = y; }

        // Handle a click at the current mouse position. Returns true when a
        // clickable segment was hit and its action ran. Hit rectangles come
        // from the most recent Render, which is MC's approach too
        // (ChatComponent.getClickedComponentStyleAt walks the same laid-out
        // lines the renderer produced).
        bool HandleClick();

        // True when the cursor is over a clickable segment. Drives the pointer
        // cursor swap, which the platform layer applies (it owns the window).
        bool IsHoveringClickable() const;

        float GetGameTime() const { return m_gameTime; }

    private:
        struct ClickRegion {
            int x0, y0, x1, y1;
            ChatClickAction action;
            std::string value;
            std::string hoverText;
        };

        std::vector<ChatMessage> m_messages;
        std::vector<ClickRegion> m_clickRegions;  // rebuilt every Render
        float m_gameTime = 0.0f;
        int m_mouseX = -1;
        int m_mouseY = -1;
    };

} // namespace Render
