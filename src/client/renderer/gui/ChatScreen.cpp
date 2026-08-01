// File: src/client/renderer/gui/ChatScreen.cpp
#include "ChatScreen.hpp"
#include "GuiGraphics.hpp"
#include "../../network/NetworkClient.hpp"
#include "../../entity/RemotePlayerManager.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace {
    // Wall-clock millis since some fixed epoch, mirroring Java's System.currentTimeMillis() /
    // Util.getMillis() that MC uses in EditBox.renderWidget for cursor blinking.
    long long NowMillis() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // ── Tab-completion provider (MC's ClientSuggestionProvider) ────────
    //
    // Returns the suggestions for the word AT the cursor in `text`, plus
    // the index where that word starts (anchor). Anchor + the existing
    // cursor position bracket the substring to REPLACE when applying.

    bool StartsWithIgnoreCase(const std::string& haystack, const std::string& needle) {
        if (needle.size() > haystack.size()) return false;
        for (size_t i = 0; i < needle.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(haystack[i])) !=
                std::tolower(static_cast<unsigned char>(needle[i]))) {
                return false;
            }
        }
        return true;
    }

    // List every known player name (self + remotes). Sorted alphabetically
    // case-insensitive (MC's getOnlinePlayerNames() returns players in the
    // order they're listed in the tab list, but vanilla also alphabetises
    // for the suggestion popup so the output is deterministic).
    std::vector<std::string> CollectPlayerNames() {
        std::vector<std::string> names;
        if (Client::g_networkClient) {
            const std::string& self = Client::g_networkClient->GetPlayerName();
            if (!self.empty()) names.push_back(self);
        }
        if (Client::g_remotePlayerManager) {
            for (const auto& [id, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                if (!rp.name.empty()) names.push_back(rp.name);
            }
        }
        std::sort(names.begin(), names.end(),
                  [](const std::string& a, const std::string& b) {
                      for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
                          int ca = std::tolower(static_cast<unsigned char>(a[i]));
                          int cb = std::tolower(static_cast<unsigned char>(b[i]));
                          if (ca != cb) return ca < cb;
                      }
                      return a.size() < b.size();
                  });
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    // Find the start of the word containing `cursor`. A word boundary is
    // a space (matches MC's CommandSuggestions which splits on whitespace).
    int FindWordStart(const std::string& text, int cursor) {
        int i = cursor;
        while (i > 0 && text[i - 1] != ' ') --i;
        return i;
    }

    // Tokenise the text from start up to `cursor` (so we know which arg
    // we're completing). The first token (after the leading '/') is the
    // command name; everything else is its args.
    std::vector<std::string> TokenizeBeforeCursor(const std::string& text, int cursor) {
        std::vector<std::string> out;
        std::string cur;
        for (int i = 0; i < cursor && i < (int)text.size(); ++i) {
            char c = text[i];
            if (c == ' ') {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    // The full suggestion-provider entry point. Returns the candidate
    // list filtered by the partial word at the cursor, AND sets `anchor`
    // to the index of where to start the replacement.
    std::vector<std::string> ComputeSuggestionsFor(const std::string& text,
                                                   int cursor,
                                                   int& anchorOut) {
        anchorOut = FindWordStart(text, cursor);

        // Not in command mode? No suggestions (MC's @-mention path isn't
        // wired yet).
        if (text.empty() || text[0] != '/') return {};

        // Tokenise up to the cursor to figure out which arg we're in.
        auto tokens = TokenizeBeforeCursor(text, cursor);
        // tokens[0] still includes the leading '/' here.

        // Are we completing the FIRST word (the command name)? That is,
        // the cursor sits inside or right after the '/foo' token and
        // there are no positional args yet.
        //
        // Cases:
        //   text="/"     tokens=["/"]      cursor=1  → command name
        //   text="/t"    tokens=["/t"]     cursor=2  → command name
        //   text="/tp "  tokens=["/tp"]    cursor=4  → arg 1 (after space)
        const bool completingCommandName =
            (tokens.size() <= 1) && (cursor <= (int)(tokens.empty() ? 0 : tokens[0].size()));

        std::string partial;
        int argIndex;

        if (completingCommandName) {
            // MC's command popup: bare names WITHOUT the leading '/'.
            // Anchor right after the slash so the apply replaces only
            // the partial command and leaves the '/' in place.
            anchorOut = std::min(cursor, 1);
            partial = text.substr(anchorOut, cursor - anchorOut);
            argIndex = 0;
        } else {
            partial = text.substr(anchorOut, cursor - anchorOut);
            // argIndex: 0 = command name, 1+ = positional args after it.
            if (!partial.empty() && !tokens.empty()) {
                argIndex = static_cast<int>(tokens.size()) - 1;
            } else {
                argIndex = static_cast<int>(tokens.size());
            }
        }

        std::vector<std::string> candidates;
        if (argIndex == 0) {
            // BARE command names (no '/'), MC-style. The '/' is already
            // in the input field and stays put when applying.
            candidates = {"tp", "teleport", "kick", "gamemode", "kill", "time", "gamerule"};
        } else {
            // Command name without the leading '/' for matching.
            std::string cmd = tokens.empty() ? "" : tokens[0];
            if (!cmd.empty() && cmd[0] == '/') cmd.erase(0, 1);
            for (auto& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (cmd == "tp" || cmd == "teleport") {
                // /tp <player>, /tp <player> <player>, /tp <x> <y> <z>
                if (argIndex == 1 || argIndex == 2) {
                    candidates = CollectPlayerNames();
                    // Also offer "~" for coordinates on arg 1..3 of /tp
                    // (MC offers the relative-coord tilde here).
                    candidates.push_back("~");
                } else if (argIndex == 3) {
                    candidates = {"~"};
                }
            } else if (cmd == "kick") {
                if (argIndex == 1) {
                    candidates = CollectPlayerNames();
                }
            } else if (cmd == "gamemode") {
                // /gamemode <mode> [player]
                if (argIndex == 1) {
                    candidates = {"survival", "creative", "adventure", "spectator"};
                } else if (argIndex == 2) {
                    candidates = CollectPlayerNames();
                }
            } else if (cmd == "kill") {
                if (argIndex == 1) {
                    candidates = CollectPlayerNames();
                }
            } else if (cmd == "time") {
                // /time <set|add|query> <value>
                if (argIndex == 1) {
                    candidates = {"set", "add", "query"};
                } else if (argIndex == 2) {
                    const std::string sub = tokens.size() > 1 ? tokens[1] : "";
                    if (sub == "query") {
                        candidates = {"daytime", "gametime", "day"};
                    } else if (sub == "set") {
                        candidates = {"day", "noon", "night", "midnight"};
                    }
                }
            } else if (cmd == "gamerule") {
                // /gamerule <rule> [true|false]
                if (argIndex == 1) {
                    candidates = {"doDaylightCycle"};
                } else if (argIndex == 2) {
                    candidates = {"true", "false"};
                }
            }
        }

        // Filter by partial prefix (case-insensitive).
        std::vector<std::string> filtered;
        for (const auto& c : candidates) {
            if (partial.empty() || StartsWithIgnoreCase(c, partial)) {
                filtered.push_back(c);
            }
        }
        return filtered;
    }
}

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
            // MC: typing closes the suggestion popup. The user types TAB
            // again to re-open with the new word.
            CloseSuggestions();
        }
    }

    void ChatScreen::OpenSuggestions() {
        int anchor = 0;
        auto sugg = ComputeSuggestionsFor(m_inputText, m_cursorPos, anchor);
        if (sugg.empty()) {
            CloseSuggestions();
            return;
        }
        m_suggestions      = std::move(sugg);
        m_suggestionAnchor = anchor;
        m_suggestionIndex  = 0;
        m_suggestionsOpen  = true;
    }

    void ChatScreen::CloseSuggestions() {
        m_suggestionsOpen = false;
        m_suggestions.clear();
        m_suggestionIndex = 0;
    }

    void ChatScreen::CycleSuggestion(int delta) {
        if (!m_suggestionsOpen || m_suggestions.empty()) return;
        const int n = static_cast<int>(m_suggestions.size());
        m_suggestionIndex = ((m_suggestionIndex + delta) % n + n) % n;
    }

    void ChatScreen::ApplySelectedSuggestion() {
        if (!m_suggestionsOpen || m_suggestions.empty()) return;
        if (m_suggestionIndex < 0 || m_suggestionIndex >= (int)m_suggestions.size()) return;

        const std::string& sel = m_suggestions[m_suggestionIndex];
        // Replace [m_suggestionAnchor, m_cursorPos) with sel, place caret
        // after the inserted text (MC: SuggestionsList.useSuggestion does
        // the same — splice + advance cursor).
        m_inputText.replace(m_suggestionAnchor,
                            m_cursorPos - m_suggestionAnchor,
                            sel);
        SetCursorPosition(m_suggestionAnchor + static_cast<int>(sel.size()));
        CloseSuggestions();
    }

    bool ChatScreen::OnKeyDown(int glfwKey) {
        if (!m_open) return false;

        // TAB — MC's CommandSuggestions: if popup is closed, open it; if
        // it's open, apply the highlighted suggestion. SHIFT+TAB isn't
        // distinct in MC, but plain TAB is the universal trigger.
        if (glfwKey == GLFW_KEY_TAB) {
            if (m_suggestionsOpen) ApplySelectedSuggestion();
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
            Close();
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
            // MC: EditBox.deleteText(-1) — remove the char before the cursor and step back.
            if (m_cursorPos > 0) {
                m_inputText.erase(m_inputText.begin() + (m_cursorPos - 1));
                SetCursorPosition(m_cursorPos - 1);
            }
            CloseSuggestions();
            return true;
        }

        if (glfwKey == GLFW_KEY_DELETE) {
            // MC: EditBox.deleteText(+1) — remove the char at the cursor.
            if (m_cursorPos < static_cast<int>(m_inputText.size())) {
                m_inputText.erase(m_inputText.begin() + m_cursorPos);
                ResetCursorBlink();
            }
            CloseSuggestions();
            return true;
        }

        if (glfwKey == GLFW_KEY_LEFT)  { CloseSuggestions(); MoveCursor(-1); return true; }
        if (glfwKey == GLFW_KEY_RIGHT) { CloseSuggestions(); MoveCursor(+1); return true; }
        if (glfwKey == GLFW_KEY_HOME)  { CloseSuggestions(); MoveCursorToStart(); return true; }
        if (glfwKey == GLFW_KEY_END)   { CloseSuggestions(); MoveCursorToEnd();   return true; }

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

        int guiWidth = graphics.GuiWidth();
        int guiHeight = graphics.GuiHeight();

        // Dark background bar at bottom (MC: full width, 12px tall)
        int inputY = guiHeight - INPUT_HEIGHT - 2;
        const int inputX = 4;
        graphics.Fill(0, inputY, guiWidth, guiHeight, 0x80000000);

        // Render the full input text (no trailing underscore — cursor is drawn separately)
        const int textY = inputY + 2;
        graphics.DrawString(m_inputText, inputX, textY, 0xFFFFFFFF, true);

        // Cursor — matches MC's EditBox.renderWidget (lines 411-458):
        //   MC line 415:   drawX += font.width(text_before) + 1;       // +1 = inter-char gap
        //   MC line 422-4: if insert (mid-text):  cursorX = drawX - 1; // bar drawn between glyphs
        //                  else (at end of text): cursorX = drawX;     // underscore one gap past last char
        //
        // Effectively:
        //   - At end of text: underscore at  text_start + width(before) + 1
        //   - Mid-text:       vertical bar at text_start + width(before) + 1 - 1 = + 0 above
        //                     (which is what GetStringWidth(before) already gives us)
        if (ShouldShowCursor()) {
            std::string before = m_inputText.substr(0, m_cursorPos);
            int beforeWidth = graphics.GetStringWidth(before);
            const bool atEnd = (m_cursorPos >= static_cast<int>(m_inputText.size()));
            if (atEnd) {
                int underscoreX = inputX + beforeWidth + 1;
                graphics.DrawString("_", underscoreX, textY, 0xFFFFFFFF, true);
            } else {
                int barX = inputX + beforeWidth;
                graphics.Fill(barX, textY - 1, barX + 1, textY + 1 + 9, 0xFFFFFFFF);
            }
        }

        // Suggestion popup is drawn LAST so it sits on top of everything.
        RenderSuggestions(graphics, inputX, inputY);
    }

    void ChatScreen::RenderSuggestions(GuiGraphics& graphics, int inputX, int inputY) {
        if (!m_suggestionsOpen || m_suggestions.empty()) return;

        // X anchor: align the popup's left edge to the start of the word
        // being completed (MC's CommandSuggestions.render). That's:
        //   inputX + width(prefix-before-anchor)
        const std::string before = m_inputText.substr(0, m_suggestionAnchor);
        const int anchorX = inputX + graphics.GetStringWidth(before);

        // Width: max suggestion width + 2px horizontal padding (MC uses 1
        // px padding either side, total 2 in interior).
        int maxW = 0;
        for (const auto& s : m_suggestions) {
            maxW = std::max(maxW, graphics.GetStringWidth(s));
        }
        const int boxX0 = anchorX - 1;            // 1px left padding
        const int boxX1 = anchorX + maxW + 1;     // 1px right padding

        // Height + scroll window: up to MAX_VISIBLE_SUGGESTIONS rows of
        // 12px each. If there are more entries than fit, scroll so the
        // selected index stays in view (MC: SuggestionsList.scroll).
        const int total = static_cast<int>(m_suggestions.size());
        const int visible = std::min(total, MAX_VISIBLE_SUGGESTIONS);
        int scrollStart = 0;
        if (total > MAX_VISIBLE_SUGGESTIONS) {
            // Keep selected centred-ish in the visible window.
            scrollStart = m_suggestionIndex - MAX_VISIBLE_SUGGESTIONS / 2;
            if (scrollStart < 0) scrollStart = 0;
            if (scrollStart + MAX_VISIBLE_SUGGESTIONS > total) {
                scrollStart = total - MAX_VISIBLE_SUGGESTIONS;
            }
        }
        const int rowH = 12;
        const int boxH = visible * rowH;
        // MC positions the popup ABOVE the input. Bottom of popup sits
        // at inputY - 1 (1px gap), top at boxY0.
        const int boxY1 = inputY - 1;
        const int boxY0 = boxY1 - boxH;

        // Box background — MC uses 0xD0000000 (~82% black). Drawn under
        // every row at once.
        graphics.Fill(boxX0, boxY0, boxX1, boxY1, 0xD0000000);

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

    std::string ChatScreen::ConsumeSubmittedMessage() {
        std::string msg = m_submittedMessage;
        m_submittedMessage.clear();
        return msg;
    }

} // namespace Render
