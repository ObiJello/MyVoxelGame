// File: src/client/renderer/gui/AnvilScreen.cpp
#include "AnvilScreen.hpp"
#include "GuiGraphics.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture
#include "client/entity/Player.hpp"
#include "client/input/Input.hpp"      // clipboard
#include "common/data/DataComponents.hpp"
#include "common/inventory/UtilityMenus.hpp"
#include "common/text/Language.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <string>

namespace Render {

    namespace {
        std::deque<Network::RenameItemC2SPacket> s_renames;

        long long NowMillis() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        // MC StringUtil.isAllowedChatCharacter, over the printable ASCII the
        // engine's font and char queue carry (the other text inputs draw the
        // same line).
        bool IsAllowedCharacter(unsigned int codepoint) {
            return codepoint >= 32 && codepoint < 127;
        }

        std::string FilterAllowed(const std::string& text) {
            std::string out;
            for (char c : text) {
                if (IsAllowedCharacter(static_cast<unsigned char>(c))) out += c;
            }
            return out;
        }

        // MC Font.plainSubstrByWidth: the longest prefix of `text` no wider
        // than `width`.
        size_t FitPrefix(GuiGraphics& g, const std::string& text, int width) {
            size_t n = 0;
            while (n < text.size() && g.GetStringWidth(text.substr(0, n + 1)) <= width) ++n;
            return n;
        }
    }

    AnvilScreen& GetAnvilScreen() {
        static AnvilScreen s;
        return s;
    }

    bool ConsumeRenameItem(Network::RenameItemC2SPacket& out) {
        if (s_renames.empty()) return false;
        out = std::move(s_renames.front());
        s_renames.pop_front();
        return true;
    }

    void AnvilScreen::Configure(const std::string& /*title*/) {
        // MC AnvilBlock.CONTAINER_TITLE is always "container.repair" — the
        // block's own name (what the server sends for every block menu) is
        // not what the anvil shows.
        m_title = Game::Language::GetOrDefault("container.repair", "Repair & Name");
    }

    Game::AnvilMenu* AnvilScreen::Anvil() const {
        return dynamic_cast<Game::AnvilMenu*>(Menu());
    }

    TextureHandle AnvilScreen::EnsureBackground() {
        if (m_backgroundTried) return m_background;
        m_backgroundTried = true;
        int w = 0, h = 0;
        m_background = LoadStandaloneGuiTexture("assets/textures/gui/container/anvil.png", w, h);
        return m_background;
    }

    bool AnvilScreen::NameEditable() const {
        const Game::AnvilMenu* anvil = Anvil();
        return anvil && anvil->GetSlot(0).HasItem();
    }

    void AnvilScreen::OnOpen() {
        // MC subInit: name.setValue(""), editable iff slot 0 has an item —
        // the first ContainerTick syncs it to whatever slot 0 then holds.
        m_value.clear();
        // Names typed into a previous anvil that never got drained (the
        // screen closed the same frame) belong to that menu, not this one.
        s_renames.clear();
        m_cursorPos = m_highlightPos = m_displayPos = 0;
        m_focusedAtMillis = NowMillis();
        m_lastInput = Game::ItemStack{};
        m_inputSeen = false;
    }

    void AnvilScreen::ContainerTick() {
        Game::AnvilMenu* anvil = Anvil();
        if (!anvil) return;
        // The result slot's mayPickup reads the level — both for the cost
        // colour below and for the click the client predicts.
        if (const Game::ClientPlayer* player = Player()) {
            anvil->SetPlayerLevel(player->xpLevel);
            anvil->creative = player->IsCreative();
        }
        const Game::ItemStack& input = anvil->GetSlot(0).GetItem();
        const bool same = m_inputSeen && input.count == m_lastInput.count &&
                          Game::IsSameItemSameComponents(input, m_lastInput);
        if (!same) SyncNameToInput();
    }

    void AnvilScreen::SyncNameToInput() {
        Game::AnvilMenu* anvil = Anvil();
        if (!anvil) return;
        const Game::ItemStack& input = anvil->GetSlot(0).GetItem();
        m_lastInput = input;
        m_inputSeen = true;
        // name.setValue(empty ? "" : hoverName); setEditable; setFocused.
        SetValue(input.IsEmpty() ? std::string{} : Game::GetItemStackHoverName(input));
    }

