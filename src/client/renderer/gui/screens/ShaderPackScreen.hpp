// File: src/client/renderer/gui/screens/ShaderPackScreen.hpp
//
// Options → Shader Packs: the packs in <game dir>/shaderpacks/ in a list
// with "Off" at the top. Clicking one applies it at once (the pipeline
// reloads and its status line under the title says what happened), so the
// world behind the menu shows the result while the screen is still up.
#pragma once

#include "Screen.hpp"
#include "Widgets.hpp"
#include "client/shader/ShaderPacks.hpp"

#include <string>
#include <vector>

namespace Render {

    class ShaderPackScreen;

    class ShaderPackList : public AbstractWidget {
    public:
        static constexpr int ROW_H = 24;

        ShaderPackList(ShaderPackScreen& screen, int x, int y, int width, int height);

        void SetPacks(std::vector<Shaders::PackInfo> packs, const std::string& selectedId);
        void SetSelected(const std::string& id) { m_selectedId = id; }

        void OnClick(double mouseX, double mouseY) override;
        bool OnScroll(double deltaY) override;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        struct Row { std::string id; std::string label; std::string detail; };

        int    RowAt(double mouseX, double mouseY) const;   // -1 = none
        int    ContentHeight() const { return static_cast<int>(m_rows.size()) * ROW_H + 4; }
        double MaxScroll() const;

        ShaderPackScreen& m_screen;
        std::vector<Row>  m_rows;
        std::string       m_selectedId;
        double            m_scroll = 0.0;
    };

    class ShaderPackScreen : public Screen {
    public:
        ShaderPackScreen() : Screen("Shader Packs") {}
        void Init() override;
        void Tick() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void OnClose() override;

        // From the list: select a pack ("" = off) and apply it.
        void Select(const std::string& id);

    private:
        static constexpr int HEADER_H = 44;
        static constexpr int FOOTER_H = 33;
        static constexpr int LIST_W   = 300;

        void Refresh();

        ShaderPackList* m_list = nullptr;
        Button*         m_settingsButton = nullptr;
        std::vector<std::string> m_knownIds;
        int m_rescanTicks = 0;
    };

} // namespace Render
