// File: src/client/renderer/gui/screens/ShaderOptionsScreen.cpp
#include "ShaderOptionsScreen.hpp"

#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "../../shader/ShaderPipeline.hpp"
#include "client/shader/ShaderPacks.hpp"
#include "platform/GameDirectory.hpp"

#include <algorithm>
#include <cmath>

namespace Render {

    // ── list ─────────────────────────────────────────────────────────────

    ShaderOptionList::ShaderOptionList(ShaderOptionsScreen& screen, int x, int y, int width, int height)
        : AbstractWidget(x, y, width, height, ""), m_screen(screen) {}

    int ShaderOptionList::ContentHeight() const {
        return static_cast<int>(m_screen.Options().size()) * ROW_H + 4;
    }

    double ShaderOptionList::MaxScroll() const {
        return std::max(0, ContentHeight() - m_height);
    }

    int ShaderOptionList::RowAt(double mouseX, double mouseY) const {
        if (!ContainsPoint(mouseX, mouseY)) return -1;
        const int local = static_cast<int>(mouseY - m_y + m_scroll) - 2;
        if (local < 0) return -1;
        const int row = local / ROW_H;
        return row < static_cast<int>(m_screen.Options().size()) ? row : -1;
    }

    void ShaderOptionList::OnClick(double mouseX, double mouseY) {
        const int row = RowAt(mouseX, mouseY);
        if (row >= 0) m_screen.Cycle(static_cast<size_t>(row));
    }

    bool ShaderOptionList::OnScroll(double deltaY) {
        if (MaxScroll() <= 0.0) return false;
        m_scroll = std::clamp(m_scroll - deltaY * ROW_H, 0.0, MaxScroll());
        return true;
    }

    const std::vector<std::string>* ShaderOptionList::TooltipAt(double mx, double my) {
        const int row = RowAt(mx, my);
        if (row < 0) return nullptr;
        const Shaders::Option& o = m_screen.Options()[static_cast<size_t>(row)];
        m_tooltip.clear();
        if (!o.comment.empty()) m_tooltip.push_back(o.comment);
        if (o.isToggle) {
            m_tooltip.push_back(std::string("Default: ") + (o.defaultOn ? "ON" : "OFF"));
        } else {
            std::string vals = "Values:";
            for (const std::string& v : o.values) vals += " " + v;
            m_tooltip.push_back(vals);
            m_tooltip.push_back("Default: " + o.defaultValue);
        }
        return m_tooltip.empty() ? nullptr : &m_tooltip;
    }

