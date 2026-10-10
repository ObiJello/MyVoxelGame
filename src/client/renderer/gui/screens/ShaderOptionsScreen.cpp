// File: src/client/renderer/gui/screens/ShaderOptionsScreen.cpp
#include "ShaderOptionsScreen.hpp"

#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "../../shader/ShaderPipeline.hpp"
#include "client/shader/ShaderPacks.hpp"
#include "platform/GameDirectory.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace Render {

    namespace {
        // A tooltip's lines: the text wrapped at about a button's width.
        void WrapInto(std::vector<std::string>& out, const std::string& text, size_t width = 48) {
            std::string line;
            std::stringstream ss(text);
            std::string word;
            while (ss >> word) {
                if (!line.empty() && line.size() + 1 + word.size() > width) { out.push_back(line); line.clear(); }
                if (!line.empty()) line += ' ';
                line += word;
            }
            if (!line.empty()) out.push_back(line);
        }
    } // namespace

    // ── grid ─────────────────────────────────────────────────────────────

    ShaderOptionGrid::ShaderOptionGrid(ShaderOptionsScreen& screen, int x, int y, int width, int height)
        : AbstractWidget(x, y, width, height, ""), m_screen(screen) {}

    int ShaderOptionGrid::ContentHeight() const {
        const int columns = std::max(1, m_screen.Columns());
        const int rows = (static_cast<int>(m_screen.Cells().size()) + columns - 1) / columns;
        return rows * ROW_H + 4;
    }

    double ShaderOptionGrid::MaxScroll() const {
        return std::max(0, ContentHeight() - m_height);
    }

    void ShaderOptionGrid::CellRect(int cell, int& x, int& y, int& w) const {
        const int columns = std::max(1, m_screen.Columns());
        const int row = cell / columns, col = cell % columns;
        w = (m_width - CELL_GAP * (columns - 1)) / columns;
        x = m_x + col * (w + CELL_GAP);
        y = m_y + 2 + row * ROW_H - static_cast<int>(std::lround(m_scroll));
    }

    int ShaderOptionGrid::CellAt(double mouseX, double mouseY) const {
        if (mouseX < m_x || mouseX >= m_x + m_width || mouseY < m_y || mouseY >= m_y + m_height) return -1;
        for (size_t i = 0; i < m_screen.Cells().size(); ++i) {
            int x, y, w;
            CellRect(static_cast<int>(i), x, y, w);
            if (mouseX >= x && mouseX < x + w && mouseY >= y && mouseY < y + CELL_H) return static_cast<int>(i);
        }
        return -1;
    }

    void ShaderOptionGrid::OnClick(double mouseX, double mouseY) {
        const int cell = CellAt(mouseX, mouseY);
        if (cell < 0) return;
        const ShaderOptionsScreen::Cell& c = m_screen.Cells()[static_cast<size_t>(cell)];
        switch (c.kind) {
            case ShaderOptionsScreen::Cell::Option:
                PlayDownSound();
                if (m_screen.IsSlider(c.option)) {
                    int x, y, w;
                    CellRect(cell, x, y, w);
                    m_dragCell = cell;
                    m_screen.SetSliderNorm(c.option, (mouseX - (x + 4)) / std::max(1, w - 8));
                } else {
                    m_screen.CycleOption(c.option, +1);
                }
                break;
            case ShaderOptionsScreen::Cell::Link:
                PlayDownSound();
                m_screen.OpenScreen(c.screen);
                m_scroll = 0.0;
                break;
            case ShaderOptionsScreen::Cell::Profile:
                PlayDownSound();
                m_screen.CycleProfile();
                break;
            case ShaderOptionsScreen::Cell::Empty:
                break;
        }
    }

    void ShaderOptionGrid::OnDrag(double mouseX, double /*mouseY*/) {
        if (m_dragCell < 0 || m_dragCell >= static_cast<int>(m_screen.Cells().size())) return;
        const ShaderOptionsScreen::Cell& c = m_screen.Cells()[static_cast<size_t>(m_dragCell)];
        if (c.kind != ShaderOptionsScreen::Cell::Option) return;
        int x, y, w;
        CellRect(m_dragCell, x, y, w);
        m_screen.SetSliderNorm(c.option, (mouseX - (x + 4)) / std::max(1, w - 8));
    }

    void ShaderOptionGrid::OnRelease(double, double) { m_dragCell = -1; }

    bool ShaderOptionGrid::OnScroll(double deltaY) {
        if (MaxScroll() <= 0.0) return false;
        m_scroll = std::clamp(m_scroll - deltaY * ROW_H, 0.0, MaxScroll());
        return true;
    }

    const std::vector<std::string>* ShaderOptionGrid::TooltipAt(double mx, double my) {
        const int cell = CellAt(mx, my);
        if (cell < 0) return nullptr;
        const ShaderOptionsScreen::Cell& c = m_screen.Cells()[static_cast<size_t>(cell)];
        const Shaders::PackLayout& layout = m_screen.Layout();
        m_tooltip.clear();
        if (c.kind == ShaderOptionsScreen::Cell::Option) {
            const Shaders::Option& o = m_screen.Options()[static_cast<size_t>(c.option)];
            const std::string comment = layout.OptionComment(o.name);
            if (!comment.empty()) WrapInto(m_tooltip, comment);
            else if (!o.comment.empty()) WrapInto(m_tooltip, o.comment);
            if (o.isToggle) {
                m_tooltip.push_back(std::string("Default: ") + (o.defaultOn ? "ON" : "OFF"));
            } else {
                m_tooltip.push_back("Default: " + layout.ValueLabel(o.name, o.defaultValue));
            }
            if (m_tooltip.size() == 1 && layout.lang.empty()) m_tooltip.insert(m_tooltip.begin(), o.name);
        } else if (c.kind == ShaderOptionsScreen::Cell::Link) {
            const std::string comment = layout.ScreenComment(c.screen);
            if (!comment.empty()) WrapInto(m_tooltip, comment);
        } else if (c.kind == ShaderOptionsScreen::Cell::Profile) {
            m_tooltip.push_back("A preset of the pack's options.");
            m_tooltip.push_back("Changing any option makes it Custom.");
        }
        return m_tooltip.empty() ? nullptr : &m_tooltip;
    }

    void ShaderOptionGrid::RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float /*partialTick*/) {
        g.Fill(m_x - 4, m_y, m_x + m_width + 4, m_y + m_height, 0x77000000);
        g.EnableScissor(m_x - 4, m_y, m_x + m_width + 4, m_y + m_height);
        const int hovered = CellAt(mouseX, mouseY);
        const auto& cells = m_screen.Cells();
        for (size_t i = 0; i < cells.size(); ++i) {
            const ShaderOptionsScreen::Cell& c = cells[i];
            if (c.kind == ShaderOptionsScreen::Cell::Empty) continue;
            int x, y, w;
            CellRect(static_cast<int>(i), x, y, w);
            if (y + CELL_H < m_y || y > m_y + m_height) continue;
            const bool hover = static_cast<int>(i) == hovered;
            const bool slider = c.kind == ShaderOptionsScreen::Cell::Option && m_screen.IsSlider(c.option);
            const std::string label = m_screen.CellLabel(c);
            if (slider) {
                // MC's slider: a dark track, the handle at the value.
                g.Fill(x, y, x + w, y + CELL_H, 0xFF000000);
                g.Fill(x + 1, y + 1, x + w - 1, y + CELL_H - 1, hover ? 0xFF5A5A5A : 0xFF4A4A4A);
                const int handleW = 8;
                const int handleX = x + 4 + static_cast<int>(std::lround(m_screen.SliderNorm(c.option) * (w - 8 - handleW)));
                g.Fill(handleX, y + 1, handleX + handleW, y + CELL_H - 1, hover ? 0xFFC0C0C0 : 0xFF9A9A9A);
                g.Fill(handleX + 1, y + 2, handleX + handleW - 1, y + CELL_H - 2, hover ? 0xFFE8E8E8 : 0xFFBEBEBE);
            } else {
                g.Fill(x, y, x + w, y + CELL_H, hover ? 0xFF808080 : 0xFF6F6F6F);
                g.Fill(x + 1, y + 1, x + w - 1, y + CELL_H - 1, hover ? 0xFF7A7A7A : 0xFF5A5A5A);
            }
            const bool changed = c.kind == ShaderOptionsScreen::Cell::Option && m_screen.OptionChanged(c.option);
            const uint32_t color = changed ? 0xFFFFFF55 : (hover ? 0xFFFFFFA0 : 0xFFE0E0E0);
            // Centred, clipped to the cell.
            std::string shown = label;
            while (shown.size() > 1 && g.GetStringWidth(shown) > w - 8) shown.pop_back();
            if (shown.size() < label.size() && shown.size() > 3) shown = shown.substr(0, shown.size() - 2) + "..";
            g.DrawCenteredString(shown, x + w / 2, y + (CELL_H - FontRenderer::LINE_HEIGHT) / 2 + 1, color);
        }
        g.DisableScissor();
        if (MaxScroll() > 0.0) {
            const int barX = m_x + m_width + 6;
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
                m_layout = Shaders::LoadLayout(root);
            }
        }
        if (m_path.empty()) m_path.push_back("");
        m_grid = AddWidget(new ShaderOptionGrid(*this, m_width / 2 - GRID_W / 2, HEADER_H,
                                                GRID_W, m_height - HEADER_H - FOOTER_H));
        const int w = 100, gap = 8;
        const int footerY = m_height - FOOTER_H / 2 - 10;
        const int left = m_width / 2 - (w * 3 + gap * 2) / 2;
        m_backButton = AddWidget(new Button(left, footerY, w, 20, "Back", [this] { Back(); }));
        AddWidget(new Button(left + w + gap, footerY, w, 20, "Reset to Defaults", [this] { ResetAll(); }));
        AddWidget(new Button(left + (w + gap) * 2, footerY, w, 20, "Done", [this] { OnClose(); }));
        BuildPage();
    }

    void ShaderOptionsScreen::BuildPage() {
        m_cells.clear();
        const std::string& screen = m_path.back();
        auto optionIndex = [&](const std::string& name) {
            for (size_t i = 0; i < m_options.size(); ++i) if (m_options[i].name == name) return static_cast<int>(i);
            return -1;
        };
        auto addOption = [&](int index) { Cell c; c.kind = Cell::Option; c.option = index; m_cells.push_back(c); };
        if (!m_layout.HasLayout()) {
            // No layout: every option, in the order declared.
            m_columns = 2;
            for (size_t i = 0; i < m_options.size(); ++i) addOption(static_cast<int>(i));
        } else if (screen == kOtherScreen) {
            // The options no screen places.
            m_columns = 2;
            std::set<std::string> placed;
            for (const auto& [name, items] : m_layout.screens) for (const std::string& it : items) placed.insert(it);
            for (size_t i = 0; i < m_options.size(); ++i) if (!placed.count(m_options[i].name)) addOption(static_cast<int>(i));
        } else {
            auto cols = m_layout.columns.find(screen);
            m_columns = cols != m_layout.columns.end() ? cols->second : 2;
            auto items = m_layout.screens.find(screen);
            if (items != m_layout.screens.end()) {
                for (const std::string& item : items->second) {
                    Cell c;
                    if (item == "<empty>") {
                        c.kind = Cell::Empty;
                    } else if (item == "<profile>") {
                        if (m_layout.profiles.empty()) continue;
                        c.kind = Cell::Profile;
                    } else if (item.size() > 2 && item.front() == '[' && item.back() == ']') {
                        c.kind = Cell::Link;
                        c.screen = item.substr(1, item.size() - 2);
                        if (!m_layout.screens.count(c.screen)) continue;   // a link to nothing
                    } else {
                        const int index = optionIndex(item);
                        if (index < 0) continue;   // an option the pack no longer declares
                        c.kind = Cell::Option;
                        c.option = index;
                    }
                    m_cells.push_back(c);
                }
            }
            if (screen.empty()) {
                // The main screen: the profiles when the layout forgot the
                // button, and the options no screen places.
                bool hasProfile = false;
                for (const Cell& c : m_cells) hasProfile = hasProfile || c.kind == Cell::Profile;
                if (!hasProfile && !m_layout.profiles.empty()) {
                    Cell c; c.kind = Cell::Profile; m_cells.insert(m_cells.begin(), c);
                }
                std::set<std::string> placed;
                for (const auto& [name, its] : m_layout.screens) for (const std::string& it : its) placed.insert(it);
                bool unplaced = false;
                for (const Shaders::Option& o : m_options) unplaced = unplaced || !placed.count(o.name);
                if (unplaced) {
                    if (m_cells.size() % static_cast<size_t>(std::max(1, m_columns)) != 0) { Cell e; m_cells.push_back(e); }
                    Cell c; c.kind = Cell::Link; c.screen = kOtherScreen; m_cells.push_back(c);
                }
            }
        }
        if (m_backButton) m_backButton->active = m_path.size() > 1;
        if (m_grid) m_grid->ResetScroll();
    }

    std::string ShaderOptionsScreen::CellLabel(const Cell& cell) const {
        switch (cell.kind) {
            case Cell::Option: {
                const Shaders::Option& o = m_options[static_cast<size_t>(cell.option)];
                const std::string value = Shaders::CurrentValue(o, m_overrides);
                const std::string shownValue = o.isToggle ? value : m_layout.ValueLabel(o.name, value);
                return m_layout.OptionLabel(o.name) + ": " + shownValue;
            }
            case Cell::Link:
                return (cell.screen == kOtherScreen ? std::string("Other Options") : m_layout.ScreenLabel(cell.screen)) + "...";
            case Cell::Profile: {
                const int p = m_layout.MatchingProfile(m_options, m_overrides);
                return "Profile: " + (p < 0 ? std::string("Custom") : m_layout.ProfileLabel(m_layout.profiles[static_cast<size_t>(p)].name));
            }
            case Cell::Empty:
                break;
        }
        return "";
    }

    bool ShaderOptionsScreen::IsSlider(int option) const {
        if (option < 0 || option >= static_cast<int>(m_options.size())) return false;
        const Shaders::Option& o = m_options[static_cast<size_t>(option)];
        return !o.isToggle && o.values.size() > 1 && m_layout.sliders.count(o.name) > 0;
    }

    double ShaderOptionsScreen::SliderNorm(int option) const {
        const Shaders::Option& o = m_options[static_cast<size_t>(option)];
        const std::string cur = Shaders::CurrentValue(o, m_overrides);
        auto it = std::find(o.values.begin(), o.values.end(), cur);
        const size_t pos = it == o.values.end() ? 0 : static_cast<size_t>(it - o.values.begin());
        return o.values.size() > 1 ? static_cast<double>(pos) / static_cast<double>(o.values.size() - 1) : 0.0;
    }

    bool ShaderOptionsScreen::OptionChanged(int option) const {
        return option >= 0 && option < static_cast<int>(m_options.size()) &&
               m_overrides.count(m_options[static_cast<size_t>(option)].name) > 0;
    }

    void ShaderOptionsScreen::SetValue(int option, const std::string& value) {
        const Shaders::Option& o = m_options[static_cast<size_t>(option)];
        const bool isDefault = o.isToggle ? (value == (o.defaultOn ? "true" : "false")) : value == o.defaultValue;
        if (isDefault) m_overrides.erase(o.name); else m_overrides[o.name] = value;
        m_dirty = true;
    }

    void ShaderOptionsScreen::CycleOption(int option, int direction) {
        if (option < 0 || option >= static_cast<int>(m_options.size())) return;
        const Shaders::Option& o = m_options[static_cast<size_t>(option)];
        if (o.isToggle) {
            const bool on = Shaders::CurrentValue(o, m_overrides) == "ON";
            SetValue(option, on ? "false" : "true");
        } else if (!o.values.empty()) {
            const std::string cur = Shaders::CurrentValue(o, m_overrides);
            auto it = std::find(o.values.begin(), o.values.end(), cur);
            const int n = static_cast<int>(o.values.size());
            const int pos = it == o.values.end() ? 0 : static_cast<int>(it - o.values.begin());
            SetValue(option, o.values[static_cast<size_t>(((pos + direction) % n + n) % n)]);
        }
    }

    void ShaderOptionsScreen::SetSliderNorm(int option, double norm) {
        if (!IsSlider(option)) return;
        const Shaders::Option& o = m_options[static_cast<size_t>(option)];
        const int n = static_cast<int>(o.values.size());
        const int pos = static_cast<int>(std::lround(std::clamp(norm, 0.0, 1.0) * (n - 1)));
        const std::string& value = o.values[static_cast<size_t>(pos)];
        if (Shaders::CurrentValue(o, m_overrides) != value) SetValue(option, value);
    }

    void ShaderOptionsScreen::OpenScreen(const std::string& screen) {
        m_path.push_back(screen);
        BuildPage();
    }

    void ShaderOptionsScreen::Back() {
        if (m_path.size() <= 1) return;
        m_path.pop_back();
        BuildPage();
    }

    void ShaderOptionsScreen::CycleProfile() {
        if (m_layout.profiles.empty()) return;
        const int cur = m_layout.MatchingProfile(m_options, m_overrides);
        const size_t next = static_cast<size_t>((cur + 1) % static_cast<int>(m_layout.profiles.size()));
        // A profile is its values over the defaults (OptiFine resets the rest).
        m_overrides.clear();
        for (const auto& [name, value] : m_layout.profiles[next].values) {
            auto it = std::find_if(m_options.begin(), m_options.end(), [&](const Shaders::Option& o) { return o.name == name; });
            if (it == m_options.end()) continue;
            SetValue(static_cast<int>(it - m_options.begin()), value);
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
        std::string title = m_packName.empty() ? m_title : m_packName;
        for (size_t i = 1; i < m_path.size(); ++i) {
            title += " > " + (m_path[i] == kOtherScreen ? std::string("Other Options") : m_layout.ScreenLabel(m_path[i]));
        }
        g.DrawCenteredString(title, m_width / 2, (HEADER_H - FontRenderer::LINE_HEIGHT) / 2, 0xFFFFFFFF);
        RenderMenuSeparators(g, m_width, HEADER_H - 2, m_height - FOOTER_H);
        if (m_options.empty()) {
            g.DrawCenteredString("This pack declares no options", m_width / 2, m_height / 2, 0xFFA0A0A0);
        } else {
            g.DrawCenteredString("Click to change, drag a slider. Changes apply when you press Done.",
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
