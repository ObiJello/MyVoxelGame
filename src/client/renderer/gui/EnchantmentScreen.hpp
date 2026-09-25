// File: src/client/renderer/gui/EnchantmentScreen.hpp
//
// Mirrors net.minecraft.client.gui.screens.inventory.EnchantmentScreen — the
// enchanting table's panel: the item and lapis slots, and the three offer
// rows. Each row shows its level icon, a line of Standard Galactic words
// (EnchantmentNames, seeded from the menu's enchantment seed so the words hold
// still), and the level cost; hovering it names the clue enchantment and what
// the offer charges, clicking it asks the server to enchant
// (ContainerButtonClickC2S, the button id being the row).
//
// Everything the rows show is the server's: the costs, the seed and the clues
// are the menu's data slots (EnchantmentMenu), which the client copy never
// rolls itself.
//
// Not drawn: the animated book above the slots (MC renders the enchanting
// table's BookModel in the GUI; the engine has no GUI entity-model path yet).
#pragma once

#include "AbstractContainerScreen.hpp"
#include "FontRenderer.hpp"
#include "common/core/JavaRandom.hpp"
#include "../backend/RenderTypes.hpp"
#include <string>
#include <utility>
#include <vector>

namespace Game { class EnchantmentMenu; }

namespace Render {

    class EnchantmentScreen : public AbstractContainerScreen {
    public:
        static constexpr int IMAGE_W = 176;
        static constexpr int IMAGE_H = 166;
        static constexpr int TITLE_X = 8;
        static constexpr int TITLE_Y = 6;
        static constexpr int INV_LABEL_X = 8;
        static constexpr int INV_LABEL_Y = IMAGE_H - 94;
        static constexpr uint32_t LABEL_COLOR = 0xFF404040;

        // MC EnchantmentScreen geometry: rows at (60, 14 + 19i), 108 x 19;
        // the hover test for the tooltip is 17 tall (isHovering(60, 14 + 19i,
        // 108, 17)).
        static constexpr int ROW_X = 60, ROW_Y = 14, ROW_STEP = 19, ROW_W = 108, ROW_H = 19;
        static constexpr int ROW_TOOLTIP_H = 17;

        // Hit-test ids for the three rows: HIT_ROW_0 - i.
        static constexpr int HIT_ROW_0 = -20;

        void Configure(const std::string& title);

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderExtras(GuiGraphics& g, int leftPos, int topPos) override;

        int  HitTestExtras(int lx, int ly) override;
        bool HandleExtraClick(int hit, int glfwButton, bool shift) override;

        void ContainerTick() override;

    private:
        using Segment = std::pair<std::string, uint32_t>;   // text, ARGB
        using Line    = std::vector<Segment>;

        Game::EnchantmentMenu* Enchanting() const;
        TextureHandle EnsureBackground();
        bool EnsureGalacticFont();

        // MC EnchantmentNames.getRandomName: three or four words off the
        // seeded random, cut to `maxWidth` in the galactic font.
        std::string RandomName(int maxWidth);
        // Advance of one galactic glyph / a whole string (MC bitmap
        // provider: glyph width + 1; the space provider's 4).
        int GalacticAdvance(unsigned char c) const;
        void DrawGalactic(GuiGraphics& g, const std::string& text, int x, int y, uint32_t color);

        // A tooltip whose lines may change colour mid-line (the clue's
        // enchantment name keeps its own grey / red inside the white clue).
        void RenderSegmentTooltip(GuiGraphics& g, const std::vector<Line>& lines, int mx, int my);

        bool RowHovered(int row, int leftPos, int topPos, int height) const;
        bool HasInfiniteMaterials() const;
        int  PlayerLevel() const;

        TextureHandle m_background = INVALID_TEXTURE;
        bool          m_backgroundTried = false;
        FontRenderer  m_galactic;
        bool          m_galacticTried = false;
        bool          m_galacticReady = false;
        std::string   m_title = "Enchant";
        // EnchantmentNames' own RandomSource, reseeded with the menu's seed
        // every frame (initSeed) so the words only change with the offers.
        Game::JavaRandom m_nameRandom{0};
    };

    EnchantmentScreen& GetEnchantmentScreen();

} // namespace Render
