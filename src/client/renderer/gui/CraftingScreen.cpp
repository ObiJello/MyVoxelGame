// File: src/client/renderer/gui/CraftingScreen.cpp
#include "CraftingScreen.hpp"
#include "GuiGraphics.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture

namespace Render {

    CraftingScreen& GetCraftingScreen() {
        static CraftingScreen s;
        return s;
    }

    TextureHandle CraftingScreen::EnsureBackground() {
        if (m_backgroundTried) return m_background;
        m_backgroundTried = true;
        int w = 0, h = 0;
        m_background = LoadStandaloneGuiTexture(
            "assets/textures/gui/container/crafting_table.png", w, h);
        return m_background;
    }

    void CraftingScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        // MC CraftingScreen.renderBg line 27: blit the panel from the 256x256
        // sheet. Nothing else — unlike the inventory screen there is no entity
        // preview on this panel.
        const TextureHandle bg = EnsureBackground();
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
            return;
        }
        g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
               0.0f, 0.0f, (float)IMAGE_W / 256.0f, (float)IMAGE_H / 256.0f);
    }

    void CraftingScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        // MC AbstractContainerScreen.renderLabels (line 227): title, then the
        // player-inventory title, both dark grey and WITHOUT a drop shadow.
        g.DrawString(m_title,     leftPos + TITLE_X,     topPos + TITLE_Y,     LABEL_COLOR, false);
        g.DrawString("Inventory", leftPos + INV_LABEL_X, topPos + INV_LABEL_Y, LABEL_COLOR, false);
    }

} // namespace Render
