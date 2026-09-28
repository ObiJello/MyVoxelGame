// File: src/client/renderer/gui/ChatScreen.hpp
// Chat input field matching MC's ChatScreen.java.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace Render {
    class GuiGraphics;

    // ── Server command list (MC ClientboundCommandsPacket) ──────────────────
    //
    // Set from CommandsS2C on join; read by tab-completion. This exists so the
    // client never hardcodes what the server registers — that list drifted
    // every time a command was added and the new one silently never appeared
    // in the popup.
    //
    // Main-thread only: written by the packet handler and read by the chat
    // screen, both of which run on the main thread (packets apply there via
    // IPacket::apply).
    void SetServerCommandNames(std::vector<std::string> names);
    const std::vector<std::string>& GetServerCommandNames();

    // The player's current dimension's biome / structure ids and tags
    // (WorldgenIdsS2C, sent on join and on every dimension change): what
    // /locate completes, shown without namespaces. Full ids in.
    void SetDimensionWorldgenIds(const std::vector<std::string>& biomes,
                                 const std::vector<std::string>& structures,
                                 const std::vector<std::string>& biomeTags,
                                 const std::vector<std::string>& structureTags);

    class ChatScreen {
    public:
        static constexpr int MAX_MESSAGE_LENGTH = 256;
        static constexpr int INPUT_HEIGHT = 12;
        // Pitch of the usage-hint lines above the input (the single hint
        // always sat 11 px above it).
        static constexpr int HINT_LINE_STEP = 11;

        void Open(bool withSlash = false);
        void Close();
        bool IsOpen() const { return m_open; }
        // MC ChatScreen(initial, isDraft, closeOnSubmit): the in-bed chat
        // keeps the box open after Enter (InBedChatScreen passes false).
        void SetCloseOnSubmit(bool close) { m_closeOnSubmit = close; }

        // Input handling
        void OnCharInput(unsigned int codepoint);
        // Paste: every printable character of `text` at the caret, in order,
        // up to the message limit. Line breaks are dropped (MC's
        // SharedConstants.filterText), so a pasted command stays one line.
        void InsertText(const std::string& text);
        // The current input line, for copy.
        const std::string& InputText() const { return m_inputText; }
        // Replace the input line, caret at its end (/control's mirror
        // adopts the controlled client's line when it opens).
        void SetInputText(const std::string& text) {
            m_inputText = text.substr(0, static_cast<size_t>(MAX_MESSAGE_LENGTH));
            m_cursorPos = static_cast<int>(m_inputText.size());
        }
        bool OnKeyDown(int glfwKey);  // Returns true if key was consumed

        // Update cursor blink
        void Update(float deltaTime);

        // Render the input field
        void Render(GuiGraphics& graphics);

        // Mouse wheel while chat is open: scrolls the completion list (MC
        // CommandSuggestions.mouseScrolled). The caller consumes the wheel
        // either way, so it never reaches the hotbar.
        void OnMouseScrolled(double deltaY);

        // How far the chat history must rise so a multi-line usage hint
        // never overlaps it: one HINT_LINE_STEP per line past the first
        // (the first fits the gap MC leaves above the input).
        int HistoryLift() const;

        // Get submitted message (empty if none pending)
        std::string ConsumeSubmittedMessage();

    private:
        bool m_open = false;
        std::string m_inputText;
        bool m_closeOnSubmit = true;
        std::string m_submittedMessage;
        int m_cursorPos = 0;          // Caret position in m_inputText (0..size())
        // MC EditBox.java line 408 uses real wall-clock millis, NOT frame deltas:
        //   showCursor = (Util.getMillis() - focusedTime) / 300L % 2L == 0L
        // We match that with steady_clock so the blink rate is independent of FPS.
        long long m_focusedAtMillis = 0;

        // Chat history
        std::vector<std::string> m_history;
    public:
        // Drops the up-arrow recall history. Paired with ChatComponent::Clear()
        // by /clearchat so the chat really is back to its just-launched state
        // rather than just visually empty.
        void ClearHistory() { m_history.clear(); m_historyIndex = -1; }
    private:
        int m_historyIndex = -1;

        // ── Tab-completion suggestions popup — mirrors MC's
        //    CommandSuggestions / SuggestionsList. Closed by default; TAB
        //    opens (computes suggestions for the word at the cursor) and
        //    applies the selected one. UP/DOWN cycle while open. Any
        //    other text-mutating input closes it.
        std::vector<std::string> m_suggestions;
        int  m_suggestionIndex = 0;
        int  m_suggestionAnchor = 0;   // Start of the word being completed (index in m_inputText)
        int  m_suggestionScroll = 0;   // First visible row (MC SuggestionsList.offset)
        double    m_suggestionScrollAccum  = 0.0;   // fractional wheel rows not yet applied
        long long m_suggestionScrollLastMs = 0;     // last wheel frame (velocity)
        bool m_suggestionsOpen = false;
        // How many characters the completion currently occupies, starting at
        // m_suggestionAnchor. Needed because TAB now types the suggestion
        // straight into the field and each further TAB has to replace what the
        // previous one wrote — not the shrinking partial the player typed.
        int  m_suggestionCurrentLen = 0;
        // MC's SuggestionsList shows up to 10 entries at once and scrolls
        // for more. Match the constant.
        static constexpr int MAX_VISIBLE_SUGGESTIONS = 10;

        // Suggestion lifecycle. OpenSuggestions asks CommandSuggestions (the
        // walk of the server's command trees) and fills m_suggestions /
        // m_suggestionAnchor.
        void OpenSuggestions();
        void CloseSuggestions();
        // Writes the highlighted suggestion into the input field, replacing
        // whatever is currently in the completion slot. Both opening the popup
        // and cycling it go through here, so the field always shows the entry
        // that is highlighted.
        void ApplySuggestionInPlace();
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
