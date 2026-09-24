// File: src/client/renderer/gui/debug/DebugOptionsScreen.cpp
#include "DebugOptionsScreen.hpp"
#include "../GuiGraphics.hpp"
#include "../FontRenderer.hpp"
#include "platform/GameDirectory.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <vector>

namespace Render {

    using namespace DebugScreen;

    namespace {
        constexpr uint32_t kWarningColor  = 0xFFDF5050;   // -2142128
        constexpr uint32_t kAlwaysOn      = 0xFFDF5050;
        constexpr uint32_t kInOverlayOn   = 0xFFFFFF55;   // -171
        constexpr uint32_t kOffOn         = 0xFFFFFFFF;   // -1
        constexpr uint32_t kUnselected    = 0xFFBABABA;   // -4539718
        const char* kWarning = "These options are for testing purposes only. They may slow down your computer, crash the game, or eat your pet rock.";

        // Word-wrap to a pixel width (MC MultiLineTextWidget).
        std::vector<std::string> Wrap(GuiGraphics& g, const std::string& text, int maxWidth) {
            std::vector<std::string> lines;
            std::string line, word;
            auto flushWord = [&]() {
                if (word.empty()) return;
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (g.GetStringWidth(candidate) > maxWidth && !line.empty()) { lines.push_back(line); line = word; }
                else line = candidate;
                word.clear();
            };
            for (char c : text) {
                if (c == ' ') flushWord(); else word += c;
            }
            flushWord();
            if (!line.empty()) lines.push_back(line);
            return lines;
        }
    }

    // ── DebugOptionList ─────────────────────────────────────────────────

    DebugOptionList::DebugOptionList(int x, int y, int width, int height)
        : AbstractWidget(x, y, width, height, "") {
        m_notAllowedTooltip = { "Not visible when debug info is reduced." };
        UpdateSearch("");
    }

