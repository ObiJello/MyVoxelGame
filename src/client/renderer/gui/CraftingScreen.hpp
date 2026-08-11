// File: src/client/renderer/gui/CraftingScreen.hpp
//
// Mirrors net.minecraft.client.gui.screens.inventory.CraftingScreen — the
// crafting table's UI.
//
// A 176x166 panel (textures/gui/container/crafting_table.png) with the 3x3
// grid, the output square and the player's storage rows. Every slot position
// comes from Game::CraftingMenu, which carries MC's coordinates verbatim, so
// this class only has to paint the panel and the two labels.
//
// Not ported (same reasons as InventoryScreen): the recipe book button and
// panel. Everything else about the screen — placing ingredients, the live
// result preview, shift-click to craft the whole stack — works, because the
// recipe lookup runs in the shared menu on both sides.
#pragma once

#include "AbstractContainerScreen.hpp"
#include "../backend/RenderTypes.hpp"
#include <string>

namespace Render {

    class CraftingScreen : public AbstractContainerScreen {
    public:
        // MC AbstractContainerScreen's default panel size, used unchanged.
        static constexpr int IMAGE_W = 176;
        static constexpr int IMAGE_H = 166;

        // MC CraftingScreen.init sets titleLabelX = 29; titleLabelY keeps the
        // base's 6. Unlike InventoryScreen, CraftingScreen does NOT override
        // renderLabels, so the base draws the player-inventory label too —
        // inventoryLabelX = 8, inventoryLabelY = imageHeight - 94 = 72.
        static constexpr int TITLE_X     = 29;
        static constexpr int TITLE_Y     = 6;
        static constexpr int INV_LABEL_X = 8;
        static constexpr int INV_LABEL_Y = IMAGE_H - 94;
        static constexpr uint32_t LABEL_COLOR = 0xFF404040;   // MC's -12566464

        // The menu title from OpenScreenS2C ("Crafting").
        void SetTitle(const std::string& title) { m_title = title; }

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;

    private:
        TextureHandle EnsureBackground();
        TextureHandle m_background = INVALID_TEXTURE;
        bool          m_backgroundTried = false;
        std::string   m_title = "Crafting";
    };

    CraftingScreen& GetCraftingScreen();

} // namespace Render