    void ShaderOptionList::RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float /*partialTick*/) {
        g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);
        g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);
        const int hovered = RowAt(mouseX, mouseY);
        const auto& options = m_screen.Options();
        for (size_t i = 0; i < options.size(); ++i) {
            const Shaders::Option& o = options[i];
            const int top = m_y + 2 + static_cast<int>(i) * ROW_H - static_cast<int>(std::lround(m_scroll));
            if (top + ROW_H < m_y || top > m_y + m_height) continue;
            const bool hover = static_cast<int>(i) == hovered;
            // A button-like row: the option's name left, its value right.
            g.Fill(m_x + 2, top, m_x + m_width - 8, top + ROW_H - 2, hover ? 0xFF808080 : 0xFF6F6F6F);
            g.Fill(m_x + 3, top + 1, m_x + m_width - 9, top + ROW_H - 3, hover ? 0xFF7A7A7A : 0xFF5A5A5A);
            const std::string value = Shaders::CurrentValue(o, m_screen.Overrides());
            const bool changed = m_screen.Overrides().count(o.name) > 0;
            g.DrawString(o.name, m_x + 8, top + (ROW_H - FontRenderer::LINE_HEIGHT) / 2 - 1,
                         hover ? 0xFFFFFFA0 : 0xFFE0E0E0);
            const std::string shown = value;
            const int valueX = m_x + m_width - 14 - g.GetStringWidth(shown);
            g.DrawString(shown, valueX, top + (ROW_H - FontRenderer::LINE_HEIGHT) / 2 - 1,
                         changed ? 0xFFFFFF55 : (value == "OFF" ? 0xFFA0A0A0 : 0xFF80FF80));
        }
        g.DisableScissor();
        if (MaxScroll() > 0.0) {
            const int barX = m_x + m_width - 6;
            g.Fill(barX, m_y, barX + 6, m_y + m_height, 0xFF000000);
            const int thumbH = std::max(32, static_cast<int>(static_cast<double>(m_height) * m_height / ContentHeight()));
            const int thumbY = m_y + static_cast<int>(m_scroll / MaxScroll() * (m_height - thumbH));
            g.Fill(barX, thumbY, barX + 6, thumbY + thumbH, 0xFF808080);
            g.Fill(barX, thumbY, barX + 5, thumbY + thumbH - 1, 0xFFC0C0C0);
        }
    }

    // ── screen ───────────────────────────────────────────────────────────

    void ShaderOptionsScreen::Init() {
        Shaders::PackInfo pack;
        if (Shaders::Find(Shaders::Selected(), pack)) {
            m_packName = pack.name;
            std::string root, error;
            if (Shaders::Prepare(pack, root, error)) {
                m_options = Shaders::Discover(root);
                m_overrides = Shaders::LoadOverrides(m_packName);
            }
        }
        m_list = AddWidget(new ShaderOptionList(*this, m_width / 2 - LIST_W / 2, HEADER_H,
                                                LIST_W, m_height - HEADER_H - FOOTER_H));
        const int w = 150, gap = 8;
        const int footerY = m_height - FOOTER_H / 2 - 10;
        const int left = m_width / 2 - (w * 2 + gap) / 2;
        AddWidget(new Button(left, footerY, w, 20, "Reset to Defaults", [this] { ResetAll(); }));
        AddWidget(new Button(left + w + gap, footerY, w, 20, "Done", [this] { OnClose(); }));
    }

    void ShaderOptionsScreen::Cycle(size_t index) {
        if (index >= m_options.size()) return;
        const Shaders::Option& o = m_options[index];
        if (o.isToggle) {
            const bool on = Shaders::CurrentValue(o, m_overrides) == "ON";
            const bool next = !on;
            if (next == o.defaultOn) m_overrides.erase(o.name); else m_overrides[o.name] = next ? "true" : "false";
        } else if (!o.values.empty()) {
            const std::string cur = Shaders::CurrentValue(o, m_overrides);
            auto it = std::find(o.values.begin(), o.values.end(), cur);
            const size_t pos = it == o.values.end() ? 0 : static_cast<size_t>(it - o.values.begin());
            const std::string next = o.values[(pos + 1) % o.values.size()];
            if (next == o.defaultValue) m_overrides.erase(o.name); else m_overrides[o.name] = next;
        }
        m_dirty = true;
    }

    void ShaderOptionsScreen::ResetAll() {
        if (m_overrides.empty()) return;
        m_overrides.clear();
        m_dirty = true;
    }

    void ShaderOptionsScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        const std::string title = m_packName.empty() ? m_title : m_title + " - " + m_packName;
        g.DrawCenteredString(title, m_width / 2, (HEADER_H - FontRenderer::LINE_HEIGHT) / 2, 0xFFFFFFFF);
        RenderMenuSeparators(g, m_width, HEADER_H - 2, m_height - FOOTER_H);
        if (m_options.empty()) {
            g.DrawCenteredString("This pack declares no options", m_width / 2, m_height / 2, 0xFFA0A0A0);
        } else {
            g.DrawCenteredString("Click an option to change it. Changes apply when you press Done.",
                                 m_width / 2, m_height - FOOTER_H - FontRenderer::LINE_HEIGHT - 2, 0xFFA0A0A0);
        }
    }

    void ShaderOptionsScreen::OnClose() {
        if (m_dirty && !m_packName.empty()) {
            Shaders::SaveOverrides(m_packName, m_overrides);
            ShaderPipeline::Get().ApplySelected();
        }
        Screen::OnClose();
    }

} // namespace Render
