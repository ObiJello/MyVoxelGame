// File: src/client/renderer/gui/screens/ShaderPackScreen.cpp
#include "ShaderPackScreen.hpp"

#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "../../shader/ShaderPipeline.hpp"
#include "ShaderOptionsScreen.hpp"
#include "platform/GameDirectory.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace Render {

    // ── list ─────────────────────────────────────────────────────────────

    ShaderPackList::ShaderPackList(ShaderPackScreen& screen, int x, int y, int width, int height)
        : AbstractWidget(x, y, width, height, ""), m_screen(screen) {}

    void ShaderPackList::SetPacks(std::vector<Shaders::PackInfo> packs, const std::string& selectedId) {
        m_rows.clear();
        m_rows.push_back({"", "Off", "The engine's own rendering"});
        for (const Shaders::PackInfo& p : packs) {
            m_rows.push_back({p.id, p.name, p.isZip ? "zip" : "folder"});
        }
        m_selectedId = selectedId;
        m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
    }

    double ShaderPackList::MaxScroll() const {
        return std::max(0, ContentHeight() - m_height);
    }

    int ShaderPackList::RowAt(double mouseX, double mouseY) const {
        if (!ContainsPoint(mouseX, mouseY)) return -1;
        const int local = static_cast<int>(mouseY - m_y + m_scroll) - 2;
        if (local < 0) return -1;
        const int row = local / ROW_H;
        return row < static_cast<int>(m_rows.size()) ? row : -1;
    }

    void ShaderPackList::OnClick(double mouseX, double mouseY) {
        const int row = RowAt(mouseX, mouseY);
        if (row < 0) return;
        m_screen.Select(m_rows[static_cast<size_t>(row)].id);
    }

    bool ShaderPackList::OnScroll(double deltaY) {
        if (MaxScroll() <= 0.0) return false;
        m_scroll = std::clamp(m_scroll - deltaY * ROW_H, 0.0, MaxScroll());
        return true;
    }

    void ShaderPackList::RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float /*partialTick*/) {
        g.Fill(m_x, m_y, m_x + m_width, m_y + m_height, 0x77000000);
        g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);
        const int hovered = RowAt(mouseX, mouseY);
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const Row& r = m_rows[i];
            const int top = m_y + 2 + static_cast<int>(i) * ROW_H - static_cast<int>(std::lround(m_scroll));
            if (top + ROW_H < m_y || top > m_y + m_height) continue;
            const bool selected = r.id == m_selectedId;
            const bool hover = static_cast<int>(i) == hovered;
            if (selected) {
                g.Fill(m_x + 2, top, m_x + m_width - 2, top + ROW_H - 2, 0xFF808080);
                g.Fill(m_x + 3, top + 1, m_x + m_width - 3, top + ROW_H - 3, 0xFF000000);
            } else if (hover) {
                g.Fill(m_x + 2, top, m_x + m_width - 2, top + ROW_H - 2, 0x40FFFFFF);
            }
            g.DrawString(r.label, m_x + 8, top + 3, selected ? 0xFFFFFF80 : 0xFFFFFFFF);
            g.DrawString(r.detail, m_x + 8, top + 3 + FontRenderer::LINE_HEIGHT, 0xFF808080);
        }
        g.DisableScissor();
        // Scrollbar, MC style.
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

    void ShaderPackScreen::Init() {
        m_list = AddWidget(new ShaderPackList(*this, m_width / 2 - LIST_W / 2, HEADER_H,
                                              LIST_W, m_height - HEADER_H - FOOTER_H));
        Refresh();

        const int w = 120, gap = 6;
        const int footerY = m_height - FOOTER_H / 2 - 10;
        const int left = m_width / 2 - (w * 3 + gap * 2) / 2;
        m_settingsButton = AddWidget(new Button(left + w + gap, footerY, w, 20, "Pack Settings...", [] {
            GetScreenManager().Push(std::make_unique<ShaderOptionsScreen>());
        }));
        m_settingsButton->SetTooltip({"The pack's own options (its #define",
                                      "settings), as in Minecraft's shader",
                                      "pack settings screen."});
        m_settingsButton->active = ShaderPipeline::Get().Active();
        auto* folder = AddWidget(new Button(left, footerY, w, 20, "Packs Folder...", [] {
            const std::string dir = Shaders::PacksDirectory();
            if (!Platform::GameDirectory::OpenInFileBrowser(dir)) {
                Log::Warning("[ShaderPacks] Could not open %s in the file browser", dir.c_str());
            }
        }));
        folder->SetTooltip({"Opens your shaderpacks folder. Put a pack's",
                            "folder or .zip there; it appears in this list."});
        AddWidget(new Button(left + (w + gap) * 2, footerY, w, 20, "Done", [this] { OnClose(); }));
    }

    void ShaderPackScreen::Refresh() {
        std::vector<Shaders::PackInfo> packs = Shaders::Discover();
        m_knownIds.clear();
        for (const Shaders::PackInfo& p : packs) m_knownIds.push_back(p.id);
        if (m_list) m_list->SetPacks(std::move(packs), Shaders::Selected());
    }

    void ShaderPackScreen::Tick() {
        // Once a second, look again, so a pack dropped into the folder while
        // this screen is up appears without reopening it.
        if (++m_rescanTicks < 20) return;
        m_rescanTicks = 0;
        std::vector<std::string> ids;
        for (const Shaders::PackInfo& p : Shaders::Discover()) ids.push_back(p.id);
        if (ids != m_knownIds) Refresh();
    }

    void ShaderPackScreen::Select(const std::string& id) {
        Shaders::SetSelected(id);
        ShaderPipeline::Get().ApplySelected();
        if (m_list) m_list->SetSelected(id);
        if (m_settingsButton) m_settingsButton->active = ShaderPipeline::Get().Active();
    }

    void ShaderPackScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        g.DrawCenteredString(m_title, m_width / 2, 8, 0xFFFFFFFF);
        const std::string& status = ShaderPipeline::Get().Status();
        const bool active = ShaderPipeline::Get().Active();
        g.DrawCenteredString(status, m_width / 2, 8 + FontRenderer::LINE_HEIGHT + 4,
                             active ? 0xFF80FF80 : (status == "Off" ? 0xFF808080 : 0xFFFF8080));
        RenderMenuSeparators(g, m_width, HEADER_H - 2, m_height - FOOTER_H);
        g.DrawCenteredString("OpenGL only. Terrain, water, entities, sky, clouds, particles and shadows go through the pack; the hand does not yet",
                             m_width / 2, m_height - FOOTER_H - FontRenderer::LINE_HEIGHT - 2, 0xFFA0A0A0);
    }

    void ShaderPackScreen::OnClose() {
        Platform::g_gameSettings.Save();
        Screen::OnClose();
    }

} // namespace Render