    void AnvilScreen::OnNameChanged() {
        Game::AnvilMenu* anvil = Anvil();
        if (!anvil || !anvil->GetSlot(0).HasItem()) return;
        // An unnamed item whose box still reads its default name is not
        // being renamed — "" tells the menu so (and keeps the cost at 0).
        const Game::ItemStack& input = anvil->GetSlot(0).GetItem();
        std::string newName = m_value;
        if (!input.components.has(Game::DataComponents::CUSTOM_NAME) &&
            newName == Game::GetItemStackHoverName(input)) {
            newName.clear();
        }
        if (anvil->SetItemName(newName)) {
            s_renames.push_back(Network::RenameItemC2SPacket{newName});
        }
    }

    // ── EditBox ───────────────────────────────────────────────────────────

    void AnvilScreen::SetValue(const std::string& value) {
        // MC EditBox.setValue: truncate to maxLength, cursor to the end with
        // no selection, then the responder.
        m_value = value.substr(0, static_cast<size_t>(Game::AnvilMenu::MAX_NAME_LENGTH));
        m_displayPos = 0;
        MoveCursorTo(static_cast<int>(m_value.size()), false);
        OnNameChanged();
    }

    void AnvilScreen::MoveCursorTo(int pos, bool extendSelection) {
        m_cursorPos = std::clamp(pos, 0, static_cast<int>(m_value.size()));
        if (!extendSelection) m_highlightPos = m_cursorPos;
        m_focusedAtMillis = NowMillis();
    }

    void AnvilScreen::SelectAll() {
        MoveCursorTo(static_cast<int>(m_value.size()), false);
        m_highlightPos = 0;
    }

    std::string AnvilScreen::SelectedText() const {
        const int s0 = std::min(m_cursorPos, m_highlightPos);
        const int s1 = std::max(m_cursorPos, m_highlightPos);
        return m_value.substr(static_cast<size_t>(s0), static_cast<size_t>(s1 - s0));
    }

    void AnvilScreen::InsertText(const std::string& input) {
        // MC EditBox.insertText: the selection (or the empty span at the
        // cursor) is replaced by as much of the filtered text as fits.
        const int start = std::min(m_cursorPos, m_highlightPos);
        const int end   = std::max(m_cursorPos, m_highlightPos);
        const int room  = Game::AnvilMenu::MAX_NAME_LENGTH - static_cast<int>(m_value.size()) + (end - start);
        if (room <= 0) return;
        const std::string text = FilterAllowed(input).substr(0, static_cast<size_t>(room));
        m_value.replace(static_cast<size_t>(start), static_cast<size_t>(end - start), text);
        MoveCursorTo(start + static_cast<int>(text.size()), false);
        OnNameChanged();
    }

    void AnvilScreen::DeleteChars(int dir) {
        // MC EditBox.deleteChars: a selection goes whole, otherwise one
        // character in `dir`.
        if (m_value.empty()) return;
        if (m_highlightPos != m_cursorPos) {
            InsertText("");
            return;
        }
        const int pos   = std::clamp(m_cursorPos + dir, 0, static_cast<int>(m_value.size()));
        const int start = std::min(pos, m_cursorPos);
        const int end   = std::max(pos, m_cursorPos);
        if (start == end) return;
        m_value.erase(static_cast<size_t>(start), static_cast<size_t>(end - start));
        MoveCursorTo(start, false);
        OnNameChanged();
    }

    void AnvilScreen::ScrollTo(GuiGraphics* g, int pos) {
        // MC EditBox.scrollTo: slide the visible window just far enough that
        // `pos` is inside it.
        const int length = static_cast<int>(m_value.size());
        m_displayPos = std::min(m_displayPos, length);
        if (!g) return;
        const size_t shown = FitPrefix(*g, m_value.substr(static_cast<size_t>(m_displayPos)), NAME_W);
        const int lastPos = static_cast<int>(shown) + m_displayPos;
        if (pos > lastPos) m_displayPos += pos - lastPos;
        else if (pos < m_displayPos) m_displayPos = pos;
        m_displayPos = std::clamp(m_displayPos, 0, length);
    }

    // ── Input ─────────────────────────────────────────────────────────────

    int AnvilScreen::HitTestExtras(int lx, int ly) {
        if (lx >= NAME_X && lx < NAME_X + NAME_W && ly >= NAME_Y && ly < NAME_Y + NAME_H) {
            return HIT_NAME_FIELD;
        }
        return HIT_NONE;
    }

