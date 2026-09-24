// File: src/client/renderer/gui/debug/DebugOptionsScreen.hpp
//
// F3+F6 — MC 26.3's DebugOptionsScreen: every debug entry as a row with
// Off / In Overlay / Always buttons, grouped by category, filtered by a
// search box, with the two profile buttons, the debug GUI scale and Done
// in the footer. Drawn over the live F3 overlay (the HUD paints it under
// the screen stack).
#pragma once

#include "../screens/Screen.hpp"
#include "DebugScreenEntries.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Render {

    class DebugOptionsScreen;

    // MC DebugOptionsScreen.OptionList — a ContainerObjectSelectionList of
    // category headers and option rows, hand-rolled on AbstractWidget.
    class DebugOptionList : public AbstractWidget {
    public:
        static constexpr int ITEM_HEIGHT = 20;
        static constexpr int ROW_WIDTH = 350;
        static constexpr int BUTTON_WIDTH = 60;
        static constexpr int BUTTON_HEIGHT = 16;
        static constexpr int SCROLLBAR = 6;

        DebugOptionList(int x, int y, int width, int height);

        void UpdateSearch(const std::string& value);
        void RefreshEntries();

        void OnClick(double mouseX, double mouseY) override;
        void OnDrag(double mouseX, double mouseY) override;
        void OnRelease(double mouseX, double mouseY) override;
        bool OnScroll(double deltaY) override;
        const std::vector<std::string>* TooltipAt(double mx, double my) override;

        // Fired after a status change so the screen can re-enable the
        // profile buttons.
        std::function<void()> onStatusChanged;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        struct Row {
            bool header = false;
            std::string label;            // category label / entry id path
            std::string id;
            bool allowed = true;
            DebugScreen::EntryStatus status = DebugScreen::EntryStatus::Never;
        };
        int    RowAt(double mouseX, double mouseY) const;
        int    RowTop(int index) const;
        int    ContentX() const { return m_x + (m_width - ROW_WIDTH) / 2; }
        int    ContentWidth() const { return ROW_WIDTH; }
        int    ButtonsStartX() const { return ContentX() + ContentWidth() - 3 * BUTTON_WIDTH; }
        double MaxScroll() const;
        int    ContentHeight() const { return static_cast<int>(m_rows.size()) * ITEM_HEIGHT + 4; }
        bool   Scrollable() const { return ContentHeight() > m_height; }
        int    ScrollbarX() const { return ContentX() + ContentWidth() + 10; }
        void   RenderToggle(GuiGraphics& g, int x, int y, const std::string& text, uint32_t onColor,
                            bool selected, bool hovered) const;

        std::vector<Row> m_rows;
        std::string m_filter;
        double m_scroll = 0.0;
        bool m_draggingScrollbar = false;
        double m_scrollbarGrab = 0.0;
        std::vector<std::string> m_notAllowedTooltip;
    };

    class DebugOptionsScreen : public Screen {
    public:
        DebugOptionsScreen();

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        bool KeyPressed(int glfwKey, int glfwMods) override;

        // MC KeyboardHandler refreshes the rows after any F3 chord fired
        // while this screen is up.
        void RefreshEntries();

    private:
        static constexpr int HEADER_HEIGHT = 61;
        static constexpr int FOOTER_HEIGHT = 33;

        void RefreshProfileButtons();
        std::string GuiScaleLabel() const;

        DebugOptionList* m_list = nullptr;
        EditBox* m_search = nullptr;
        Button* m_defaultProfile = nullptr;
        Button* m_performanceProfile = nullptr;
        Button* m_guiScale = nullptr;
        std::string m_filter;
    };

} // namespace Render
