// File: src/client/renderer/gui/screens/Widgets.cpp
#include "Widgets.hpp"
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <memory>

namespace Render {

    uint32_t ApplyAlpha(uint32_t argb, float alpha) {
        if (alpha >= 1.0f) return argb;
        if (alpha < 0.0f) alpha = 0.0f;
        uint32_t a = (argb >> 24) & 0xFF;
        a = static_cast<uint32_t>(static_cast<float>(a) * alpha);
        return (a << 24) | (argb & 0x00FFFFFF);
    }

    // ── AbstractWidget ──────────────────────────────────────────────────────

    void AbstractWidget::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        if (!visible) return;
        m_hovered = mouseX >= m_x && mouseX < m_x + m_width &&
                    mouseY >= m_y && mouseY < m_y + m_height;
        RenderWidget(g, mouseX, mouseY, partialTick);
    }

    void AbstractWidget::RenderButtonChrome(GuiGraphics& g, const std::string& label) const {
        const char* sprite = !active               ? "widget/button_disabled"
                           : (m_hovered || m_focused) ? "widget/button_highlighted"
                                                      : "widget/button";
        g.BlitSprite(sprite, m_x, m_y, m_width, m_height, ApplyAlpha(0xFFFFFFFF, m_alpha));

        uint32_t color = active ? WidgetDims::TEXT_COLOR_ACTIVE
                                : WidgetDims::TEXT_COLOR_INACTIVE;
        g.DrawCenteredString(label, m_x + m_width / 2,
                             m_y + (m_height - FontRenderer::LINE_HEIGHT) / 2 + 1,
                             ApplyAlpha(color, m_alpha));
    }

    // ── Button ──────────────────────────────────────────────────────────────

    void Button::RenderWidget(GuiGraphics& g, int, int, float) {
        RenderButtonChrome(g, m_message);
    }

    bool Button::KeyPressed(int glfwKey, int) {
        // MC Button.keyPressed: enter / space / numpad-enter activate.
        if (glfwKey == GLFW_KEY_ENTER || glfwKey == GLFW_KEY_KP_ENTER ||
            glfwKey == GLFW_KEY_SPACE) {
            OnClick(m_x + m_width / 2.0, m_y + m_height / 2.0);
            return true;
        }
        return false;
    }

    // ── SliderButton ────────────────────────────────────────────────────────

    namespace {
        constexpr int SLIDER_HANDLE_W = 8; // MC AbstractSliderButton HANDLE_WIDTH
    }

    void SliderButton::RenderWidget(GuiGraphics& g, int, int, float) {
        // Track: highlighted variant when keyboard-focused (MC getSprite()).
        const char* track = m_focused ? "widget/slider_highlighted" : "widget/slider";
        g.BlitSprite(track, m_x, m_y, m_width, m_height, ApplyAlpha(0xFFFFFFFF, m_alpha));

        const char* handle = (m_hovered || m_focused) ? "widget/slider_handle_highlighted"
                                                      : "widget/slider_handle";
        int handleX = m_x + static_cast<int>(m_value * static_cast<double>(m_width - SLIDER_HANDLE_W));
        g.BlitSprite(handle, handleX, m_y, SLIDER_HANDLE_W, m_height,
                     ApplyAlpha(0xFFFFFFFF, m_alpha));

        uint32_t color = active ? WidgetDims::TEXT_COLOR_ACTIVE
                                : WidgetDims::TEXT_COLOR_INACTIVE;
        g.DrawCenteredString(m_message, m_x + m_width / 2,
                             m_y + (m_height - FontRenderer::LINE_HEIGHT) / 2 + 1,
                             ApplyAlpha(color, m_alpha));
    }

    void SliderButton::SetValue(double v) {
        v = std::clamp(v, 0.0, 1.0);
        if (v == m_value) return;
        m_value = v;
        if (m_apply)  m_apply(m_value);
        if (m_format) m_message = m_format(m_value);
    }

    void SliderButton::SetValueFromMouse(double mouseX) {
        // MC AbstractSliderButton.setValueFromMouse: normalize the mouse X
        // into the travel range of the 8px handle.
        double t = (static_cast<double>(mouseX) - m_x - SLIDER_HANDLE_W * 0.5) /
                   static_cast<double>(m_width - SLIDER_HANDLE_W);
        SetValue(t);
    }

    void SliderButton::OnClick(double mouseX, double) { SetValueFromMouse(mouseX); }
    void SliderButton::OnDrag(double mouseX, double)  { SetValueFromMouse(mouseX); }
    void SliderButton::OnRelease(double, double)      {}

    bool SliderButton::KeyPressed(int glfwKey, int) {
        double step = m_keyStep > 0.0 ? m_keyStep
                                      : 1.0 / static_cast<double>(m_width - SLIDER_HANDLE_W);
        if (glfwKey == GLFW_KEY_LEFT)  { SetValue(m_value - step); return true; }
        if (glfwKey == GLFW_KEY_RIGHT) { SetValue(m_value + step); return true; }
        return false;
    }

    // ── CycleButton ─────────────────────────────────────────────────────────

    void CycleButton::Cycle(int delta) {
        int n = static_cast<int>(m_values.size());
        m_index = ((m_index + delta) % n + n) % n;
        UpdateMessage();
        if (m_onChange) m_onChange(m_index);
    }

    void CycleButton::OnClick(double, double) {
        // MC CycleButton.onClick: shift-click cycles backwards. We can't see
        // modifiers here, so Screen passes shift via KeyPressed path only;
        // plain click always advances (backwards cycling still available via
        // keyboard left arrow, below).
        Cycle(+1);
    }

    bool CycleButton::KeyPressed(int glfwKey, int) {
        if (glfwKey == GLFW_KEY_ENTER || glfwKey == GLFW_KEY_KP_ENTER ||
            glfwKey == GLFW_KEY_SPACE || glfwKey == GLFW_KEY_RIGHT) {
            Cycle(+1);
            return true;
        }
        if (glfwKey == GLFW_KEY_LEFT) { Cycle(-1); return true; }
        return false;
    }

    void CycleButton::RenderWidget(GuiGraphics& g, int, int, float) {
        RenderButtonChrome(g, m_message);
    }

    CycleButton* CycleButton::MakeOnOff(int x, int y, int width, int height,
                                        const std::string& caption, bool initial,
                                        std::function<void(bool)> onChange) {
        return new CycleButton(x, y, width, height, caption, {"ON", "OFF"},
                               initial ? 0 : 1,
                               [fn = std::move(onChange)](int idx) { if (fn) fn(idx == 0); });
    }

    // ── EditBox ─────────────────────────────────────────────────────────────

    void EditBox::SetText(const std::string& text) {
        m_text = text.substr(0, static_cast<size_t>(m_maxLength));
        m_cursorPos = static_cast<int>(m_text.size());
        NotifyChanged();
    }

    void EditBox::OnClick(double, double) { m_focused = true; }

    void EditBox::InsertText(const std::string& s) {
        for (char c : s) {
            if (static_cast<int>(m_text.size()) >= m_maxLength) break;
            // Printable ASCII only — matches the font atlas coverage.
            if (c < 32 || c > 126) continue;
            m_text.insert(m_text.begin() + m_cursorPos, c);
            ++m_cursorPos;
        }
        NotifyChanged();
    }

    void EditBox::DeleteChars(int dir) {
        if (dir < 0 && m_cursorPos > 0) {
            m_text.erase(m_text.begin() + (m_cursorPos - 1));
            --m_cursorPos;
            NotifyChanged();
        } else if (dir > 0 && m_cursorPos < static_cast<int>(m_text.size())) {
            m_text.erase(m_text.begin() + m_cursorPos);
            NotifyChanged();
        }
    }

    bool EditBox::KeyPressed(int glfwKey, int) {
        if (!m_focused) return false;
        switch (glfwKey) {
            case GLFW_KEY_BACKSPACE: DeleteChars(-1); return true;
            case GLFW_KEY_DELETE:    DeleteChars(+1); return true;
            case GLFW_KEY_LEFT:      if (m_cursorPos > 0) --m_cursorPos; return true;
            case GLFW_KEY_RIGHT:
                if (m_cursorPos < static_cast<int>(m_text.size())) ++m_cursorPos;
                return true;
            case GLFW_KEY_HOME:      m_cursorPos = 0; return true;
            case GLFW_KEY_END:       m_cursorPos = static_cast<int>(m_text.size()); return true;
            default: return false;
        }
    }

    bool EditBox::CharTyped(unsigned int codepoint) {
        if (!m_focused) return false;
        if (codepoint < 32 || codepoint > 126) return false;
        InsertText(std::string(1, static_cast<char>(codepoint)));
        return true;
    }

    void EditBox::RenderWidget(GuiGraphics& g, int, int, float) {
        const char* sprite = m_focused ? "widget/text_field_highlighted"
                                       : "widget/text_field";
        g.BlitSprite(sprite, m_x, m_y, m_width, m_height, ApplyAlpha(0xFFFFFFFF, m_alpha));

        const int textX = m_x + 4;
        const int textY = m_y + (m_height - FontRenderer::LINE_HEIGHT) / 2 + 1;

        if (m_text.empty() && !m_focused && !m_hint.empty()) {
            g.DrawString(m_hint, textX, textY, ApplyAlpha(0xFF808080, m_alpha));
            return;
        }

        g.DrawString(m_text, textX, textY, ApplyAlpha(0xFFE0E0E0, m_alpha));

        // Caret: 300ms blink cycle, only while focused (MC EditBox).
        if (m_focused) {
            bool blinkOn = (static_cast<long long>(glfwGetTime() * 1000.0) / 300) % 2 == 0;
            if (blinkOn) {
                int caretX = textX + g.GetStringWidth(m_text.substr(0, m_cursorPos));
                if (m_cursorPos == static_cast<int>(m_text.size())) {
                    // End-of-text caret is an underscore (MC).
                    g.DrawString("_", caretX, textY, ApplyAlpha(0xFFE0E0E0, m_alpha));
                } else {
                    g.Fill(caretX, textY - 1, caretX + 1,
                           textY + FontRenderer::LINE_HEIGHT, ApplyAlpha(0xFFD0D0D0, m_alpha));
                }
            }
        }
    }

    // ── StringWidget ────────────────────────────────────────────────────────

    void StringWidget::RenderWidget(GuiGraphics& g, int, int, float) {
        g.DrawCenteredString(m_message, m_x + m_width / 2,
                             m_y + (m_height - FontRenderer::LINE_HEIGHT) / 2,
                             ApplyAlpha(m_color, m_alpha));
    }

    // ── PlainTextButton ─────────────────────────────────────────────────────

    void PlainTextButton::RenderWidget(GuiGraphics& g, int, int, float) {
        uint32_t color = ApplyAlpha(0xFFFFFFFF, GetAlpha());
        g.DrawString(GetMessage(), m_x, m_y, color);
        if (m_hovered || m_focused) {
            // Vanilla underlines the copyright line on hover.
            int w = g.GetStringWidth(GetMessage());
            g.Fill(m_x, m_y + FontRenderer::LINE_HEIGHT,
                   m_x + w, m_y + FontRenderer::LINE_HEIGHT + 1, color);
        }
    }

    // ── TabButton ───────────────────────────────────────────────────────────

    void TabButton::RenderWidget(GuiGraphics& g, int, int, float) {
        const char* sprite = selected
            ? (m_hovered || m_focused ? "widget/tab_selected_highlighted" : "widget/tab_selected")
            : (m_hovered || m_focused ? "widget/tab_highlighted" : "widget/tab");
        g.BlitSprite(sprite, m_x, m_y, m_width, m_height, ApplyAlpha(0xFFFFFFFF, m_alpha));
        // MC draws unselected tab labels slightly lower (tucked under look).
        const int textY = m_y + (m_height - FontRenderer::LINE_HEIGHT) / 2 + (selected ? 0 : 2);
        g.DrawCenteredString(m_message, m_x + m_width / 2, textY,
                             ApplyAlpha(selected ? 0xFFFFFFFF : 0xFFA0A0A0, m_alpha));
    }

    // ── OptionsList ─────────────────────────────────────────────────────────

    void OptionsList::AddBig(AbstractWidget* w) {
        Row row;
        row.left = w;
        row.big = true;
        if (w) m_children.emplace_back(w);
        m_rows.push_back(row);
    }

    void OptionsList::AddSmall(AbstractWidget* left, AbstractWidget* right) {
        Row row;
        row.left = left;
        row.right = right;
        if (left)  m_children.emplace_back(left);
        if (right) m_children.emplace_back(right);
        m_rows.push_back(row);
    }

    void OptionsList::AddHeader(const std::string& text) {
        Row row;
        row.header = text;
        m_rows.push_back(row);
    }

    double OptionsList::MaxScroll() const {
        double content = static_cast<double>(m_rows.size()) * ROW_HEIGHT;
        double max = content - m_height + 4.0;
        return max > 0.0 ? max : 0.0;
    }

    void OptionsList::ClampScroll() {
        m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
    }

    void OptionsList::LayoutRow(const Row& row, int rowTop) {
        // Column layout mirrors MC OptionsList: 310px span centered, 150px
        // widgets, 10px gutter. Big rows span the full 310.
        const int center = m_x + m_width / 2;
        if (row.big) {
            if (row.left) row.left->SetPosition(center - ROW_WIDTH / 2, rowTop);
        } else {
            if (row.left)  row.left->SetPosition(center - ROW_WIDTH / 2, rowTop);
            if (row.right) row.right->SetPosition(center + 5, rowTop);
        }
    }

    AbstractWidget* OptionsList::HitTest(double mx, double my) {
        if (!ContainsPoint(mx, my)) return nullptr;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            int rowTop = m_y + 2 + static_cast<int>(i) * ROW_HEIGHT -
                         static_cast<int>(m_scroll);
            LayoutRow(m_rows[i], rowTop);
            const Row& row = m_rows[i];
            for (AbstractWidget* w : {row.left, row.right}) {
                if (w && w->IsMouseOver(mx, my)) return w;
            }
        }
        return nullptr;
    }

    const std::vector<std::string>* OptionsList::TooltipAt(double mx, double my) {
        if (!ContainsPoint(mx, my)) return nullptr;
        // Like HitTest, but by ContainsPoint so DISABLED children still show
        // their tooltip (vanilla behaviour for greyed-out options).
        for (size_t i = 0; i < m_rows.size(); ++i) {
            int rowTop = m_y + 2 + static_cast<int>(i) * ROW_HEIGHT -
                         static_cast<int>(m_scroll);
            LayoutRow(m_rows[i], rowTop);
            const Row& row = m_rows[i];
            for (AbstractWidget* w : {row.left, row.right}) {
                if (!w) continue;
                if (const auto* tip = w->TooltipAt(mx, my)) return tip;
            }
        }
        return nullptr;
    }

    void OptionsList::OnClick(double mouseX, double mouseY) {
        // Scrollbar hit?
        if (ScrollbarVisible()) {
            int sx = ScrollbarX();
            if (mouseX >= sx && mouseX < sx + SCROLLBAR_W) {
                m_draggingScrollbar = true;
                double frac = MaxScroll() > 0 ? m_scroll / MaxScroll() : 0.0;
                double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height /
                                          (static_cast<double>(m_rows.size()) * ROW_HEIGHT));
                double thumbY = m_y + frac * (m_height - thumbH);
                m_scrollbarGrabOffset = mouseY - thumbY;
                OnDrag(mouseX, mouseY);
                return;
            }
        }

        m_draggedChild = HitTest(mouseX, mouseY);
        if (m_draggedChild) m_draggedChild->OnClick(mouseX, mouseY);
    }

    void OptionsList::OnDrag(double mouseX, double mouseY) {
        if (m_draggingScrollbar) {
            double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height /
                                      (static_cast<double>(m_rows.size()) * ROW_HEIGHT));
            double track = static_cast<double>(m_height) - thumbH;
            if (track > 0.0) {
                double frac = (mouseY - m_scrollbarGrabOffset - m_y) / track;
                m_scroll = std::clamp(frac, 0.0, 1.0) * MaxScroll();
            }
            return;
        }
        if (m_draggedChild) m_draggedChild->OnDrag(mouseX, mouseY);
    }

    void OptionsList::OnRelease(double mouseX, double mouseY) {
        m_draggingScrollbar = false;
        if (m_draggedChild) {
            m_draggedChild->OnRelease(mouseX, mouseY);
            m_draggedChild = nullptr;
        }
    }

    bool OptionsList::OnScroll(double deltaY) {
        if (!ScrollbarVisible()) return false;
        // MC AbstractSelectionList: wheel scrolls half a row-height per notch
        // ×2 rows. Use one row per notch — matches modern MC feel.
        m_scroll -= deltaY * ROW_HEIGHT;
        ClampScroll();
        return true;
    }

    void OptionsList::RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        ClampScroll();

        // Dark backdrop behind the rows. menu_list_background.png lives
        // outside the sprites atlas, so use a plain fill at vanilla's
        // in-menu list darkness instead.
        g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);

        g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);
        for (size_t i = 0; i < m_rows.size(); ++i) {
            int rowTop = m_y + 2 + static_cast<int>(i) * ROW_HEIGHT -
                         static_cast<int>(m_scroll);
            if (rowTop + ROW_HEIGHT < m_y || rowTop > m_y + m_height) continue;
            Row& row = m_rows[i];
            if (!row.header.empty()) {
                g.DrawCenteredString(row.header, m_x + m_width / 2,
                                     rowTop + (ROW_HEIGHT - FontRenderer::LINE_HEIGHT) / 2,
                                     0xFFFFFFFF);
                continue;
            }
            LayoutRow(row, rowTop);
            if (row.left)  row.left->Render(g, mouseX, mouseY, partialTick);
            if (row.right) row.right->Render(g, mouseX, mouseY, partialTick);
        }
        g.DisableScissor();

        // Scrollbar (MC widget/scroller sprites).
        if (ScrollbarVisible()) {
            int sx = ScrollbarX();
            g.BlitSprite("widget/scroller_background", sx, m_y, SCROLLBAR_W, m_height);
            double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height /
                                      (static_cast<double>(m_rows.size()) * ROW_HEIGHT));
            double frac = MaxScroll() > 0 ? m_scroll / MaxScroll() : 0.0;
            int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
            g.BlitSprite("widget/scroller", sx, thumbY, SCROLLBAR_W,
                         static_cast<int>(thumbH));
        }
    }

} // namespace Render