    bool AnvilScreen::HandleExtraClick(int hit, int glfwButton, bool shift) {
        if (hit != HIT_NAME_FIELD || glfwButton != GLFW_MOUSE_BUTTON_LEFT) return false;
        if (!NameEditable()) return true;
        // MC EditBox.onClick: the cursor goes to the character under the
        // mouse (shift extends the selection).
        // Resolved to a character at the next draw, where the font is.
        m_nameClickX     = static_cast<int>(MouseGui().x);
        m_nameClickShift = shift;
        m_nameClickPending = true;
        return true;
    }

    bool AnvilScreen::HandleExtraKey(int glfwKey, int glfwMods) {
        // MC AnvilScreen.keyPressed: an editable, focused box takes every key
        // (ESC was handled before this); an uneditable one takes none.
        if (!NameEditable()) return false;
        const bool shift   = (glfwMods & GLFW_MOD_SHIFT) != 0;
        const bool command = (glfwMods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) != 0;
        if (command) {
            switch (glfwKey) {
                case GLFW_KEY_A: SelectAll(); return true;
                case GLFW_KEY_C: Input::SetClipboardText(SelectedText()); return true;
                case GLFW_KEY_V: InsertText(Input::GetClipboardText()); return true;
                case GLFW_KEY_X:
                    Input::SetClipboardText(SelectedText());
                    InsertText("");
                    return true;
                default: break;
            }
        }
        switch (glfwKey) {
            case GLFW_KEY_BACKSPACE: DeleteChars(-1); break;
            case GLFW_KEY_DELETE:    DeleteChars(1);  break;
            case GLFW_KEY_LEFT:      MoveCursorTo(m_cursorPos - 1, shift); break;
            case GLFW_KEY_RIGHT:     MoveCursorTo(m_cursorPos + 1, shift); break;
            case GLFW_KEY_HOME:      MoveCursorTo(0, shift); break;
            case GLFW_KEY_END:       MoveCursorTo(static_cast<int>(m_value.size()), shift); break;
            default: break;
        }
        return true;
    }

    bool AnvilScreen::HandleExtraCharInput(unsigned int codepoint) {
        if (!NameEditable()) return false;
        if (IsAllowedCharacter(codepoint)) InsertText(std::string(1, static_cast<char>(codepoint)));
        return true;
    }

    // ── Drawing ───────────────────────────────────────────────────────────

