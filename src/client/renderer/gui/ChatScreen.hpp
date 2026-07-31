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
        int m_cursorPos = 0;          // Caret position in m_inputText (0..size())
        // MC EditBox.java line 408 uses real wall-clock millis, NOT frame deltas:
        //   showCursor = (Util.getMillis() - focusedTime) / 300L % 2L == 0L
        // We match that with steady_clock so the blink rate is independent of FPS.
        long long m_focusedAtMillis = 0;

        // Chat history
        std::vector<std::string> m_history;
        int m_historyIndex = -1;

        // ── Tab-completion suggestions popup — mirrors MC's
        //    CommandSuggestions / SuggestionsList. Closed by default; TAB
        //    opens (computes suggestions for the word at the cursor) and
        //    applies the selected one. UP/DOWN cycle while open. Any
        //    other text-mutating input closes it.
        std::vector<std::string> m_suggestions;
        int  m_suggestionIndex = 0;
        int  m_suggestionAnchor = 0;   // Start of the word being completed (index in m_inputText)
        bool m_suggestionsOpen = false;
        // MC's SuggestionsList shows up to 10 entries at once and scrolls
        // for more. Match the constant.
        static constexpr int MAX_VISIBLE_SUGGESTIONS = 10;

        // Suggestion lifecycle. ComputeSuggestions tokenises the input,
        // dispatches per command, fills m_suggestions / m_suggestionAnchor.
        void OpenSuggestions();
        void CloseSuggestions();
        void ApplySelectedSuggestion();
        void CycleSuggestion(int delta);
        void RenderSuggestions(GuiGraphics& graphics, int inputX, int inputY);

        // Helpers — MC's EditBox.setCursorPosition / moveCursor / moveCursorToStart / End
        void SetCursorPosition(int pos);
        void MoveCursor(int dir);
        void MoveCursorToStart();
        void MoveCursorToEnd();
        // MC: ResetCursorBlink resets focusedTime so the cursor reappears immediately on input
        void ResetCursorBlink();
        bool ShouldShowCursor() const; // Reads steady_clock — matches MC's getMillis pattern
    };

} // namespace Render
