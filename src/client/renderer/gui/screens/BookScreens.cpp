// File: src/client/renderer/gui/screens/BookScreens.cpp
//
// MC BookViewScreen / LecternScreen / BookEditScreen / BookSignScreen,
// PageButton, MultiLineEditBox + MultilineTextField, and the slice of
// StringSplitter / Font the book pages are laid out with. See BookScreens.hpp.
#include "BookScreens.hpp"

#include "../AbstractContainerScreen.hpp"
#include "../ChatComponent.hpp"
#include "../FontRenderer.hpp"
#include "../GuiGraphics.hpp"
#include "../../backend/RenderBackend.hpp"
#include "client/input/Input.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/sound/ClientSounds.hpp"
#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/Item.hpp"
#include "common/inventory/InventoryMenu.hpp"
#include "common/inventory/LecternMenu.hpp"
#include "common/text/Language.hpp"
#include "common/text/TextComponent.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace Render {

    namespace {

        using Game::Text::Component;
        using Game::Text::Style;

        // ── Layout constants (BookViewScreen / BookEditScreen) ──────────────
        constexpr int kImageWidth        = 192;
        constexpr int kImageHeight       = 192;
        constexpr int kTextureSize       = 256;
        constexpr int kTextWidth         = 114;
        constexpr int kViewTextHeight    = 128;    // BookViewScreen.TEXT_HEIGHT
        constexpr int kEditTextHeight    = 126;    // BookEditScreen.TEXT_HEIGHT
        constexpr int kLineHeight        = 9;      // Font.lineHeight
        constexpr int kPageTextX         = 36;
        constexpr int kPageTextY         = 30;
        constexpr int kPageIndicatorX    = 148;
        constexpr int kPageIndicatorY    = 16;
        constexpr int kPageButtonY       = 157;
        constexpr int kPageBackX         = 43;
        constexpr int kPageForwardX      = 116;
        constexpr int kMenuButtonSize    = 98;
        constexpr uint32_t kBlack        = 0xFF000000u;   // -16777216
        constexpr uint32_t kDarkGray     = 0xFF555555u;   // ChatFormatting.DARK_GRAY

        int BackgroundLeft(int width) { return (width - kImageWidth) / 2; }
        constexpr int BackgroundTop() { return 2; }
        constexpr int MenuControlsTop() { return BackgroundTop() + kImageHeight + 2; }

        double NowMillis() {
            using namespace std::chrono;
            return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        }

        // TextCursorUtils.isCursorVisible.
        bool CursorVisible(double sinceMs) {
            return static_cast<long long>(sinceMs) / 300 % 2 == 0;
        }

        // ── Outbound queues (drained by PlatformMain) ───────────────────────
        std::deque<Network::EditBookC2SPacket>             s_editBooks;
        std::deque<Network::ContainerButtonClickC2SPacket> s_buttonClicks;
        bool                                               s_containerClose = false;

        // ── Font metrics — MC's Font over this engine's ascii atlas ─────────
        // The renderer's font, captured from the GuiGraphics a screen draws
        // with. Every book screen renders before it lays anything out.
        const FontRenderer* s_font = nullptr;

        // MC GlyphInfo.getAdvance(bold): a bitmap glyph advances its width +
        // 1, one more when bold; the space provider's space advances 4 (5
        // bold). A character outside the atlas takes the renderer's blank
        // 4-pixel cell.
        int CharAdvance(unsigned char c, bool bold) {
            if (c == ' ') return 4 + (bold ? 1 : 0);
            if (c < 32 || c > 126) return 4;
            const int glyph = s_font ? s_font->GetCharWidth(c) : 5;
            return glyph + 1 + (bold ? 1 : 0);
        }

        // Length in bytes of the UTF-8 sequence starting at s[i].
        size_t Utf8Length(const std::string& s, size_t i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            size_t n = 1;
            if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
            return std::min(n, s.size() - i);
        }

        // MC Font.width(String) for plain text.
        int PlainWidth(const std::string& s) {
            int w = 0;
            for (size_t i = 0; i < s.size(); i += Utf8Length(s, i)) {
                w += CharAdvance(static_cast<unsigned char>(s[i]), false);
            }
            return w;
        }

        // MC Font.plainSubstrByWidth: the longest prefix no wider than
        // `maxWidth` (StringSplitter.WidthLimitedCharSink).
        size_t PlainSubstrByWidth(const std::string& s, int maxWidth) {
            int remaining = maxWidth;
            size_t i = 0;
            while (i < s.size()) {
                remaining -= CharAdvance(static_cast<unsigned char>(s[i]), false);
                if (remaining < 0) break;
                i += Utf8Length(s, i);
            }
            return i;
        }

        // Obfuscated text (§k / "obfuscated": true): MC draws a random glyph
        // of the same advance every frame (BakedGlyph's random from the same
        // width bucket).
        char ObfuscatedGlyph(unsigned char c) {
            static std::mt19937 rng{0x5EEDu};
            const int want = CharAdvance(c, false);
            std::array<char, 96> candidates{};
            size_t n = 0;
            for (int g = 33; g <= 126; ++g) {
                if (CharAdvance(static_cast<unsigned char>(g), false) == want) candidates[n++] = static_cast<char>(g);
            }
            if (n == 0) return static_cast<char>(c);
            return candidates[std::uniform_int_distribution<size_t>(0, n - 1)(rng)];
        }

        // Draws one run of text in one style at MC's advances — word by word,
        // so spaces take MC's 4 pixels rather than the atlas's 5. Returns the
        // advance. Underline / strikethrough are filled per glyph the way
        // StringRenderOutput adds its effects, spaces included.
        int DrawRun(GuiGraphics& g, const std::string& text, int x, int y, uint32_t color,
                    bool bold, bool underlined, bool strikethrough, bool obfuscated) {
            const std::string boldPrefix = bold ? "\xC2\xA7l" : "";
            int cursor = x;
            std::string word;
            int wordX = x;
            auto flush = [&] {
                if (!word.empty()) g.DrawString(boldPrefix + word, wordX, y, color, false);
                word.clear();
            };
            for (size_t i = 0; i < text.size(); i += Utf8Length(text, i)) {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                const int advance = CharAdvance(c, bold);
                if (c > 32 && c <= 126) {
                    if (word.empty()) wordX = cursor;
                    word.push_back(obfuscated ? ObfuscatedGlyph(c) : static_cast<char>(c));
                } else {
                    flush();   // a space or a glyph the atlas does not have
                }
                if (strikethrough) g.Fill(cursor - 1, y + 4, cursor + advance, y + 5, color);
                if (underlined)    g.Fill(cursor - 1, y + 9, cursor + advance, y + 10, color);
                cursor += advance;
            }
            flush();
            return cursor - x;
        }

        void DrawPlain(GuiGraphics& g, const std::string& text, int x, int y, uint32_t color) {
            DrawRun(g, text, x, y, color, false, false, false, false);
        }

        // ── Line breaking — MC StringSplitter.LineBreakFinder ───────────────

        struct BreakItem {
            int  width   = 0;
            bool newline = false;
            bool space   = false;
        };
        struct LineSpan { size_t begin; size_t end; };

        // Both MC splitters share the finder: break at a newline; past
        // `maxWidth` break at the last space seen (which is skipped), else
        // before the glyph that overflowed — never before the first glyph
        // with a width. `formattedTail` is the FormattedText variant's rule
        // that a text ending in a line break gets one more, empty, line.
        std::vector<LineSpan> BreakLines(const std::vector<BreakItem>& items, int maxWidth, bool formattedTail) {
            std::vector<LineSpan> lines;
            const float limit = static_cast<float>(std::max(maxWidth, 1));
            size_t start = 0;
            bool endedOnNewline = false;
            while (start < items.size()) {
                float width = 0.0f;
                long lastSpace = -1;
                bool hadNonZero = false;
                long lineBreak = -1;
                for (size_t i = start; i < items.size(); ++i) {
                    const BreakItem& it = items[i];
                    if (it.newline) { lineBreak = static_cast<long>(i); break; }
                    if (it.space) lastSpace = static_cast<long>(i);
                    width += static_cast<float>(it.width);
                    if (hadNonZero && width > limit) {
                        lineBreak = lastSpace != -1 ? lastSpace : static_cast<long>(i);
                        break;
                    }
                    hadNonZero |= it.width != 0;
                }
                if (lineBreak < 0) {
                    lines.push_back({start, items.size()});
                    endedOnNewline = false;
                    start = items.size();
                    break;
                }
                const BreakItem& tail = items[static_cast<size_t>(lineBreak)];
                lines.push_back({start, static_cast<size_t>(lineBreak)});
                endedOnNewline = tail.newline;
                start = static_cast<size_t>(lineBreak) + ((tail.newline || tail.space) ? 1 : 0);
            }
            if (formattedTail && endedOnNewline && start >= items.size()) {
                lines.push_back({items.size(), items.size()});
            }
            return lines;
        }

        // ── A page as styled glyphs (FormattedText.visit + the legacy '§'
        // codes StringDecomposer.iterateFormatted applies) ────────────────
        struct StyledGlyph {
            std::string text;       // one UTF-8 character
            uint16_t    style = 0;  // index into StyledPage::styles
        };

        struct StyledPage {
            std::vector<Style>       styles;
            std::vector<StyledGlyph> glyphs;
            std::vector<LineSpan>    lines;

            uint16_t Intern(const Style& s) {
                for (size_t i = 0; i < styles.size(); ++i) {
                    if (styles[i] == s) return static_cast<uint16_t>(i);
                }
                styles.push_back(s);
                return static_cast<uint16_t>(styles.size() - 1);
            }
        };

        StyledPage LayoutPage(const Component& page, const Style& base, int maxWidth) {
            StyledPage out;
            Game::Text::Visit(page, base, [&out](const Style& runStyle, std::string_view text) {
                Style style = runStyle;
                uint16_t index = out.Intern(style);
                const std::string s(text);
                for (size_t i = 0; i < s.size();) {
                    // '§' + code: a formatting change, never a glyph; RESET
                    // returns to the run's own style (Font's reset style).
                    if (static_cast<unsigned char>(s[i]) == 0xC2 && i + 1 < s.size() &&
                        static_cast<unsigned char>(s[i + 1]) == 0xA7) {
                        if (i + 2 >= s.size()) break;
                        if (auto f = Game::Text::FormattingByCode(s[i + 2])) {
                            style = *f == Game::Text::Formatting::Reset ? runStyle : style.ApplyLegacyFormat(*f);
                            index = out.Intern(style);
                        }
                        i += 3;
                        continue;
                    }
                    const size_t n = Utf8Length(s, i);
                    out.glyphs.push_back({s.substr(i, n), index});
                    i += n;
                }
                return true;
            });
            std::vector<BreakItem> items;
            items.reserve(out.glyphs.size());
            for (const StyledGlyph& gl : out.glyphs) {
                const unsigned char c = static_cast<unsigned char>(gl.text[0]);
                BreakItem it;
                it.newline = c == '\n';
                it.space = c == ' ';
                it.width = it.newline ? 0 : CharAdvance(c, out.styles[gl.style].IsBold());
                items.push_back(it);
            }
            out.lines = BreakLines(items, maxWidth, /*formattedTail=*/true);
            return out;
        }

        uint32_t ColorOf(const Style& style, uint32_t fallback) {
            return style.color ? (0xFF000000u | style.color->rgb) : fallback;
        }

        // Draws line `index`, one run per style change.
        void DrawPageLine(GuiGraphics& g, const StyledPage& page, size_t index, int x, int y, uint32_t fallback) {
            const LineSpan& line = page.lines[index];
            size_t i = line.begin;
            int cursor = x;
            while (i < line.end) {
                const uint16_t styleIndex = page.glyphs[i].style;
                std::string run;
                while (i < line.end && page.glyphs[i].style == styleIndex) {
                    run += page.glyphs[i].text;
                    ++i;
                }
                const Style& s = page.styles[styleIndex];
                cursor += DrawRun(g, run, cursor, y, ColorOf(s, fallback), s.IsBold(), s.IsUnderlined(),
                                  s.IsStrikethrough(), s.IsObfuscated());
            }
        }

        // ActiveTextCollector.ClickableStyleFinder: the style of the glyph
        // under (mouseX) on line `index` drawn from `x`, or null.
        const Style* StyleAt(const StyledPage& page, size_t index, int x, int mouseX) {
            const LineSpan& line = page.lines[index];
            int cursor = x;
            for (size_t i = line.begin; i < line.end; ++i) {
                const Style& s = page.styles[page.glyphs[i].style];
                const int advance = CharAdvance(static_cast<unsigned char>(page.glyphs[i].text[0]), s.IsBold());
                if (mouseX >= cursor && mouseX < cursor + advance) return &s;
                cursor += advance;
            }
            return nullptr;
        }

        // "Page %1$s of %2$s" (book.pageIndicator).
        std::string PageIndicator(int page, int count) {
            return Game::Text::GetString(Component::Translatable(
                "book.pageIndicator",
                {Component::Literal(std::to_string(page)), Component::Literal(std::to_string(count))}));
        }

        std::string Tr(const char* key) { return Game::Language::Get(key); }

        // The one texture every book screen draws its page on.
        struct BookTexture {
            TextureHandle handle = INVALID_TEXTURE;
            BookTexture() {
                int w = 0, h = 0;
                handle = LoadStandaloneGuiTexture("assets/textures/gui/book.png", w, h);
            }
            ~BookTexture() {
                // Deferred: the frame that closed the screen may still be
                // drawing the page on the GPU.
                if (g_renderBackend && handle != INVALID_TEXTURE) g_renderBackend->DeferredDestroyTexture(handle);
            }
            BookTexture(const BookTexture&) = delete;
            BookTexture& operator=(const BookTexture&) = delete;

            void Draw(GuiGraphics& g, int x, int y) const {
                if (handle == INVALID_TEXTURE) {
                    g.Fill(x, y, x + kImageWidth, y + kImageHeight, 0xFFE8DDBF);
                    return;
                }
                const float uv = static_cast<float>(kImageWidth) / static_cast<float>(kTextureSize);
                g.Blit(handle, x, y, x + kImageWidth, y + kImageHeight, 0.0f, 0.0f, uv, uv);
            }
        };

        // The local player's inventory — the book in hand lives there.
        Game::Inventory* LocalInventory() {
            Game::InventoryMenu* menu = PlayerInventoryMenu();
            return menu ? &menu->getInventory() : nullptr;
        }

        // Inventory index of `hand`, and MC's slot number for it
        // (ServerboundEditBookPacket: the selected hotbar slot, or 40).
        int HandIndex(const Game::Inventory& inv, uint32_t hand) {
            return hand == 0 ? Game::Inventory::HotbarToIndex(inv.GetSelectedSlot())
                             : Game::Inventory::OFFHAND_BEGIN;
        }
        int HandPacketSlot(const Game::Inventory& inv, uint32_t hand) {
            return hand == 0 ? inv.GetSelectedSlot() : 40;
        }

        // MC ClientPacketListener.sendUnattendedCommand, for a click on a
        // run_command page: the command goes to the server as typed.
        void SendClickCommand(const std::string& command) {
            if (!Client::g_networkClient || !Client::g_networkClient->IsConnected()) return;
            auto conn = Client::g_networkClient->GetConnection();
            if (!conn) return;
            // Commands.trimOptionalPrefix, then the chat pipe's own "/".
            std::string trimmed = command;
            if (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(0, 1);
            conn->SendChatMessage("/" + trimmed);
        }

        // ── PageButton — MC PageButton ──────────────────────────────────────
        class PageButton : public Button {
        public:
            PageButton(int x, int y, bool forward, OnPress onPress, bool playTurnSound)
                : Button(x, y, 23, 13, forward ? Tr("book.page_button.next") : Tr("book.page_button.previous"),
                         std::move(onPress)),
                  m_forward(forward), m_playTurnSound(playTurnSound) {
                // PageButton.playDownSound replaces the UI click with the
                // page turn (and nothing, for a book that plays no sound).
                m_playsDownSound = false;
            }

            void OnClick(double mx, double my) override {
                if (m_playTurnSound) Client::Sounds::PlayUI("item.book.page_turn", 1.0f);
                Button::OnClick(mx, my);
            }

        protected:
            void RenderWidget(GuiGraphics& g, int, int, float) override {
                const bool lit = m_hovered || m_focused;
                const char* sprite = m_forward ? (lit ? "widget/page_forward_highlighted" : "widget/page_forward")
                                               : (lit ? "widget/page_backward_highlighted" : "widget/page_backward");
                g.BlitSprite(sprite, m_x, m_y, 23, 13);
            }

        private:
            bool m_forward;
            bool m_playTurnSound;
        };

        // ── BookViewScreen ──────────────────────────────────────────────────

        class BookViewScreen : public Screen {
        public:
            explicit BookViewScreen(std::vector<Component> pages, bool playTurnSound = true)
                : Screen(Tr("book.view.title")), m_pages(std::move(pages)), m_playTurnSound(playTurnSound) {}

            void Init() override {
                CreateMenuControls();
                CreatePageControlButtons();
            }

            void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                s_font = g.GetFontRenderer();
                // extractBackground: the menu background, then the page.
                RenderBackground(g, mouseX, mouseY, partialTick);
                m_book.Draw(g, BackgroundLeft(m_width), BackgroundTop());
                g.NextStratum();
                for (auto& w : m_widgets) w->Render(g, mouseX, mouseY, partialTick);
                g.NextStratum();
                VisitText(g, mouseX, mouseY);
            }

            bool KeyPressed(int glfwKey, int glfwMods) override {
                if (Screen::KeyPressed(glfwKey, glfwMods)) return true;
                // PageUp / PageDown press the page buttons (onPress: no sound).
                if (glfwKey == GLFW_KEY_PAGE_UP)   { PageBack();    return true; }
                if (glfwKey == GLFW_KEY_PAGE_DOWN) { PageForward(); return true; }
                return false;
            }

            bool MouseClicked(double mx, double my, int button) override {
                if (button == GLFW_MOUSE_BUTTON_LEFT) {
                    if (const Style* style = ClickableStyleAt(static_cast<int>(mx), static_cast<int>(my))) {
                        if (style->clickEvent && HandleClickEvent(*style->clickEvent)) return true;
                    }
                }
                return Screen::MouseClicked(mx, my, button);
            }

        protected:
            // MC setBookAccess.
            void SetPages(std::vector<Component> pages) {
                m_pages = std::move(pages);
                m_currentPage = std::clamp(m_currentPage, 0, PageCount());
                UpdateButtonVisibility();
                m_cachedPage = -1;
            }

            // MC setPage.
            bool SetPage(int page) {
                const int clamped = std::clamp(page, 0, std::max(PageCount() - 1, 0));
                if (clamped == m_currentPage) return false;
                m_currentPage = clamped;
                UpdateButtonVisibility();
                m_cachedPage = -1;
                return true;
            }

            virtual bool ForcePage(int page) { return SetPage(page); }

            virtual void CreateMenuControls() {
                AddWidget(new Button((m_width - 200) / 2, MenuControlsTop(), 200, WidgetDims::BUTTON_HEIGHT,
                                     Tr("gui.done"), [this] { OnClose(); }));
            }

            virtual void PageBack() {
                if (m_currentPage > 0) --m_currentPage;
                UpdateButtonVisibility();
            }

            virtual void PageForward() {
                if (m_currentPage < PageCount() - 1) ++m_currentPage;
                UpdateButtonVisibility();
            }

            virtual void CloseContainerOnServer() {}

            int PageCount() const { return static_cast<int>(m_pages.size()); }

            int m_currentPage = 0;

        private:
            void CreatePageControlButtons() {
                const int left = BackgroundLeft(m_width);
                const int top  = BackgroundTop();
                m_forwardButton = AddWidget(new PageButton(left + kPageForwardX, top + kPageButtonY, true,
                                                           [this] { PageForward(); }, m_playTurnSound));
                m_backButton    = AddWidget(new PageButton(left + kPageBackX, top + kPageButtonY, false,
                                                           [this] { PageBack(); }, m_playTurnSound));
                UpdateButtonVisibility();
            }

            void UpdateButtonVisibility() {
                if (m_forwardButton) m_forwardButton->visible = m_currentPage < PageCount() - 1;
                if (m_backButton)    m_backButton->visible    = m_currentPage > 0;
            }

            void EnsureLayout() {
                if (m_cachedPage == m_currentPage && m_cachedFont == s_font) return;
                // ComponentUtils.mergeStyles(page, PAGE_TEXT_STYLE): black,
                // unshadowed, the page's own style winning.
                Style base;
                base.color = Game::Text::TextColor::FromRgb(0x000000);
                const Component empty;
                const Component& page = (m_currentPage >= 0 && m_currentPage < PageCount())
                                            ? m_pages[static_cast<size_t>(m_currentPage)] : empty;
                m_layout = LayoutPage(page, base, kTextWidth);
                m_pageMsg = PageIndicator(m_currentPage + 1, std::max(PageCount(), 1));
                m_cachedPage = m_currentPage;
                m_cachedFont = s_font;
            }

            int ShownLines() const {
                return std::min(kViewTextHeight / kLineHeight, static_cast<int>(m_layout.lines.size()));
            }

            void VisitText(GuiGraphics& g, int mouseX, int mouseY) {
                EnsureLayout();
                const int left = BackgroundLeft(m_width);
                const int top  = BackgroundTop();
                // TextAlignment.RIGHT at (left + 148, top + 16).
                DrawPlain(g, m_pageMsg, left + kPageIndicatorX - PlainWidth(m_pageMsg), top + kPageIndicatorY, kBlack);
                const int shown = ShownLines();
                for (int i = 0; i < shown; ++i) {
                    DrawPageLine(g, m_layout, static_cast<size_t>(i), left + kPageTextX,
                                 top + kPageTextY + i * kLineHeight, kBlack);
                }
                // HoveredTextEffects.TOOLTIP_AND_CURSOR: a show_text hover.
                if (const Style* style = ClickableStyleAt(mouseX, mouseY); style && style->hoverText) {
                    RenderHoverTooltip(g, *style->hoverText, mouseX, mouseY);
                }
            }

            const Style* ClickableStyleAt(int mouseX, int mouseY) {
                EnsureLayout();
                const int left = BackgroundLeft(m_width);
                const int top  = BackgroundTop() + kPageTextY;
                if (mouseY < top) return nullptr;
                const int line = (mouseY - top) / kLineHeight;
                if (line < 0 || line >= ShownLines()) return nullptr;
                return StyleAt(m_layout, static_cast<size_t>(line), left + kPageTextX, mouseX);
            }

            void RenderHoverTooltip(GuiGraphics& g, const Component& text, int mouseX, int mouseY) {
                // A plain tooltip panel (the screens' tooltip chrome) with
                // the hover text wrapped the way MC wraps tooltips (170 px).
                StyledPage hover = LayoutPage(text, Style{}, 170);
                if (hover.lines.empty()) return;
                int maxW = 0;
                for (size_t i = 0; i < hover.lines.size(); ++i) {
                    int w = 0;
                    for (size_t k = hover.lines[i].begin; k < hover.lines[i].end; ++k) {
                        w += CharAdvance(static_cast<unsigned char>(hover.glyphs[k].text[0]),
                                         hover.styles[hover.glyphs[k].style].IsBold());
                    }
                    maxW = std::max(maxW, w);
                }
                const int th = static_cast<int>(hover.lines.size()) * (kLineHeight + 1);
                int tx = mouseX + 12, ty = mouseY - 12;
                if (tx + maxW + 8 > m_width) tx = m_width - maxW - 8;
                if (ty + th + 8 > m_height)  ty = m_height - th - 8;
                tx = std::max(tx, 0);
                ty = std::max(ty, 0);
                g.NextStratum();
                g.Fill(tx - 3, ty - 3, tx + maxW + 3, ty + th + 3, 0xF0100010);
                g.RenderOutline(tx - 3, ty - 3, maxW + 6, th + 6, 0xFF250559);
                for (size_t i = 0; i < hover.lines.size(); ++i) {
                    DrawPageLine(g, hover, i, tx, ty + static_cast<int>(i) * (kLineHeight + 1), 0xFFFFFFFFu);
                }
            }

            // MC BookViewScreen.handleClickEvent → Screen.defaultHandle*.
            bool HandleClickEvent(const Game::Text::ClickEvent& event) {
                using Action = Game::Text::ClickEvent::Action;
                switch (event.action) {
                    case Action::ChangePage:
                        ForcePage(event.page - 1);
                        return true;
                    case Action::RunCommand:
                        CloseContainerOnServer();
                        SendClickCommand(event.value);
                        // clickCommandAction(player, command, null) — the
                        // book closes.
                        if (m_manager) m_manager->Clear();
                        return true;
                    case Action::CopyToClipboard:
                        CopyToClipboard(event.value);
                        return true;
                    case Action::SuggestCommand:
                        // activeScreen.insertText: a book has no text field;
                        // MC's click does nothing visible here either.
                        return true;
                    case Action::OpenUrl:
                    case Action::OpenFile:
                    case Action::ShowDialog:
                    case Action::Custom:
                        Log::Info("[Book] %s click events are not supported: %s",
                                  Game::Text::ClickEvent::ActionName(event.action), event.value.c_str());
                        return true;
                }
                return false;
            }

            std::vector<Component> m_pages;
            bool                   m_playTurnSound = true;
            BookTexture            m_book;
            StyledPage             m_layout;
            std::string            m_pageMsg;
            int                    m_cachedPage = -1;
            const FontRenderer*    m_cachedFont = nullptr;
            PageButton*            m_forwardButton = nullptr;
            PageButton*            m_backButton = nullptr;
        };

        // MC BookViewScreen.BookAccess.fromItem.
        std::optional<std::vector<Component>> BookAccessFromItem(const Game::ItemStack& stack) {
            if (auto written = stack.get(Game::DataComponents::WRITTEN_BOOK_CONTENT)) {
                return written->GetPages(false);
            }
            if (auto writable = stack.get(Game::DataComponents::WRITABLE_BOOK_CONTENT)) {
                std::vector<Component> pages;
                for (auto& page : writable->GetPages(false)) pages.push_back(Component::Literal(std::move(page)));
                return pages;
            }
            return std::nullopt;
        }

        // ── LecternScreen ───────────────────────────────────────────────────

        bool s_lecternScreenOpen = false;

        class LecternScreen : public BookViewScreen {
        public:
            LecternScreen(bool mayBuild)
                : BookViewScreen({}, true), m_mayBuild(mayBuild) {
                s_lecternScreenOpen = true;
                SyncFromMenu(true);
            }
            ~LecternScreen() override { s_lecternScreenOpen = false; }

            bool IsPauseScreen() const override { return false; }

            void OnClose() override {
                // LocalPlayer.closeContainer: tell the server, fall back to
                // the player's own menu, then leave.
                if (m_popped) return;
                CloseContainerOnServer();
                m_popped = true;
                Screen::OnClose();
            }

            void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                SyncFromMenu(false);
                BookViewScreen::Render(g, mouseX, mouseY, partialTick);
            }

            // The server took the menu away; no close packet goes back.
            void CloseSilently() {
                if (m_popped) return;
                m_popped = true;
                m_closed = true;
                if (m_manager) m_manager->Pop();
            }

        protected:
            void CreateMenuControls() override {
                if (!m_mayBuild) {
                    BookViewScreen::CreateMenuControls();
                    return;
                }
                const int y = MenuControlsTop();
                const int middle = m_width / 2;
                AddWidget(new Button(middle - kMenuButtonSize - 2, y, kMenuButtonSize, WidgetDims::BUTTON_HEIGHT,
                                     Tr("gui.done"), [this] { OnClose(); }));
                AddWidget(new Button(middle + 2, y, kMenuButtonSize, WidgetDims::BUTTON_HEIGHT,
                                     Tr("lectern.take_book"),
                                     [this] { SendButtonClick(Game::LecternMenu::BUTTON_TAKE_BOOK); }));
            }

            void PageBack() override    { SendButtonClick(Game::LecternMenu::BUTTON_PREV_PAGE); }
            void PageForward() override { SendButtonClick(Game::LecternMenu::BUTTON_NEXT_PAGE); }

            bool ForcePage(int page) override {
                Game::LecternMenu* menu = Menu();
                if (!menu || page == menu->GetPage()) return false;
                SendButtonClick(Game::LecternMenu::BUTTON_PAGE_JUMP_RANGE_START + page);
                return true;
            }

            void CloseContainerOnServer() override {
                if (m_closed) return;
                m_closed = true;
                s_containerClose = true;
                SetClientContainerMenu(nullptr, Game::MenuType::Inventory);
            }

        private:
            static Game::LecternMenu* Menu() {
                if (ClientContainerMenuType() != Game::MenuType::Lectern) return nullptr;
                return dynamic_cast<Game::LecternMenu*>(PlayerContainerMenu());
            }

            void SendButtonClick(int buttonId) {
                Game::LecternMenu* menu = Menu();
                if (!menu || m_closed) return;
                Network::ContainerButtonClickC2SPacket packet;
                packet.containerId = menu->containerId;
                packet.buttonId = static_cast<uint32_t>(buttonId);
                s_buttonClicks.push_back(packet);
            }

            // MC's ContainerListener: slotChanged → bookChanged,
            // dataChanged(0) → pageChanged.
            void SyncFromMenu(bool force) {
                Game::LecternMenu* menu = Menu();
                if (!menu) return;
                const uint32_t revision = ContainerContentRevision();
                if (force || revision != m_seenRevision) {
                    m_seenRevision = revision;
                    SetPages(BookAccessFromItem(menu->GetBook()).value_or(std::vector<Component>{}));
                }
                const int page = menu->GetPage();
                if (force || page != m_seenPage) {
                    m_seenPage = page;
                    SetPage(page);
                }
            }

            bool     m_mayBuild = true;
            bool     m_closed = false;   // the server has been told (or told us)
            bool     m_popped = false;   // the pop is queued on the manager
            uint32_t m_seenRevision = 0;
            int      m_seenPage = -1;
        };

        LecternScreen* CurrentLecternScreen() {
            if (!s_lecternScreenOpen) return nullptr;
            return dynamic_cast<LecternScreen*>(GetScreenManager().Current());
        }

        // ── BookPageEditor — MC MultiLineEditBox over MultilineTextField ────

        class BookPageEditor : public AbstractWidget {
        public:
            struct View { int begin; int end; };

            BookPageEditor(int x, int y, int width, int height)
                : AbstractWidget(x, y, width, height, "") {}

            std::function<void(const std::string&)> valueListener;

            // MultilineTextField.setValue(value, allowOverflowLineLimit).
            void SetValue(const std::string& value, bool allowOverflowLineLimit) {
                const std::string truncated = value.substr(0, std::min<size_t>(value.size(), kCharacterLimit));
                if (allowOverflowLineLimit || !OverflowsLineLimit(truncated)) {
                    m_value = truncated;
                    m_cursor = static_cast<int>(m_value.size());
                    m_selectCursor = m_cursor;
                    OnValueChange();
                }
            }
            const std::string& Value() const { return m_value; }

            void SetFocused(bool f) override {
                if (f && !m_focused) m_focusedAt = NowMillis();
                AbstractWidget::SetFocused(f);
            }

            bool CharTyped(unsigned int codepoint) override {
                if (!m_focused) return false;
                // StringUtil.isAllowedChatCharacter, within the atlas.
                if (codepoint < 32 || codepoint > 126) return false;
                InsertText(std::string(1, static_cast<char>(codepoint)));
                return true;
            }

            bool KeyPressed(int key, int mods) override {
                if (!m_focused) return false;
                m_selecting = (mods & GLFW_MOD_SHIFT) != 0;
                const bool shortcut = (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)) != 0;
                // hasControlDownWithQuirk: Alt on a Mac, Ctrl elsewhere.
#ifdef __APPLE__
                const bool wordMod = (mods & GLFW_MOD_ALT) != 0;
#else
                const bool wordMod = (mods & GLFW_MOD_CONTROL) != 0;
#endif
                if (shortcut && key == GLFW_KEY_A) {
                    m_cursor = static_cast<int>(m_value.size());
                    m_selectCursor = 0;
                    return true;
                }
                if (shortcut && key == GLFW_KEY_C) {
                    Input::SetClipboardText(SelectedText());
                    return true;
                }
                if (shortcut && key == GLFW_KEY_V) {
                    InsertText(Input::GetClipboardText());
                    return true;
                }
                if (shortcut && key == GLFW_KEY_X) {
                    Input::SetClipboardText(SelectedText());
                    InsertText("");
                    return true;
                }
                switch (key) {
                    case GLFW_KEY_BACKSPACE:
                        if (wordMod) DeleteText(PreviousWord().begin - m_cursor);
                        else         DeleteText(-1);
                        return true;
                    case GLFW_KEY_ENTER:
                    case GLFW_KEY_KP_ENTER:
                        InsertText("\n");
                        return true;
                    case GLFW_KEY_DELETE:
                        if (wordMod) DeleteText(NextWord().begin - m_cursor);
                        else         DeleteText(1);
                        return true;
                    case GLFW_KEY_HOME:
                        SeekCursor(wordMod ? 0 : CursorLineView(0).begin);
                        return true;
                    case GLFW_KEY_END:
                        SeekCursor(wordMod ? static_cast<int>(m_value.size()) : CursorLineView(0).end);
                        return true;
                    case GLFW_KEY_RIGHT:
                        SeekCursor(wordMod ? NextWord().begin : m_cursor + 1);
                        return true;
                    case GLFW_KEY_LEFT:
                        SeekCursor(wordMod ? PreviousWord().begin : m_cursor - 1);
                        return true;
                    case GLFW_KEY_DOWN:
                        if (!wordMod) SeekCursorLine(1);
                        return true;
                    case GLFW_KEY_UP:
                        if (!wordMod) SeekCursorLine(-1);
                        return true;
                    default:
                        return false;
                }
            }

            void OnClick(double mx, double my) override {
                // MultiLineEditBox.onClick: a double click selects the word.
                const double now = NowMillis();
                const bool doubleClick = now - m_lastClickMs < 250.0;
                m_lastClickMs = now;
                if (doubleClick) {
                    const View word = PreviousWord();
                    SeekCursor(word.begin);
                    m_selecting = true;
                    SeekCursor(WordEnd(word.begin));
                    m_selecting = false;
                    return;
                }
                m_selecting = (Input::IsGlfwKeyDown(GLFW_KEY_LEFT_SHIFT) || Input::IsGlfwKeyDown(GLFW_KEY_RIGHT_SHIFT));
                SeekCursorToPoint(mx - m_x - kInnerPadding, my - m_y - kInnerPadding);
            }

            void OnDrag(double mx, double my) override {
                // MultiLineEditBox.onDrag: extend the selection to the point.
                m_selecting = true;
                SeekCursorToPoint(mx - m_x - kInnerPadding, my - m_y - kInnerPadding);
                m_selecting = (Input::IsGlfwKeyDown(GLFW_KEY_LEFT_SHIFT) || Input::IsGlfwKeyDown(GLFW_KEY_RIGHT_SHIFT));
            }

        protected:
            void RenderWidget(GuiGraphics& g, int, int, float) override {
                s_font = g.GetFontRenderer();
                if (m_linesFont != s_font) Reflow();
                const int innerLeft = m_x + kInnerPadding;
                int drawTop = m_y + kInnerPadding;
                const bool showCursor = m_focused && CursorVisible(NowMillis() - m_focusedAt);
                const bool insertCursor = m_cursor < static_cast<int>(m_value.size());
                int cursorX = 0, cursorY = 0;
                bool drawnCursor = false;
                for (const View& line : m_lines) {
                    if (!drawnCursor && showCursor && insertCursor && m_cursor >= line.begin && m_cursor <= line.end) {
                        const std::string before = m_value.substr(line.begin, m_cursor - line.begin);
                        const int beforeW = PlainWidth(before);
                        DrawPlain(g, before, innerLeft, drawTop, kBlack);
                        DrawPlain(g, m_value.substr(m_cursor, line.end - m_cursor), innerLeft + beforeW, drawTop, kBlack);
                        // TextCursorUtils.extractInsertCursor.
                        g.Fill(innerLeft + beforeW, drawTop - 1, innerLeft + beforeW + 1, drawTop + kLineHeight + 1, kBlack);
                        drawnCursor = true;
                    } else {
                        const std::string text = m_value.substr(line.begin, line.end - line.begin);
                        DrawPlain(g, text, innerLeft, drawTop, kBlack);
                        if (!insertCursor) {
                            cursorX = innerLeft + PlainWidth(text);
                            cursorY = drawTop;
                        }
                    }
                    drawTop += kLineHeight;
                }
                if (showCursor && !insertCursor) {
                    // TextCursorUtils.extractAppendCursor: "_".
                    DrawPlain(g, "_", cursorX, cursorY, kBlack);
                }
                if (m_cursor != m_selectCursor) {
                    // GuiGraphicsExtractor.textHighlight: MC inverts the text
                    // under a blue OR-reverse fill; drawn here as the blue
                    // highlight over the text.
                    const int selBegin = std::min(m_cursor, m_selectCursor);
                    const int selEnd   = std::max(m_cursor, m_selectCursor);
                    int top = m_y + kInnerPadding;
                    for (const View& line : m_lines) {
                        if (selBegin > line.end) { top += kLineHeight; continue; }
                        if (line.begin > selEnd) break;
                        const int x0 = PlainWidth(m_value.substr(line.begin, std::max(selBegin, line.begin) - line.begin));
                        const int x1 = selEnd > line.end ? m_width - kInnerPadding
                                                         : PlainWidth(m_value.substr(line.begin, selEnd - line.begin));
                        g.Fill(innerLeft + x0, top, innerLeft + x1, top + kLineHeight, 0x800000FFu);
                        top += kLineHeight;
                    }
                }
            }

        private:
            static constexpr int kInnerPadding  = 4;
            static constexpr size_t kCharacterLimit = static_cast<size_t>(Game::WritableBookContent::PAGE_EDIT_LENGTH);
            static constexpr int kLineLimit     = kEditTextHeight / kLineHeight;   // 14
            static constexpr int kTextAreaWidth = kTextWidth;                      // 122 - 2 * 4

            // MultilineTextField's splitLines(value, width, EMPTY, false, …).
            static std::vector<View> SplitLines(const std::string& value) {
                std::vector<BreakItem> items;
                items.reserve(value.size());
                for (unsigned char c : value) {
                    BreakItem it;
                    it.newline = c == '\n';
                    it.space = c == ' ';
                    it.width = it.newline ? 0 : CharAdvance(c, false);
                    items.push_back(it);
                }
                std::vector<View> out;
                for (const LineSpan& span : BreakLines(items, kTextAreaWidth, false)) {
                    out.push_back({static_cast<int>(span.begin), static_cast<int>(span.end)});
                }
                return out;
            }

            // MultilineTextField.overflowsLineLimit.
            static bool OverflowsLineLimit(const std::string& value) {
                const size_t lines = SplitLines(value).size() + (!value.empty() && value.back() == '\n' ? 1 : 0);
                return lines > static_cast<size_t>(kLineLimit);
            }

            void Reflow() {
                m_linesFont = s_font;
                m_lines.clear();
                if (m_value.empty()) {
                    m_lines.push_back({0, 0});
                    return;
                }
                m_lines = SplitLines(m_value);
                if (m_value.back() == '\n') {
                    m_lines.push_back({static_cast<int>(m_value.size()), static_cast<int>(m_value.size())});
                }
            }

            void OnValueChange() {
                Reflow();
                if (valueListener) valueListener(m_value);
            }

            std::string SelectedText() const {
                const int b = std::min(m_cursor, m_selectCursor);
                const int e = std::max(m_cursor, m_selectCursor);
                return m_value.substr(b, e - b);
            }

            // MultilineTextField.insertText.
            void InsertText(const std::string& input) {
                if (input.empty() && m_cursor == m_selectCursor) return;
                // StringUtil.filterText(input, allowLineBreaks = true), then
                // the character limit; the atlas is ASCII.
                std::string text;
                for (unsigned char c : input) {
                    if (c == '\n' || (c >= 32 && c <= 126)) text.push_back(static_cast<char>(c));
                }
                const int b = std::min(m_cursor, m_selectCursor);
                const int e = std::max(m_cursor, m_selectCursor);
                const size_t remaining = kCharacterLimit - std::min(kCharacterLimit, m_value.size() - (e - b));
                if (text.size() > remaining) text.resize(remaining);
                std::string next = m_value;
                next.replace(b, e - b, text);
                if (OverflowsLineLimit(next)) return;
                m_value = std::move(next);
                m_cursor = b + static_cast<int>(text.size());
                m_selectCursor = m_cursor;
                OnValueChange();
            }

            // MultilineTextField.deleteText.
            void DeleteText(int dir) {
                if (m_cursor == m_selectCursor) {
                    m_selectCursor = std::clamp(m_cursor + dir, 0, static_cast<int>(m_value.size()));
                }
                InsertText("");
            }

            void SeekCursor(int pos) {
                m_cursor = std::clamp(pos, 0, static_cast<int>(m_value.size()));
                if (!m_selecting) m_selectCursor = m_cursor;
            }

            int LineAtCursor() const {
                for (size_t i = 0; i < m_lines.size(); ++i) {
                    if (m_cursor >= m_lines[i].begin && m_cursor <= m_lines[i].end) return static_cast<int>(i);
                }
                return -1;
            }

            View CursorLineView(int offset) const {
                const int line = LineAtCursor();
                if (line < 0) return m_lines.back();
                return m_lines[static_cast<size_t>(std::clamp(line + offset, 0, static_cast<int>(m_lines.size()) - 1))];
            }

            // MultilineTextField.seekCursorLine (LINE_SEEK_PIXEL_BIAS 2).
            void SeekCursorLine(int offset) {
                if (offset == 0) return;
                const View here = CursorLineView(0);
                const int left = PlainWidth(m_value.substr(here.begin, m_cursor - here.begin)) + 2;
                const View there = CursorLineView(offset);
                const std::string text = m_value.substr(there.begin, there.end - there.begin);
                SeekCursor(there.begin + static_cast<int>(PlainSubstrByWidth(text, left)));
            }

            // MultilineTextField.seekCursorToPoint.
            void SeekCursorToPoint(double x, double y) {
                const int left = static_cast<int>(std::floor(x));
                const int top = static_cast<int>(std::floor(y / kLineHeight));
                const View line = m_lines[static_cast<size_t>(std::clamp(top, 0, static_cast<int>(m_lines.size()) - 1))];
                const std::string text = m_value.substr(line.begin, line.end - line.begin);
                SeekCursor(line.begin + static_cast<int>(PlainSubstrByWidth(text, left)));
            }

            static bool IsSpace(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

            int WordEnd(int from) const {
                int end = from;
                while (end < static_cast<int>(m_value.size()) && !IsSpace(m_value[end])) ++end;
                return end;
            }

            View PreviousWord() const {
                if (m_value.empty()) return {0, 0};
                int start = std::clamp(m_cursor, 0, static_cast<int>(m_value.size()) - 1);
                while (start > 0 && IsSpace(m_value[start - 1])) --start;
                while (start > 0 && !IsSpace(m_value[start - 1])) --start;
                return {start, WordEnd(start)};
            }

            View NextWord() const {
                if (m_value.empty()) return {0, 0};
                int start = std::clamp(m_cursor, 0, static_cast<int>(m_value.size()) - 1);
                while (start < static_cast<int>(m_value.size()) && !IsSpace(m_value[start])) ++start;
                while (start < static_cast<int>(m_value.size()) && IsSpace(m_value[start])) ++start;
                return {start, WordEnd(start)};
            }

            std::string         m_value;
            int                 m_cursor = 0;
            int                 m_selectCursor = 0;
            bool                m_selecting = false;
            std::vector<View>   m_lines{{0, 0}};
            const FontRenderer* m_linesFont = nullptr;
            double              m_focusedAt = 0.0;
            double              m_lastClickMs = -1000.0;
        };

        // ── BookEditScreen / BookSignScreen ─────────────────────────────────

        class BookEditScreen;

        class BookSignScreen : public Screen {
        public:
            BookSignScreen(BookEditScreen* editor, uint32_t hand, std::string ownerName)
                : Screen(Tr("book.sign.title")), m_editor(editor), m_hand(hand),
                  m_ownerText(Game::Text::GetString(Component::Translatable(
                      "book.byAuthor", {Component::Literal(std::move(ownerName))}))) {}

            void Init() override;
            void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
            bool KeyPressed(int glfwKey, int glfwMods) override;
            bool CharTyped(unsigned int codepoint) override;
            bool IsPauseScreen() const override { return true; }
            // Screen.onClose (Esc): setScreen(null) — the sign screen and the
            // editor under it both go, unsaved. Cancel is the way back.
            void OnClose() override { if (m_manager) m_manager->Clear(); }

            std::string titleValue;

        private:
            void SaveChanges();
            void UpdateFinalize() {
                // StringUtil.isBlank.
                bool blank = true;
                for (char c : titleValue) if (!IsBlankChar(c)) { blank = false; break; }
                if (m_finalize) m_finalize->active = !blank;
            }
            static bool IsBlankChar(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

            BookEditScreen* m_editor;
            uint32_t        m_hand;
            std::string     m_ownerText;
            Button*         m_finalize = nullptr;
            BookTexture     m_book;
            double          m_openedAt = NowMillis();
            static constexpr size_t kTitleMaxLength = 15;   // titleBox.setMaxLength(15)
        };

        class BookEditScreen : public Screen {
        public:
            BookEditScreen(const Game::WritableBookContent& content, uint32_t hand, std::string ownerName)
                : Screen(Tr("book.edit.title")), m_hand(hand), m_ownerName(std::move(ownerName)) {
                for (auto& page : content.GetPages(false)) m_pages.push_back(std::move(page));
                if (m_pages.empty()) m_pages.emplace_back();
            }

            void Init() override {
                const int left = BackgroundLeft(m_width);
                const int top = BackgroundTop();
                // MultiLineEditBox at ((w - 114) / 2 - 8, 28), 122 × 134, no
                // decorations or background, black text and cursor, 1024
                // characters, 126 / 9 = 14 lines.
                m_page = AddWidget(new BookPageEditor((m_width - kTextWidth) / 2 - 8, 28, 122, 134));
                m_page->valueListener = [this](const std::string& value) {
                    m_pages[static_cast<size_t>(m_currentPage)] = value;
                };
                UpdatePageContent();
                m_backButton = AddWidget(new PageButton(left + kPageBackX, top + kPageButtonY, false,
                                                        [this] { PageBack(); }, true));
                AddWidget(new PageButton(left + kPageForwardX, top + kPageButtonY, true,
                                         [this] { PageForward(); }, true));
                AddWidget(new Button(m_width / 2 - kMenuButtonSize - 2, MenuControlsTop(), kMenuButtonSize,
                                     WidgetDims::BUTTON_HEIGHT, Tr("book.signButton"), [this] {
                    auto sign = std::make_unique<BookSignScreen>(this, m_hand, m_ownerName);
                    sign->titleValue = m_signTitle;
                    if (m_manager) m_manager->Push(std::move(sign));
                }));
                AddWidget(new Button(m_width / 2 + 2, MenuControlsTop(), kMenuButtonSize,
                                     WidgetDims::BUTTON_HEIGHT, Tr("gui.done"), [this] {
                    SaveChanges();
                    Screen::OnClose();
                }));
                UpdateButtonVisibility();
                SetFocus(m_page);   // setInitialFocus(page)
            }

            void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override {
                s_font = g.GetFontRenderer();
                RenderBackground(g, mouseX, mouseY, partialTick);
                m_book.Draw(g, BackgroundLeft(m_width), BackgroundTop());
                g.NextStratum();
                for (auto& w : m_widgets) w->Render(g, mouseX, mouseY, partialTick);
                // visitText: the page indicator, right-aligned at left + 148.
                const std::string msg = PageIndicator(m_currentPage + 1, static_cast<int>(m_pages.size()));
                DrawPlain(g, msg, BackgroundLeft(m_width) + kPageIndicatorX - PlainWidth(msg),
                          BackgroundTop() + kPageIndicatorY, kBlack);
            }

            bool KeyPressed(int glfwKey, int glfwMods) override {
                if (glfwKey == GLFW_KEY_PAGE_UP)   { PageBack();    return true; }
                if (glfwKey == GLFW_KEY_PAGE_DOWN) { PageForward(); return true; }
                // The editor keeps Tab out of focus cycling: MC's text area
                // consumes navigation, and a book has nowhere else to type.
                return Screen::KeyPressed(glfwKey, glfwMods);
            }

            bool MouseClicked(double mx, double my, int button) override {
                const bool handled = Screen::MouseClicked(mx, my, button);
                // PageButton.shouldTakeFocusAfterInteraction is false: the
                // editor keeps the keyboard through a page turn.
                if (FocusedWidget() != m_page) SetFocus(m_page);
                return handled;
            }

            // BookSignScreen's Cancel keeps the typed title for next time.
            std::string m_signTitle;

            // For BookSignScreen.saveChanges: the pages as they stand.
            const std::vector<std::string>& Pages() const { return m_pages; }

        private:
            void PageBack() {
                if (m_currentPage > 0) {
                    --m_currentPage;
                    UpdatePageContent();
                }
                UpdateButtonVisibility();
            }

            void PageForward() {
                if (m_currentPage < static_cast<int>(m_pages.size()) - 1) {
                    ++m_currentPage;
                } else {
                    AppendPageToBook();
                    if (m_currentPage < static_cast<int>(m_pages.size()) - 1) ++m_currentPage;
                }
                UpdatePageContent();
                UpdateButtonVisibility();
            }

            void UpdatePageContent() {
                if (m_page) m_page->SetValue(m_pages[static_cast<size_t>(m_currentPage)], true);
            }

            void UpdateButtonVisibility() {
                if (m_backButton) m_backButton->visible = m_currentPage > 0;
            }

            void AppendPageToBook() {
                if (m_pages.size() < static_cast<size_t>(Game::WritableBookContent::MAX_PAGES)) m_pages.emplace_back();
            }

            void EraseEmptyTrailingPages() {
                while (!m_pages.empty() && m_pages.back().empty()) m_pages.pop_back();
            }

            // MC saveChanges: trailing empties go, the local copy updates,
            // and the pages go to the server without a title.
            void SaveChanges() {
                EraseEmptyTrailingPages();
                Game::Inventory* inv = LocalInventory();
                if (!inv) return;
                const int index = HandIndex(*inv, m_hand);
                // updateLocalCopy.
                Game::ItemStack& stack = inv->MutableSlot(index);
                if (stack.get(Game::DataComponents::WRITABLE_BOOK_CONTENT)) {
                    Game::WritableBookContent content;
                    for (const std::string& page : m_pages) {
                        content.pages.push_back(Game::Filterable<std::string>::PassThrough(page));
                    }
                    stack.components.set(Game::DataComponents::WRITABLE_BOOK_CONTENT, std::move(content));
                }
                Network::EditBookC2SPacket packet;
                packet.slot = HandPacketSlot(*inv, m_hand);
                packet.pages = m_pages;
                s_editBooks.push_back(std::move(packet));
            }

            uint32_t                 m_hand;
            std::string              m_ownerName;
            std::vector<std::string> m_pages;
            int                      m_currentPage = 0;
            BookPageEditor*          m_page = nullptr;
            PageButton*              m_backButton = nullptr;
            BookTexture              m_book;
        };

        void BookSignScreen::Init() {
            // Sign and Close at (w/2 - 100, 196), inactive until a title;
            // Cancel at (w/2 + 2, 196).
            m_finalize = AddWidget(new Button(m_width / 2 - 100, 196, kMenuButtonSize, WidgetDims::BUTTON_HEIGHT,
                                              Tr("book.finalizeButton"), [this] {
                SaveChanges();
                if (m_manager) m_manager->Clear();
            }));
            AddWidget(new Button(m_width / 2 + 2, 196, kMenuButtonSize, WidgetDims::BUTTON_HEIGHT,
                                 Tr("gui.cancel"), [this] {
                if (m_editor) m_editor->m_signTitle = titleValue;
                if (m_manager) m_manager->Pop();   // back to the editor
            }));
            UpdateFinalize();
        }

        void BookSignScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
            s_font = g.GetFontRenderer();
            RenderBackground(g, mouseX, mouseY, partialTick);
            const int xo = (m_width - kImageWidth) / 2;
            m_book.Draw(g, xo, BackgroundTop());
            g.NextStratum();
            for (auto& w : m_widgets) w->Render(g, mouseX, mouseY, partialTick);

            const std::string header = Tr("book.editTitle");
            DrawPlain(g, header, xo + kPageTextX + (kTextWidth - PlainWidth(header)) / 2, 34, kBlack);

            // The title box: EditBox((w - 114) / 2 - 3, 50, 114 × 20),
            // borderless and centred, black, no shadow. A borderless box
            // draws its text at its own top (EditBox: textY = getY()), the
            // "_" cursor at the end while it blinks on.
            const int boxX = (m_width - kTextWidth) / 2 - 3;
            const int textW = PlainWidth(titleValue);
            const int textX = boxX + (kTextWidth - textW) / 2;
            DrawPlain(g, titleValue, textX, 50, kBlack);
            if (CursorVisible(NowMillis() - m_openedAt)) DrawPlain(g, "_", textX + textW, 50, kBlack);

            DrawPlain(g, m_ownerText, xo + kPageTextX + (kTextWidth - PlainWidth(m_ownerText)) / 2, 60, kDarkGray);

            // textWithWordWrap(FINALIZE_WARNING, xo + 36, 82, 114).
            StyledPage warning = LayoutPage(Component::Literal(Tr("book.finalizeWarning")), Style{}, kTextWidth);
            for (size_t i = 0; i < warning.lines.size(); ++i) {
                DrawPageLine(g, warning, i, xo + kPageTextX, 82 + static_cast<int>(i) * kLineHeight, kBlack);
            }
        }

        bool BookSignScreen::KeyPressed(int glfwKey, int glfwMods) {
            // Enter with a title signs (BookSignScreen.keyPressed).
            if ((glfwKey == GLFW_KEY_ENTER || glfwKey == GLFW_KEY_KP_ENTER) && !titleValue.empty()) {
                SaveChanges();
                if (m_manager) m_manager->Clear();
                return true;
            }
            if (glfwKey == GLFW_KEY_BACKSPACE) {
                if (!titleValue.empty()) {
                    titleValue.pop_back();
                    UpdateFinalize();
                }
                return true;
            }
            if (glfwKey == GLFW_KEY_V && (glfwMods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER))) {
                for (unsigned char c : Input::GetClipboardText()) {
                    if (titleValue.size() >= kTitleMaxLength) break;
                    if (c >= 32 && c <= 126) titleValue.push_back(static_cast<char>(c));
                }
                UpdateFinalize();
                return true;
            }
            return Screen::KeyPressed(glfwKey, glfwMods);
        }

        bool BookSignScreen::CharTyped(unsigned int codepoint) {
            if (codepoint < 32 || codepoint > 126) return true;
            if (titleValue.size() < kTitleMaxLength) {
                titleValue.push_back(static_cast<char>(codepoint));
                UpdateFinalize();
            }
            return true;
        }

        void BookSignScreen::SaveChanges() {
            // ServerboundEditBookPacket(slot, pages, Optional.of(title.trim())).
            Game::Inventory* inv = LocalInventory();
            if (!inv || !m_editor) return;
            std::string title = titleValue;
            const size_t first = title.find_first_not_of(" \t\r\n");
            const size_t last = title.find_last_not_of(" \t\r\n");
            title = first == std::string::npos ? std::string() : title.substr(first, last - first + 1);
            Network::EditBookC2SPacket packet;
            packet.slot = HandPacketSlot(*inv, m_hand);
            packet.pages = m_editor->Pages();
            packet.title = title;
            s_editBooks.push_back(std::move(packet));
        }

    } // namespace

    // ── Host hooks ──────────────────────────────────────────────────────────

    void OpenBookFromHand(uint32_t hand) {
        Game::Inventory* inv = LocalInventory();
        if (!inv) return;
        const Game::ItemStack& stack = inv->GetSlot(HandIndex(*inv, hand));
        auto pages = BookAccessFromItem(stack);
        if (!pages) return;
        // minecraft.gui.setScreen(new BookViewScreen(access)).
        GetScreenManager().Set(std::make_unique<BookViewScreen>(std::move(*pages)));
    }

    void OpenBookEditScreen(const Game::ItemStack& book, uint32_t hand) {
        auto content = book.get(Game::DataComponents::WRITABLE_BOOK_CONTENT);
        if (!content) return;
        std::string owner = Client::g_networkClient ? Client::g_networkClient->GetPlayerName() : std::string();
        GetScreenManager().Set(std::make_unique<BookEditScreen>(*content, hand, std::move(owner)));
    }

    void OpenLecternScreen(const std::string& title, bool mayBuild) {
        (void)title;   // LecternScreen shows the book, not the container title
        if (LecternScreen* open = CurrentLecternScreen()) open->CloseSilently();
        GetScreenManager().Set(std::make_unique<LecternScreen>(mayBuild));
    }

    void CloseLecternScreen() {
        if (LecternScreen* open = CurrentLecternScreen()) open->CloseSilently();
    }

    bool IsLecternScreenOpen() { return s_lecternScreenOpen; }

    bool ConsumeEditBook(Network::EditBookC2SPacket& out) {
        if (s_editBooks.empty()) return false;
        out = std::move(s_editBooks.front());
        s_editBooks.pop_front();
        return true;
    }

    bool ConsumeContainerButtonClick(Network::ContainerButtonClickC2SPacket& out) {
        if (s_buttonClicks.empty()) return false;
        out = s_buttonClicks.front();
        s_buttonClicks.pop_front();
        return true;
    }

    void QueueContainerButtonClick(uint32_t containerId, uint32_t buttonId) {
        Network::ContainerButtonClickC2SPacket packet;
        packet.containerId = containerId;
        packet.buttonId = buttonId;
        s_buttonClicks.push_back(packet);
    }

    bool ConsumeBookContainerClose() {
        const bool close = s_containerClose;
        s_containerClose = false;
        return close;
    }

} // namespace Render
