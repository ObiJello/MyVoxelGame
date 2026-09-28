// File: src/client/renderer/gui/LoomScreen.hpp
//
// Mirrors net.minecraft.client.gui.screens.inventory.LoomScreen — the loom's
// panel: the banner, dye and pattern slots, the 4 x 4 scrolling grid of
// patterns the inputs allow, the result banner's preview, and the error mark
// on a banner that already carries six layers.
//
// The grid is the client menu's view of LoomMenu's selectable patterns (the
// #no_item_required list, or what the pattern item provides); clicking a cell
// selects it (ContainerButtonClickC2S, the button id being the pattern's
// index), and the server's result slot follows.
#pragma once

#include "AbstractContainerScreen.hpp"
#include "../backend/RenderTypes.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace Game { class LoomMenu; }

namespace Render {

    class LoomScreen : public AbstractContainerScreen {
    public:
        static constexpr int IMAGE_W = 176;
        static constexpr int IMAGE_H = 166;
        static constexpr int TITLE_X = 8;
        static constexpr int TITLE_Y = 6 - 2;   // LoomScreen: titleLabelY -= 2
        static constexpr int INV_LABEL_X = 8;
        static constexpr int INV_LABEL_Y = IMAGE_H - 94;
        static constexpr uint32_t LABEL_COLOR = 0xFF404040;

        // MC LoomScreen geometry.
        static constexpr int PATTERN_COLUMNS = 4;
        static constexpr int PATTERN_ROWS = 4;
        static constexpr int PATTERN_IMAGE_SIZE = 14;
        static constexpr int PATTERNS_X = 60;
        static constexpr int PATTERNS_Y = 13;
        static constexpr int SCROLLER_X = 119;
        static constexpr int SCROLLER_Y = 13;
        static constexpr int SCROLLER_W = 12;
        static constexpr int SCROLLER_H = 15;
        static constexpr int SCROLLER_FULL_HEIGHT = 56;
        // mouseClicked's scroller grab zone starts 4 px higher than the bar.
        static constexpr int SCROLLER_CLICK_Y = 9;

        // Hit-test ids: HIT_PATTERN_0 - visible cell (0..15), and the scroller.
        static constexpr int HIT_SCROLLER  = -20;
        static constexpr int HIT_PATTERN_0 = -21;

        void Configure(const std::string& title);

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderExtras(GuiGraphics& g, int leftPos, int topPos) override;

        const char* GetNoItemIcon(int menuIndex) const override;

        int  HitTestExtras(int lx, int ly) override;
        bool HandleExtraClick(int hit, int glfwButton, bool shift) override;
        void HandleExtraRelease() override;
        bool HandleExtraScroll(double dy) override;
        void OnExtraMouseMove(int leftPos, int topPos) override;

        void OnOpen() override;
        void ContainerTick() override;

    private:
        Game::LoomMenu* Loom() const;
        TextureHandle EnsureBackground();
        // entity/banner/<asset>.png, loaded once.
        TextureHandle PatternTexture(const std::string& asset);

        // containerChanged: the list on offer and whether the grid shows.
        void Refresh();
        int  TotalRowCount() const;
        bool IsScrollBarActive() const;

        // extractBannerOnButton: the pattern's flag face, 5 x 10, on grey.
        void DrawBannerOnButton(GuiGraphics& g, int posX, int posY, const std::string& pattern);
        // GuiBannerResultRenderer, seen face-on: the base colour's flag, then
        // every layer, filling (x0, y0)-(x1, y1).
        void DrawResultBanner(GuiGraphics& g, int x0, int y0, int x1, int y1);
        void DrawTextTooltip(GuiGraphics& g, const std::string& text, int mx, int my);

        TextureHandle m_background = INVALID_TEXTURE;
        bool          m_backgroundTried = false;
        std::unordered_map<std::string, TextureHandle> m_patternTextures;
        std::string   m_title = "Loom";

        std::vector<std::string> m_patterns;   // what the grid offers
        bool  m_displayPatterns = false;
        bool  m_hasMaxPatterns = false;
        float m_scrollOffs = 0.0f;
        bool  m_scrolling = false;
        int   m_startRow = 0;
    };

    LoomScreen& GetLoomScreen();

} // namespace Render
