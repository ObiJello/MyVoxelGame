// File: src/client/renderer/gui/ChatScreen.cpp
#include "ChatScreen.hpp"
#include "CommandSuggestions.hpp"
#include "GuiGraphics.hpp"
#include "FontRenderer.hpp"
#include "common/network/packets/game/ChatMessageC2SPacket.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>

static_assert(Render::ChatScreen::MAX_MESSAGE_LENGTH == static_cast<int>(Network::kMaxChatMessageLength),
              "the chat box and the chat packet must agree on the length cap");

namespace {
    // Wall-clock millis since some fixed epoch, mirroring Java's System.currentTimeMillis() /
    // Util.getMillis() that MC uses in EditBox.renderWidget for cursor blinking.
    long long NowMillis() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // The usage hint in the chat's original look: plain grey shadowed text,
    // no backing, one line every HINT_LINE_STEP px stacking upward so the
    // last line sits where the single hint always did (`bottomY` - step).
    void DrawHintLines(Render::GuiGraphics& graphics,
                       const std::vector<Render::CommandSuggestions::HintLine>& lines,
                       int x, int bottomY) {
        const int n = static_cast<int>(lines.size());
        for (int i = 0; i < n; ++i) {
            const int y = bottomY - (n - i) * Render::ChatScreen::HINT_LINE_STEP;
            if (y < 0) continue;
            graphics.DrawString(lines[static_cast<size_t>(i)].text, x, y, 0xFFA0A0A0, true);
        }
    }
}

namespace Render {

    namespace {
        // Populated by CommandsS2C on join. The fallback below is what a
        // pre-CommandsS2C server (or a session where the packet hasn't landed
        // yet) gets: the commands that existed when tab-completion was written.
        // It is deliberately NOT kept up to date — the whole point of the
        // packet is that this list stops mattering.
        std::vector<std::string> s_serverCommandNames = {
            "difficulty", "dimension", "entitystats", "gamemode", "gamerule", "kick", "kill", "locate", "portal", "scale", "seed",
            "shape", "sheepeat", "summon", "teleport", "tick", "time", "tp",
        };
    } // namespace

    void SetServerCommandNames(std::vector<std::string> names) {
        s_serverCommandNames = std::move(names);
    }

    const std::vector<std::string>& GetServerCommandNames() {
        return s_serverCommandNames;
    }

    void SetDimensionWorldgenIds(const std::vector<std::string>& biomes,
                                 const std::vector<std::string>& structures,
                                 const std::vector<std::string>& biomeTags,
                                 const std::vector<std::string>& structureTags) {
        CommandSuggestions::SetDimensionWorldgenIds(biomes, structures, biomeTags, structureTags);
    }

    void ChatScreen::Open(bool withSlash) {
        m_open = true;
        m_inputText = withSlash ? "/" : "";
        m_submittedMessage.clear();
        m_displayPos = 0;
        MoveCursorTo(static_cast<int>(m_inputText.size()), false); // Caret at end (after '/' if present)
        m_historyIndex = -1;
    }

    void ChatScreen::Close() {
        m_open = false;
        m_inputText.clear();
        m_cursorPos = m_highlightPos = m_displayPos = 0;
        CloseSuggestions();
    }

    void ChatScreen::ResetCursorBlink() {
        // MC's behavior: focusedTime is updated when the cursor moves or the field gains focus,
        // so the cursor immediately reappears in the "visible" half of its blink cycle.
        m_focusedAtMillis = NowMillis();
    }

    bool ChatScreen::ShouldShowCursor() const {
        // MC EditBox.java line 408 (verbatim formula):
        //   showCursor = (Util.getMillis() - focusedTime) / 300L % 2L == 0L
        // → 300ms visible / 300ms hidden, total 600ms cycle, driven by wall-clock millis.
        long long elapsed = NowMillis() - m_focusedAtMillis;
        if (elapsed < 0) elapsed = 0;
        return ((elapsed / 300LL) % 2LL) == 0LL;
    }

    // ── MC EditBox: measuring, scrolling, selection ─────────────────────────
    //
    // The line is one row that scrolls horizontally (EditBox.displayPos):
    // only the characters from m_displayPos that fit the box are drawn, and
    // every caret move scrolls just enough to keep the caret visible. Widths
    // are the font's per-glyph advances (FontRenderer: glyph + 1 px, the input
    // being printable ASCII), so measuring a 32 767-character line stays
    // linear in what is measured.