    void AnvilScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        const TextureHandle bg = EnsureBackground();
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
        } else {
            constexpr float SHEET = 256.0f;
            g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
                   0.0f, 0.0f, static_cast<float>(IMAGE_W) / SHEET, static_cast<float>(IMAGE_H) / SHEET);
        }
        // AnvilScreen.extractBackground: the name field, lit while slot 0
        // holds something.
        g.BlitSprite(NameEditable() ? "container/anvil/text_field" : "container/anvil/text_field_disabled",
                     leftPos + FIELD_X, topPos + FIELD_Y, FIELD_W, FIELD_H);
        // extractErrorIcon: inputs present but no result.
        const Game::AnvilMenu* anvil = Anvil();
        if (anvil && (anvil->GetSlot(0).HasItem() || anvil->GetSlot(1).HasItem()) &&
            !anvil->GetSlot(anvil->ResultSlotIndex()).HasItem()) {
            g.BlitSprite("container/anvil/error", leftPos + ERROR_X, topPos + ERROR_Y, ERROR_W, ERROR_H);
        }
    }

    void AnvilScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        g.DrawString(m_title, leftPos + TITLE_X, topPos + TITLE_Y, LABEL_COLOR, false);
        g.DrawString(Game::Language::GetOrDefault("container.inventory", "Inventory"),
                     leftPos + INV_LABEL_X, topPos + INV_LABEL_Y, LABEL_COLOR, false);

        // AnvilScreen.extractLabels: the cost line.
        const Game::AnvilMenu* anvil = Anvil();
        if (!anvil) return;
        const int cost = anvil->GetCost();
        if (cost <= 0) return;
        const bool creative = Player() && Player()->IsCreative();
        std::string line;
        uint32_t color = COST_COLOR;
        if (cost >= Game::AnvilMenu::TOO_EXPENSIVE_COST && !creative) {
            line  = Game::Language::GetOrDefault("container.repair.expensive", "Too Expensive!");
            color = COST_ERROR_COLOR;
        } else if (!anvil->GetSlot(anvil->ResultSlotIndex()).HasItem()) {
            return;
        } else {
            std::string pattern = Game::Language::GetOrDefault("container.repair.cost", "Enchantment Cost: %1$s");
            const std::string arg = std::to_string(cost);
            for (const char* token : {"%1$s", "%s"}) {
                const size_t at = pattern.find(token);
                if (at != std::string::npos) {
                    pattern.replace(at, std::string(token).size(), arg);
                    break;
                }
            }
            line = pattern;
            if (!anvil->GetSlot(anvil->ResultSlotIndex()).MayPickup()) color = COST_ERROR_COLOR;
        }
        const int tx = IMAGE_W - 8 - g.GetStringWidth(line) - 2;
        g.Fill(leftPos + tx - 2, topPos + COST_STRIP_Y0, leftPos + IMAGE_W - 8, topPos + COST_STRIP_Y1,
               COST_STRIP_COLOR);
        g.DrawString(line, leftPos + tx, topPos + COST_TEXT_Y, color, true);
    }

    void AnvilScreen::RenderExtras(GuiGraphics& g, int leftPos, int topPos) {
        RenderNameBox(g, leftPos, topPos);
    }

    void AnvilScreen::RenderNameBox(GuiGraphics& g, int leftPos, int topPos) {
        const int x = leftPos + NAME_X;
        // Unbordered EditBox: text at the box's left edge, centred in its 12
        // pixels ((height - 8) / 2).
        const int y = topPos + NAME_Y + (NAME_H - 8) / 2;
        const bool editable = NameEditable();

        // A click in the box resolves here, where the font is at hand.
        if (m_nameClickPending) {
            m_nameClickPending = false;
            if (editable) {
                const std::string shown = m_value.substr(static_cast<size_t>(m_displayPos));
                const int rel = m_nameClickX - x;
                const int pos = static_cast<int>(FitPrefix(g, shown, std::max(0, rel))) + m_displayPos;
                MoveCursorTo(pos, m_nameClickShift);
            }
        }
        ScrollTo(&g, m_cursorPos);

        const std::string visible = m_value.substr(static_cast<size_t>(m_displayPos));
        const std::string shown = visible.substr(0, FitPrefix(g, visible, NAME_W));
        // setTextColor(-1) / setTextColorUneditable(-1): white either way.
        if (!shown.empty()) g.DrawString(shown, x, y, 0xFFFFFFFF, true);
        if (!editable) return;

        const int cursorInShown = m_cursorPos - m_displayPos;
        const bool cursorVisible = cursorInShown >= 0 && cursorInShown <= static_cast<int>(shown.size());

        // Selection: invertHighlightedTextColor(false) — the blue highlight
        // quad under white text, painted the way the creative search box
        // paints the same effect.
        if (m_highlightPos != m_cursorPos) {
            const int s0 = std::clamp(std::min(m_cursorPos, m_highlightPos) - m_displayPos, 0,
                                      static_cast<int>(shown.size()));
            const int s1 = std::clamp(std::max(m_cursorPos, m_highlightPos) - m_displayPos, 0,
                                      static_cast<int>(shown.size()));
            if (s1 > s0) {
                const int x0 = x + g.GetStringWidth(shown.substr(0, static_cast<size_t>(s0)));
                const int x1 = x + g.GetStringWidth(shown.substr(0, static_cast<size_t>(s1)));
                const std::string selected = shown.substr(static_cast<size_t>(s0), static_cast<size_t>(s1 - s0));
                g.Fill(x0, y - 1, x1, y + 9, 0xFF8B8BFF);
                g.DrawString(selected, x0 + 1, y + 1, 0xFF3E3EFF, false);
                g.DrawString(selected, x0, y, 0xFFFFFFFF, false);
            }
        }

        // Caret: 300 ms on / off from the last edit (EditBox.renderWidget);
        // "_" at the end of the text, a bar inside it.
        long long elapsed = NowMillis() - m_focusedAtMillis;
        if (elapsed < 0) elapsed = 0;
        if (!cursorVisible || ((elapsed / 300LL) % 2LL) != 0LL) return;
        const int caretX = x + g.GetStringWidth(shown.substr(0, static_cast<size_t>(cursorInShown)));
        if (m_cursorPos >= static_cast<int>(m_value.size())) {
            g.DrawString("_", caretX, y, 0xFFFFFFFF, true);
        } else {
            g.Fill(caretX, y - 1, caretX + 1, y + 1 + 9, 0xFFFFFFFF);
        }
    }

} // namespace Render