    void DebugOptionList::UpdateSearch(const std::string& value) {
        m_filter = value;
        m_rows.clear();
        // Sorted by category then id — the registry map is already id-ordered.
        std::vector<std::pair<const std::string*, const Entry*>> all;
        for (const auto& [id, entry] : AllEntries()) all.emplace_back(&id, entry.get());
        std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
            return static_cast<int>(a.second->GetCategory()) < static_cast<int>(b.second->GetCategory());
        });
        const bool reduced = EntryList::ShowOnlyReducedInfo();
        int currentCategory = -1;
        for (const auto& [id, entry] : all) {
            if (id->find(value) == std::string::npos) continue;
            const int cat = static_cast<int>(entry->GetCategory());
            if (cat != currentCategory) {
                Row header;
                header.header = true;
                header.label = CategoryLabel(entry->GetCategory());
                m_rows.push_back(header);
                currentCategory = cat;
            }
            Row row;
            row.id = *id;
            row.label = *id;
            row.allowed = entry->IsAllowed(reduced);
            row.status = Entries().GetStatus(*id);
            m_rows.push_back(row);
        }
        m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
    }

    void DebugOptionList::RefreshEntries() {
        for (Row& r : m_rows) if (!r.header) r.status = Entries().GetStatus(r.id);
    }

    double DebugOptionList::MaxScroll() const {
        const double max = ContentHeight() - m_height;
        return max > 0.0 ? max : 0.0;
    }

    int DebugOptionList::RowTop(int index) const {
        return m_y + 4 + index * ITEM_HEIGHT - static_cast<int>(m_scroll);
    }

    int DebugOptionList::RowAt(double mouseX, double mouseY) const {
        if (!ContainsPoint(mouseX, mouseY)) return -1;
        if (mouseX < ContentX() || mouseX >= ContentX() + ContentWidth()) return -1;
        const double rel = mouseY - (m_y + 4) + m_scroll;
        if (rel < 0) return -1;
        const int idx = static_cast<int>(rel / ITEM_HEIGHT);
        return idx < static_cast<int>(m_rows.size()) ? idx : -1;
    }

    bool DebugOptionList::OnScroll(double deltaY) {
        if (MaxScroll() <= 0.0) return false;
        m_scroll = std::clamp(m_scroll - deltaY * (ITEM_HEIGHT / 2.0), 0.0, MaxScroll());
        return true;
    }

    void DebugOptionList::OnClick(double mouseX, double mouseY) {
        if (Scrollable() && mouseX >= ScrollbarX() && mouseX < ScrollbarX() + SCROLLBAR) {
            m_draggingScrollbar = true;
            const double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height / ContentHeight());
            const double frac = MaxScroll() > 0.0 ? m_scroll / MaxScroll() : 0.0;
            const double thumbY = m_y + frac * (m_height - thumbH);
            m_scrollbarGrab = (mouseY >= thumbY && mouseY < thumbY + thumbH) ? mouseY - thumbY : thumbH / 2.0;
            OnDrag(mouseX, mouseY);
            return;
        }
        const int idx = RowAt(mouseX, mouseY);
        if (idx < 0) return;
        Row& row = m_rows[static_cast<size_t>(idx)];
        if (row.header) return;
        const int top = RowTop(idx);
        const int by = top + (ITEM_HEIGHT - BUTTON_HEIGHT) / 2;
        if (mouseY < by || mouseY >= by + BUTTON_HEIGHT) return;
        const int bx = ButtonsStartX();
        if (mouseX < bx) return;
        const int slot = static_cast<int>((mouseX - bx) / BUTTON_WIDTH);
        static const EntryStatus kBySlot[3] = { EntryStatus::Never, EntryStatus::InOverlay, EntryStatus::AlwaysOn };
        if (slot < 0 || slot > 2) return;
        if (row.status == kBySlot[slot]) return;   // the selected button is inactive
        Entries().SetStatus(row.id, kBySlot[slot]);
        row.status = kBySlot[slot];
        if (onStatusChanged) onStatusChanged();
    }

    void DebugOptionList::OnDrag(double, double mouseY) {
        if (!m_draggingScrollbar) return;
        const double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height / ContentHeight());
        const double track = m_height - thumbH;
        if (track <= 0.0) return;
        const double frac = std::clamp((mouseY - m_scrollbarGrab - m_y) / track, 0.0, 1.0);
        m_scroll = frac * MaxScroll();
    }

    void DebugOptionList::OnRelease(double, double) { m_draggingScrollbar = false; }

    const std::vector<std::string>* DebugOptionList::TooltipAt(double mx, double my) {
        const int idx = RowAt(mx, my);
        if (idx < 0) return nullptr;
        const Row& row = m_rows[static_cast<size_t>(idx)];
        if (row.header || row.allowed || mx >= ButtonsStartX()) return nullptr;
        return &m_notAllowedTooltip;
    }

    void DebugOptionList::RenderToggle(GuiGraphics& g, int x, int y, const std::string& text, uint32_t onColor,
                                       bool selected, bool hovered) const {
        // MC CycleButton.booleanBuilder(...).displayOnlyValue(): the value
        // text alone; the selected state's button is `active = false`.
        const char* sprite = selected ? "widget/button_disabled" : (hovered ? "widget/button_highlighted" : "widget/button");
        g.BlitSprite(sprite, x, y, BUTTON_WIDTH, BUTTON_HEIGHT);
        const uint32_t color = selected ? onColor : kUnselected;
        g.DrawCenteredString(text, x + BUTTON_WIDTH / 2, y + (BUTTON_HEIGHT - FontRenderer::LINE_HEIGHT) / 2 + 1, color);
    }

    void DebugOptionList::RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float) {
        m_scroll = std::clamp(m_scroll, 0.0, MaxScroll());
        g.EnableScissor(m_x, m_y, m_x + m_width, m_y + m_height);
        const int hoveredRow = RowAt(mouseX, mouseY);
        for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
            const Row& row = m_rows[static_cast<size_t>(i)];
            const int top = RowTop(i);
            if (top + ITEM_HEIGHT < m_y || top > m_y + m_height) continue;
            const int x = ContentX();
            if (row.header) {
                // FocusableTextWidget: the category label, centred, in
                // MC's BOLD + UNDERLINE style (one string with the codes —
                // the font draws bold as its second copy, shadows first).
                const std::string label = "\xC2\xA7l\xC2\xA7n" + row.label;
                const int w = g.GetStringWidth(label);
                const int tx = x + ContentWidth() / 2 - w / 2;
                const int ty = top + ITEM_HEIGHT / 2 - FontRenderer::LINE_HEIGHT / 2;
                g.DrawString(label, tx, ty, 0xFFFFFFFF);
                continue;
            }
            g.DrawString(row.label, x, top + 5, row.allowed ? 0xFFFFFFFF : 0xFF808080);
            const int by = top + (ITEM_HEIGHT - BUTTON_HEIGHT) / 2;
            const int bx = ButtonsStartX();
            const bool inButtons = hoveredRow == i && mouseY >= by && mouseY < by + BUTTON_HEIGHT;
            const int hoverSlot = inButtons && mouseX >= bx ? static_cast<int>((mouseX - bx) / BUTTON_WIDTH) : -1;
            RenderToggle(g, bx,                    by, "Off",        kOffOn,       row.status == EntryStatus::Never,     hoverSlot == 0);
            RenderToggle(g, bx + BUTTON_WIDTH,     by, "In Overlay", kInOverlayOn, row.status == EntryStatus::InOverlay, hoverSlot == 1);
            RenderToggle(g, bx + 2 * BUTTON_WIDTH, by, "Always",     kAlwaysOn,    row.status == EntryStatus::AlwaysOn,  hoverSlot == 2);
        }
        g.DisableScissor();

        if (Scrollable()) {
            const int sx = ScrollbarX();
            g.BlitSprite("widget/scroller_background", sx, m_y, SCROLLBAR, m_height);
            const double thumbH = std::max(32.0, static_cast<double>(m_height) * m_height / ContentHeight());
            const double frac = MaxScroll() > 0.0 ? m_scroll / MaxScroll() : 0.0;
            const int thumbY = m_y + static_cast<int>(frac * (m_height - thumbH));
            g.BlitSprite("widget/scroller", sx, thumbY, SCROLLBAR, static_cast<int>(thumbH));
        }
    }

    // ── DebugOptionsScreen ──────────────────────────────────────────────

    DebugOptionsScreen::DebugOptionsScreen() : Screen("Debug Options") {}

    std::string DebugOptionsScreen::GuiScaleLabel() const {
        const int v = Platform::g_gameSettings.GetInt("debugGuiScale", -1);
        if (v == -1) return "GUI Scale: Unchanged";
        if (v == 0) return "GUI Scale: Auto";
        return "GUI Scale: " + std::to_string(v);
    }

    void DebugOptionsScreen::Init() {
        const int listWidth = DebugOptionList::ROW_WIDTH;
        // Header: title row (spacer | title | search box) then the warning,
        // stacked with 8 px spacing inside a 61 px header.
        const int titleY = 8;
        m_search = AddWidget(new EditBox(m_width / 2 + listWidth / 6 + 4, titleY, listWidth / 3, 20, ""));
        m_search->SetHint("Search...");
        m_search->SetText(m_filter);
        m_search->SetResponder([this](const std::string& v) { m_filter = v; if (m_list) m_list->UpdateSearch(v); });

        m_list = AddWidget(new DebugOptionList(0, HEADER_HEIGHT, m_width, m_height - HEADER_HEIGHT - FOOTER_HEIGHT));
        m_list->UpdateSearch(m_filter);
        m_list->onStatusChanged = [this] { RefreshProfileButtons(); };

        // Footer: LinearLayout.horizontal().spacing(4): 120 | 120 | 110 | 60.
        const int total = 120 + 4 + 120 + 4 + 110 + 4 + 60;
        int x = m_width / 2 - total / 2;
        const int footerY = m_height - FOOTER_HEIGHT / 2 - 10;
        m_defaultProfile = AddWidget(new Button(x, footerY, 120, 20, ProfileLabel(Profile::Default), [this] {
            Entries().LoadProfile(Profile::Default);
            Entries().Save();
            if (m_list) m_list->RefreshEntries();
            RefreshProfileButtons();
        }));
        x += 124;
        m_performanceProfile = AddWidget(new Button(x, footerY, 120, 20, ProfileLabel(Profile::Performance), [this] {
            Entries().LoadProfile(Profile::Performance);
            Entries().Save();
            if (m_list) m_list->RefreshEntries();
            RefreshProfileButtons();
        }));
        x += 124;
        m_guiScale = AddWidget(new Button(x, footerY, 110, 20, GuiScaleLabel(), [this] {
            // -1 (unchanged) → 0 (auto) → 1..4 → back to -1.
            int v = Platform::g_gameSettings.GetInt("debugGuiScale", -1);
            v = v >= 4 ? -1 : v + 1;
            Platform::g_gameSettings.SetInt("debugGuiScale", v);
            Platform::g_gameSettings.Save();
            if (m_guiScale) m_guiScale->SetMessage(GuiScaleLabel());
        }));
        m_guiScale->SetTooltip({ "Overrides the debug overlay with a different", "GUI scale than the rest of the game" });
        x += 114;
        AddWidget(new Button(x, footerY, 60, 20, "Done", [this] { OnClose(); }));
        RefreshProfileButtons();
        SetFocus(m_search);
    }

    void DebugOptionsScreen::RefreshProfileButtons() {
        if (m_defaultProfile) m_defaultProfile->active = !Entries().IsUsingProfile(Profile::Default);
        if (m_performanceProfile) m_performanceProfile->active = !Entries().IsUsingProfile(Profile::Performance);
    }

    void DebugOptionsScreen::RefreshEntries() {
        if (m_list) m_list->RefreshEntries();
        RefreshProfileButtons();
    }

    bool DebugOptionsScreen::KeyPressed(int glfwKey, int glfwMods) {
        return Screen::KeyPressed(glfwKey, glfwMods);
    }

    void DebugOptionsScreen::Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) {
        Screen::Render(g, mouseX, mouseY, partialTick);
        const int listWidth = DebugOptionList::ROW_WIDTH;
        // Title in the middle third of the title row, vertically centred on the search box.
        g.DrawString(m_title, m_width / 2 - listWidth / 6 - g.GetStringWidth(m_title) / 2 + 4, 8 + (20 - FontRenderer::LINE_HEIGHT) / 2 + 1, 0xFFFFFFFF);
        int y = 8 + 20 + 8;
        for (const std::string& line : Wrap(g, kWarning, listWidth)) {
            g.DrawCenteredString(line, m_width / 2, y, kWarningColor);
            y += FontRenderer::LINE_HEIGHT;
        }
        RenderMenuSeparators(g, m_width, HEADER_HEIGHT - 2, m_height - FOOTER_HEIGHT);
    }

} // namespace Render