    int ChatScreen::CharAdvance(char c) const {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!m_font) return 6;
        if (u < 32 || u > 126) return 4;
        return m_font->GetCharWidth(u) + 1;
    }

    int ChatScreen::TextWidth(int from, int to) const {
        int w = 0;
        for (int i = std::max(0, from); i < to && i < static_cast<int>(m_inputText.size()); ++i) {
            w += CharAdvance(m_inputText[static_cast<size_t>(i)]);
        }
        return w > 0 ? w - 1 : 0;   // no spacing after the last glyph
    }

    // MC Font.plainSubstrByWidth(value.substring(from), width).length().
    int ChatScreen::FitForward(int from, int width) const {
        int w = 0, n = 0;
        for (int i = from; i < static_cast<int>(m_inputText.size()); ++i) {
            const int adv = CharAdvance(m_inputText[static_cast<size_t>(i)]);
            if (w + adv - 1 > width) break;
            w += adv;
            ++n;
        }
        return n;
    }

    // The same from the other end: characters ending at `to` within width.
    int ChatScreen::FitBackward(int to, int width) const {
        int w = 0, n = 0;
        for (int i = std::min(to, static_cast<int>(m_inputText.size())) - 1; i >= 0; --i) {
            const int adv = CharAdvance(m_inputText[static_cast<size_t>(i)]);
            if (w + adv - 1 > width) break;
            w += adv;
            ++n;
        }
        return n;
    }

    // MC EditBox.scrollTo: keep `pos` inside the shown window; reaching the
    // left edge pages a window's worth back.
    void ChatScreen::ScrollTo(int pos) {
        const int length = static_cast<int>(m_inputText.size());
        m_displayPos = std::min(m_displayPos, length);
        const int lastPos = m_displayPos + FitForward(m_displayPos, m_innerWidth);
        if (pos == m_displayPos && m_displayPos > 0) {
            m_displayPos -= FitBackward(m_displayPos, m_innerWidth);
        }
        if (pos > lastPos) {
            m_displayPos += pos - lastPos;
        } else if (pos <= m_displayPos) {
            m_displayPos -= m_displayPos - pos;
        }
        m_displayPos = std::clamp(m_displayPos, 0, length);
    }

    void ChatScreen::MoveCursorTo(int pos, bool extendSelection) {
        const int n = static_cast<int>(m_inputText.size());
        m_cursorPos = std::clamp(pos, 0, n);
        if (!extendSelection) m_highlightPos = m_cursorPos;
        m_highlightPos = std::clamp(m_highlightPos, 0, n);
        ScrollTo(m_cursorPos);
        ResetCursorBlink();
    }

    void ChatScreen::SetCursorPosition(int pos) { MoveCursorTo(pos, false); }

    void ChatScreen::MoveCursor(int dir) { SetCursorPosition(m_cursorPos + dir); }
    void ChatScreen::MoveCursorToStart() { SetCursorPosition(0); }
    void ChatScreen::MoveCursorToEnd()   { SetCursorPosition(static_cast<int>(m_inputText.size())); }

    // MC EditBox.getWordPosition(dir, from, stripSpaces = true).
    int ChatScreen::WordPosition(int dir, int from) const {
        const int length = static_cast<int>(m_inputText.size());
        int result = from;
        if (dir > 0) {
            const size_t space = m_inputText.find(' ', static_cast<size_t>(result));
            result = space == std::string::npos ? length : static_cast<int>(space);
            while (result < length && m_inputText[static_cast<size_t>(result)] == ' ') ++result;
        } else {
            while (result > 0 && m_inputText[static_cast<size_t>(result - 1)] == ' ') --result;
            while (result > 0 && m_inputText[static_cast<size_t>(result - 1)] != ' ') --result;
        }
        return result;
    }

    // MC EditBox.deleteText / deleteCharsToPos: a selection goes first;
    // otherwise everything between the caret and `fromCursorTo`.
    void ChatScreen::DeleteSelectionOr(int fromCursorTo) {
        int start = std::min(m_cursorPos, m_highlightPos);
        int end   = std::max(m_cursorPos, m_highlightPos);
        if (start == end) {
            const int to = std::clamp(fromCursorTo, 0, static_cast<int>(m_inputText.size()));
            start = std::min(m_cursorPos, to);
            end   = std::max(m_cursorPos, to);
        }
        if (start == end) return;
        m_inputText.erase(static_cast<size_t>(start), static_cast<size_t>(end - start));
        MoveCursorTo(start, false);
    }

    std::string ChatScreen::CopyText() const {
        const int start = std::min(m_cursorPos, m_highlightPos);
        const int end   = std::max(m_cursorPos, m_highlightPos);
        if (start == end) return m_inputText;
        return m_inputText.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
    }

    std::string ChatScreen::CutText() {
        if (m_cursorPos == m_highlightPos) return {};
        std::string cut = CopyText();
        DeleteSelectionOr(m_cursorPos);
        CloseSuggestions();
        return cut;
    }

    int ChatScreen::IndexAtX(double guiX) const {
        constexpr int kInputX = 4;
        const int width = static_cast<int>(std::floor(guiX)) - kInputX;
        if (width <= 0) return m_displayPos;
        return m_displayPos + FitForward(m_displayPos, width);
    }

    bool ChatScreen::OnMouseClicked(double guiX, double guiY, bool shift) {
        if (!m_open || guiY < m_inputBarY) return false;
        MoveCursorTo(IndexAtX(guiX), shift);
        CloseSuggestions();
        return true;
    }

    void ChatScreen::OnMouseDragged(double guiX) {
        if (!m_open) return;
        MoveCursorTo(IndexAtX(guiX), true);
    }

    void ChatScreen::InsertText(const std::string& text) {
        if (!m_open) return;
        // MC EditBox.insertText: filtered (SharedConstants.filterText — line
        // breaks and controls dropped), replacing the selection, cut at the
        // length limit.
        std::string filtered;
        filtered.reserve(text.size());
        for (const char c : text) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u >= 32 && u < 127) filtered += c;
        }
        const int start = std::min(m_cursorPos, m_highlightPos);
        const int end   = std::max(m_cursorPos, m_highlightPos);
        const int room  = MAX_MESSAGE_LENGTH - (static_cast<int>(m_inputText.size()) - (end - start));
        if (room < static_cast<int>(filtered.size())) filtered.resize(static_cast<size_t>(std::max(0, room)));
        m_inputText.replace(static_cast<size_t>(start), static_cast<size_t>(end - start), filtered);
        MoveCursorTo(start + static_cast<int>(filtered.size()), false);
        // MC: typing closes the suggestion popup. The user types TAB again to
        // re-open with the new word.
        CloseSuggestions();
    }

    void ChatScreen::OnCharInput(unsigned int codepoint) {
        if (!m_open) return;
        // Only printable ASCII for now.
        if (codepoint >= 32 && codepoint < 127) InsertText(std::string(1, static_cast<char>(codepoint)));
    }

    // ── Cycle-in-place completion ───────────────────────────────────────────
    //
    // Deliberately NOT MC's model. Vanilla opens a popup, leaves the field
    // alone, and only splices the highlighted entry in when you press TAB a
    // second time. Here the first TAB both opens the list AND types the first
    // entry into the field; every further TAB overwrites it with the next one,
    // wrapping at the end. The list stays visible the whole time so you can see
    // what you are cycling through, and typing anything — a space, most
    // usefully — closes the popup and keeps whatever word is showing.
    //
    // The suggestions are computed ONCE, from the partial word as originally
    // typed, and never recomputed while cycling. Recomputing would be wrong:
    // after the first TAB the field holds a complete command, which matches
    // only itself, and the list would collapse to one entry after a single
    // press.
    void ChatScreen::OpenSuggestions() {
        // Every argument completes from the command's tree (CommandsS2C) —
        // see CommandSuggestions; there is no per-command code here.
        auto completion = CommandSuggestions::Complete(m_inputText, m_cursorPos);
        if (!completion || completion->items.empty()) {
            CloseSuggestions();
            return;
        }
        m_suggestions      = std::move(completion->items);
        m_suggestionAnchor = completion->anchor;
        m_suggestionIndex  = 0;
        m_suggestionScroll = 0;
        m_suggestionsOpen  = true;
        // The slot currently holds the partial word the player typed, so the
        // first apply replaces exactly that.
        m_suggestionCurrentLen = std::max(0, m_cursorPos - m_suggestionAnchor);
        ApplySuggestionInPlace();
    }

    void ChatScreen::CloseSuggestions() {
        m_suggestionsOpen = false;
        m_suggestions.clear();
        m_suggestionIndex = 0;
        m_suggestionScroll = 0;
        m_suggestionCurrentLen = 0;
    }

    void ChatScreen::CycleSuggestion(int delta) {
        if (!m_suggestionsOpen || m_suggestions.empty()) return;
        const int n = static_cast<int>(m_suggestions.size());
        m_suggestionIndex = ((m_suggestionIndex + delta) % n + n) % n;
        // MC SuggestionsList.select: scroll just enough to show the selection.
        if (m_suggestionIndex < m_suggestionScroll) {
            m_suggestionScroll = m_suggestionIndex;
        } else if (m_suggestionIndex >= m_suggestionScroll + MAX_VISIBLE_SUGGESTIONS) {
            m_suggestionScroll = m_suggestionIndex - MAX_VISIBLE_SUGGESTIONS + 1;
        }
        ApplySuggestionInPlace();
    }

    void ChatScreen::ApplySuggestionInPlace() {
        if (!m_suggestionsOpen || m_suggestions.empty()) return;
        if (m_suggestionIndex < 0 || m_suggestionIndex >= (int)m_suggestions.size()) return;

        const int textLen = static_cast<int>(m_inputText.size());
        // The anchor can go stale if the text changed under us (history recall,
        // a paste). Bail rather than splice at a nonsense offset.
        if (m_suggestionAnchor < 0 || m_suggestionAnchor > textLen) {
            CloseSuggestions();
            return;
        }

        const std::string& sel = m_suggestions[m_suggestionIndex];
        const int replaceLen = std::clamp(m_suggestionCurrentLen, 0, textLen - m_suggestionAnchor);

        // Refuse a replacement that would overflow the field — MC clamps the
        // same limit on insertText. Leaving the previous entry showing is
        // better than silently producing a truncated command.
        if (textLen - replaceLen + static_cast<int>(sel.size()) > MAX_MESSAGE_LENGTH) {
            return;
        }

        m_inputText.replace(m_suggestionAnchor, replaceLen, sel);
        m_suggestionCurrentLen = static_cast<int>(sel.size());
        SetCursorPosition(m_suggestionAnchor + static_cast<int>(sel.size()));
    }

    bool ChatScreen::OnKeyDown(int glfwKey, int mods) {
        if (!m_open) return false;
        const bool shift = (mods & GLFW_MOD_SHIFT) != 0;
        // MC Screen.hasControlDown (Cmd on macOS, Ctrl elsewhere); Alt too,
        // macOS's own word modifier.
        const bool word  = (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_ALT)) != 0;

        // Select all (MC EditBox: Screen.isSelectAll).
        if (glfwKey == GLFW_KEY_A && (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER))) {
            MoveCursorTo(static_cast<int>(m_inputText.size()), false);
            m_highlightPos = 0;
            return true;
        }

        // TAB — first press opens the list AND fills in its first entry; each
        // further press walks down the list, replacing the filled-in word and
        // wrapping around at the end. Whatever is showing when you type a space
        // (or anything else) is the one you keep. See OpenSuggestions for why
        // this is not MC's two-step model.
        if (glfwKey == GLFW_KEY_TAB) {
            if (m_suggestionsOpen) CycleSuggestion(+1);
            else                   OpenSuggestions();
            return true;
        }

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
            if (m_closeOnSubmit) {
                Close();
            } else {
                // MC ChatScreen.keyPressed with closeOnSubmit = false: the
                // line is sent, the box is emptied and stays up (the bed).
                m_inputText.clear();
                m_displayPos = 0;
                MoveCursorTo(0, false);
                CloseSuggestions();
            }
            return true;
        }

        if (glfwKey == GLFW_KEY_ESCAPE) {
            // MC: ESC first closes the suggestion popup, second press
            // closes the whole chat.
            if (m_suggestionsOpen) { CloseSuggestions(); return true; }
            Close();
            return true;
        }

        if (glfwKey == GLFW_KEY_BACKSPACE) {
            // MC EditBox.deleteText(-1): the selection, else the character
            // (or, with Ctrl/Cmd/Alt, the word) before the caret.
            DeleteSelectionOr(word ? WordPosition(-1, m_cursorPos) : m_cursorPos - 1);
            CloseSuggestions();
            return true;
        }

        if (glfwKey == GLFW_KEY_DELETE) {
            // MC EditBox.deleteText(+1): the selection, else what follows.
            DeleteSelectionOr(word ? WordPosition(+1, m_cursorPos) : m_cursorPos + 1);
            CloseSuggestions();
            return true;
        }

        // Caret moves (MC EditBox.keyPressed): Shift extends the selection;
        // Ctrl/Cmd/Alt jump a word; Home/End (and Cmd+arrow on macOS) go to
        // the ends. Each scrolls the line to keep the caret in view.
        const bool lineEnd = (mods & GLFW_MOD_SUPER) != 0 && (mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT)) == 0;
        if (glfwKey == GLFW_KEY_LEFT) {
            CloseSuggestions();
            MoveCursorTo(lineEnd ? 0 : word ? WordPosition(-1, m_cursorPos) : m_cursorPos - 1, shift);
            return true;
        }
        if (glfwKey == GLFW_KEY_RIGHT) {
            CloseSuggestions();
            MoveCursorTo(lineEnd ? static_cast<int>(m_inputText.size())
                                 : word ? WordPosition(+1, m_cursorPos) : m_cursorPos + 1, shift);
            return true;
        }
        if (glfwKey == GLFW_KEY_HOME) { CloseSuggestions(); MoveCursorTo(0, shift); return true; }
        if (glfwKey == GLFW_KEY_END)  { CloseSuggestions(); MoveCursorTo(static_cast<int>(m_inputText.size()), shift); return true; }

        if (glfwKey == GLFW_KEY_UP) {
            // MC: UP cycles suggestion selection when popup is open, else
            // walks chat history.
            if (m_suggestionsOpen) { CycleSuggestion(-1); return true; }
            if (!m_history.empty()) {
                if (m_historyIndex < 0) {
                    m_historyIndex = static_cast<int>(m_history.size()) - 1;
                } else if (m_historyIndex > 0) {
                    m_historyIndex--;
                }
                m_inputText = m_history[m_historyIndex];
                m_displayPos = 0;
                MoveCursorToEnd();
            }
            return true;
        }

        if (glfwKey == GLFW_KEY_DOWN) {
            if (m_suggestionsOpen) { CycleSuggestion(+1); return true; }
            if (m_historyIndex >= 0) {
                m_historyIndex++;
                if (m_historyIndex >= static_cast<int>(m_history.size())) {
                    m_historyIndex = -1;
                    m_inputText.clear();
                } else {
                    m_inputText = m_history[m_historyIndex];
                }
                m_displayPos = 0;
                MoveCursorToEnd();
            }
            return true;
        }

        return true; // Consume all keys when chat is open
    }

    void ChatScreen::Update(float /*deltaTime*/) {
        // No-op: blink is driven by wall-clock millis in ShouldShowCursor() so the rate is
        // independent of frame rate. MC does the same — see EditBox.renderWidget.
    }

    void ChatScreen::Render(GuiGraphics& graphics) {
        if (!m_open) return;

        // Own stratum, above the chat history. Within one stratum the GUI
        // batcher orders all fills under all text (GuiRenderer::BuildBatches
        // pushes fills first and the sort is stable), so the suggestion
        // popup's background could never cover ChatComponent's lines — a
        // broadcast rendered above the popup box but below its entries. MC
        // solves this the same way: CommandSuggestions renders in a later
        // stratum than the chat.
        graphics.NextStratum();

        int guiWidth = graphics.GuiWidth();
        int guiHeight = graphics.GuiHeight();

        // Dark background bar at bottom (MC: full width, 12px tall)
        int inputY = guiHeight - INPUT_HEIGHT - 2;
        const int inputX = 4;
        graphics.Fill(0, inputY, guiWidth, guiHeight, 0x80000000);

        // MC EditBox.renderWidget: only the window from displayPos that fits
        // the box (font.plainSubstrByWidth), the selection highlighted, the
        // caret relative to the window. The inner width is this frame's; a
        // resize re-scrolls to keep the caret visible.
        m_font = graphics.GetFontRenderer();
        m_inputBarY = inputY;
        const int innerWidth = std::max(16, guiWidth - inputX - 6);
        if (innerWidth != m_innerWidth) {
            m_innerWidth = innerWidth;
            ScrollTo(m_cursorPos);
        }
        m_displayPos = std::clamp(m_displayPos, 0, static_cast<int>(m_inputText.size()));
        const int shownLen = FitForward(m_displayPos, m_innerWidth);
        const int shownEnd = m_displayPos + shownLen;
        const std::string shown = m_inputText.substr(static_cast<size_t>(m_displayPos), static_cast<size_t>(shownLen));
        const int textY = inputY + 2;

        // Selection (MC renderHighlight), clipped to the shown window.
        if (m_highlightPos != m_cursorPos) {
            const int selStart = std::clamp(std::min(m_cursorPos, m_highlightPos), m_displayPos, shownEnd);
            const int selEnd   = std::clamp(std::max(m_cursorPos, m_highlightPos), m_displayPos, shownEnd);
            if (selEnd > selStart) {
                const int x0 = inputX + (selStart > m_displayPos ? TextWidth(m_displayPos, selStart) + 1 : 0);
                const int x1 = inputX + TextWidth(m_displayPos, selEnd) + 1;
                graphics.Fill(x0 - 1, textY - 1, x1, textY + 1 + 9, 0xFF3050C8);
            }
        }
        graphics.DrawString(shown, inputX, textY, 0xFFFFFFFF, true);

        // Cursor — matches MC's EditBox.renderWidget (lines 411-458), measured
        // from the window's start:
        //   - At end of text: underscore at  text_start + width(before) + 1
        //   - Mid-text:       vertical bar at text_start + width(before)
        if (ShouldShowCursor() && m_cursorPos >= m_displayPos && m_cursorPos <= shownEnd) {
            const int beforeWidth = m_cursorPos > m_displayPos ? TextWidth(m_displayPos, m_cursorPos) + 1 : 0;
            const bool atEnd = (m_cursorPos >= static_cast<int>(m_inputText.size()));
            if (atEnd) {
                graphics.DrawString("_", inputX + (beforeWidth > 0 ? beforeWidth : 1), textY, 0xFFFFFFFF, true);
            } else {
                const int barX = inputX + std::max(0, beforeWidth - 1);
                graphics.Fill(barX, textY - 1, barX + 1, textY + 1 + 9, 0xFFFFFFFF);
            }
        }

        // Suggestion popup is drawn LAST so it sits on top of everything.
        // Usage lines for the command being typed (from its tree): grey text
        // directly above the input, or above the popup when that is open.
        if (!m_suggestionsOpen) {
            if (const auto hint = CommandSuggestions::Hint(m_inputText, m_cursorPos); hint && !hint->empty()) {
                DrawHintLines(graphics, *hint, inputX, inputY);
            }
        }

        RenderSuggestions(graphics, inputX, inputY);
    }

    void ChatScreen::RenderSuggestions(GuiGraphics& graphics, int inputX, int inputY) {
        if (!m_suggestionsOpen || m_suggestions.empty()) return;

        // X anchor: align the popup's left edge to the start of the word
        // being completed (MC's CommandSuggestions.render). That's:
        //   inputX + width(prefix-before-anchor)
        // The anchor's x in the SCROLLED line (left of the window: the box's
        // left edge), the popup then kept on screen.
        int anchorX = inputX + (m_suggestionAnchor > m_displayPos ? TextWidth(m_displayPos, m_suggestionAnchor) + 1 : 0);

        // Width: max suggestion width + 2px horizontal padding (MC uses 1
        // px padding either side, total 2 in interior).
        int maxW = 0;
        for (const auto& s : m_suggestions) {
            maxW = std::max(maxW, graphics.GetStringWidth(s));
        }
        anchorX = std::clamp(anchorX, inputX, std::max(inputX, graphics.GuiWidth() - maxW - 2));
        const int boxX0 = anchorX - 1;            // 1px left padding
        const int boxX1 = anchorX + maxW + 1;     // 1px right padding

        // Height + scroll window: up to MAX_VISIBLE_SUGGESTIONS rows of
        // 12px each. If there are more entries than fit, scroll so the
        // selected index stays in view (MC: SuggestionsList.scroll).
        const int total = static_cast<int>(m_suggestions.size());
        const int visible = std::min(total, MAX_VISIBLE_SUGGESTIONS);
        // MC SuggestionsList.offset: its own scroll position, moved by the
        // wheel and pulled along by keyboard selection (select()).
        const int scrollStart = std::clamp(m_suggestionScroll, 0, std::max(0, total - MAX_VISIBLE_SUGGESTIONS));
        const int rowH = 12;
        const int boxH = visible * rowH;
        // MC positions the popup ABOVE the input. Bottom of popup sits
        // at inputY - 1 (1px gap), top at boxY0.
        const int boxY1 = inputY - 1;
        const int boxY0 = boxY1 - boxH;

        // Box background — MC uses 0xD0000000 (~82% black). Drawn under
        // every row at once.
        graphics.Fill(boxX0, boxY0, boxX1, boxY1, 0xD0000000);

        if (const auto hint = CommandSuggestions::Hint(m_inputText, m_suggestionAnchor); hint && !hint->empty()) {
            DrawHintLines(graphics, *hint, inputX, boxY0);
        }

        // Each visible row, top-down.
        for (int i = 0; i < visible; ++i) {
            const int idx = scrollStart + i;
            const std::string& text = m_suggestions[idx];
            const int rowY = boxY0 + i * rowH;
            // Text color: selected = MC yellow (0xFFFFFF55), others gray (MC 0xFFAAAAAA).
            const uint32_t color = (idx == m_suggestionIndex)
                ? 0xFFFFFF55u
                : 0xFFAAAAAAu;
            // 1px gutter from box edge; +2 to match MC's CommandSuggestions
            // text indent inside the box.
            graphics.DrawString(text, anchorX, rowY + 2, color, /*dropShadow=*/true);
        }
    }

    // MC CommandSuggestions.mouseScrolled → SuggestionsList.mouseScrolled:
    // the list's offset moves one row per notch (up = earlier entries); the
    // selection stays where it is.
    //
    // Speed-sensitive: the wheel's actual delta (fractional and accelerated
    // on macOS trackpads and mice) accumulates, so a slow scroll moves one
    // row at a time; and wheel frames arriving in quick succession multiply
    // it (up to x4 for frames under ~16 ms apart), so a fast flick crosses a
    // long list. Only this list scrolls this way.
    void ChatScreen::OnMouseScrolled(double deltaY) {
        if (!m_open || !m_suggestionsOpen || m_suggestions.empty() || deltaY == 0.0) return;
        const long long now = NowMillis();
        const long long gap = now - m_suggestionScrollLastMs;
        m_suggestionScrollLastMs = now;
        // A pause or a change of direction starts over.
        if (gap > 250 || (m_suggestionScrollAccum != 0.0 && (deltaY > 0.0) != (m_suggestionScrollAccum > 0.0))) {
            m_suggestionScrollAccum = 0.0;
        }
        constexpr double kFastGapMs = 80.0;
        const double velocity = gap < kFastGapMs
            ? 1.0 + 3.0 * (kFastGapMs - static_cast<double>(std::max(gap, 16LL))) / (kFastGapMs - 16.0)
            : 1.0;
        m_suggestionScrollAccum += deltaY * velocity;
        const int rows = static_cast<int>(m_suggestionScrollAccum);   // toward zero
        if (rows == 0) return;
        m_suggestionScrollAccum -= rows;
        const int maxScroll = std::max(0, static_cast<int>(m_suggestions.size()) - MAX_VISIBLE_SUGGESTIONS);
        // Wheel up (positive) shows earlier entries.
        m_suggestionScroll = std::clamp(m_suggestionScroll - rows, 0, maxScroll);
    }

    int ChatScreen::HistoryLift() const {
        if (!m_open) return 0;
        const auto hint = CommandSuggestions::Hint(m_inputText, m_suggestionsOpen ? m_suggestionAnchor : m_cursorPos);
        if (!hint || hint->size() <= 1) return 0;
        return static_cast<int>(hint->size() - 1) * HINT_LINE_STEP;
    }

    std::string ChatScreen::ConsumeSubmittedMessage() {
        std::string msg = m_submittedMessage;
        m_submittedMessage.clear();
        return msg;
    }

} // namespace Render
