// File: src/client/renderer/gui/screens/ShaderOptionsScreen.hpp
//
// The active shader pack's settings, laid out the way the pack asks
// (Shaders::PackLayout — shaders.properties `screen=` with its sub-screens,
// `sliders=`, `profile.*`, and the language file's names and tooltips), as
// OptiFine's and Iris's shader settings screens show them: a grid of
// buttons per screen, a toggle or choice cycling on click, a slider dragged
// (or clicked) over its values, a link opening its sub-screen, the profile
// button cycling the pack's profiles. A pack without a layout lists every
// option flat; options a layout leaves out are reachable under "Other
// Options", so nothing a pack declares is hidden. Done saves the choices
// next to the pack and reloads it; Reset clears them.
#pragma once

#include "Screen.hpp"
#include "Widgets.hpp"
#include "client/shader/ShaderOptions.hpp"

#include <string>
#include <vector>

namespace Render {

    class ShaderOptionsScreen;

    // The current screen's cells, scrolled when they overflow.
    class ShaderOptionGrid : public AbstractWidget {
    public:
        static constexpr int CELL_H  = 20;
        static constexpr int ROW_H   = 24;
        static constexpr int CELL_GAP = 10;

        ShaderOptionGrid(ShaderOptionsScreen& screen, int x, int y, int width, int height);

        void OnClick(double mouseX, double mouseY) override;
        void OnDrag(double mouseX, double mouseY) override;
        void OnRelease(double mouseX, double mouseY) override;
        bool OnScroll(double deltaY) override;
        const std::vector<std::string>* TooltipAt(double mx, double my) override;
        void ResetScroll() { m_scroll = 0.0; }

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        int    CellAt(double mouseX, double mouseY) const;
        void   CellRect(int cell, int& x, int& y, int& w) const;
        int    ContentHeight() const;
        double MaxScroll() const;

        ShaderOptionsScreen& m_screen;
        double m_scroll = 0.0;
        int    m_dragCell = -1;      // the slider being dragged
        std::vector<std::string> m_tooltip;
    };

    class ShaderOptionsScreen : public Screen {
    public:
        ShaderOptionsScreen() : Screen("Shader Pack Settings") {}

        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void OnClose() override;

        // One cell of the current screen.
        struct Cell {
            enum Kind { Empty, Option, Link, Profile };
            Kind        kind = Empty;
            int         option = -1;    // Option: index into Options()
            std::string screen;         // Link: the screen it opens
        };
        const std::vector<Cell>& Cells() const { return m_cells; }
        int Columns() const { return m_columns; }
        const std::vector<Shaders::Option>& Options() const { return m_options; }
        const Shaders::Overrides& Overrides() const { return m_overrides; }
        const Shaders::PackLayout& Layout() const { return m_layout; }

        // What a cell shows and does.
        std::string CellLabel(const Cell& cell) const;
        bool        IsSlider(int option) const;
        double      SliderNorm(int option) const;           // 0..1 over the option's values
        bool        OptionChanged(int option) const;          // differs from the pack's default
        void        CycleOption(int option, int direction);   // toggle, or the next/previous value
        void        SetSliderNorm(int option, double norm);
        void        OpenScreen(const std::string& screen);
        void        Back();
        void        CycleProfile();
        void        ResetAll();

    private:
        static constexpr int HEADER_H = 33;
        static constexpr int FOOTER_H = 33;
        static constexpr int GRID_W   = 310;   // two 150 px cells and the gap (MC's options grid)
        static constexpr const char* kOtherScreen = "<other>";

        void BuildPage();
        void SetValue(int option, const std::string& value);

        std::string m_packName;
        std::vector<Shaders::Option> m_options;
        Shaders::Overrides m_overrides;
        Shaders::PackLayout m_layout;
        std::vector<std::string> m_path;      // the screens opened, "" = the main one
        std::vector<Cell> m_cells;
        int m_columns = 2;
        bool m_dirty = false;
        ShaderOptionGrid* m_grid = nullptr;
        Button* m_backButton = nullptr;
    };

} // namespace Render
