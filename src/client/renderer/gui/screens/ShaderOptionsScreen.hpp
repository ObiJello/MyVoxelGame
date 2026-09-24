// File: src/client/renderer/gui/screens/ShaderOptionsScreen.hpp
//
// The active shader pack's settings: one row per option the pack declares
// (Shaders::Discover), click to toggle or step to the next value. Done
// saves the choices next to the pack and reloads it; Reset clears them.
#pragma once

#include "Screen.hpp"
#include "Widgets.hpp"
#include "client/shader/ShaderOptions.hpp"

#include <string>
#include <vector>

namespace Render {

    class ShaderOptionsScreen;

    class ShaderOptionList : public AbstractWidget {
    public:
        static constexpr int ROW_H = 22;

        ShaderOptionList(ShaderOptionsScreen& screen, int x, int y, int width, int height);

        void OnClick(double mouseX, double mouseY) override;
        bool OnScroll(double deltaY) override;
        const std::vector<std::string>* TooltipAt(double mx, double my) override;

    protected:
        void RenderWidget(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;

    private:
        int    RowAt(double mouseX, double mouseY) const;
        int    ContentHeight() const;
        double MaxScroll() const;

        ShaderOptionsScreen& m_screen;
        double m_scroll = 0.0;
        std::vector<std::string> m_tooltip;
    };

    class ShaderOptionsScreen : public Screen {
    public:
        ShaderOptionsScreen() : Screen("Shader Pack Settings") {}
        void Init() override;
        void Render(GuiGraphics& g, int mouseX, int mouseY, float partialTick) override;
        void OnClose() override;

        const std::vector<Shaders::Option>& Options() const { return m_options; }
        const Shaders::Overrides& Overrides() const { return m_overrides; }
        void Cycle(size_t index);       // toggle, or the next value
        void ResetAll();

    private:
        static constexpr int HEADER_H = 33;
        static constexpr int FOOTER_H = 33;
        static constexpr int LIST_W   = 320;

        std::string m_packName;
        std::vector<Shaders::Option> m_options;
        Shaders::Overrides m_overrides;
        bool m_dirty = false;
        ShaderOptionList* m_list = nullptr;
    };

} // namespace Render
