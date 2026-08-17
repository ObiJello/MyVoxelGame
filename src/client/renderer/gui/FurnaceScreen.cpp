// File: src/client/renderer/gui/FurnaceScreen.cpp
#include "FurnaceScreen.hpp"
#include "GuiGraphics.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture
#include "common/inventory/FurnaceMenu.hpp"
#include <algorithm>
#include <cmath>

namespace Render {

    FurnaceScreen& GetFurnaceScreen() {
        static FurnaceScreen s;
        return s;
    }

    void FurnaceScreen::Configure(Game::MenuType type, const std::string& title) {
        // Panel sheet AND sprite pair both switch with the variant — MC gives
        // each cooker its own three assets (BlastFurnaceScreen.java:14-16).
        switch (type) {
            case Game::MenuType::BlastFurnace:
                m_texture    = "assets/textures/gui/container/blast_furnace.png";
                m_litSprite  = "container/blast_furnace/lit_progress";
                m_burnSprite = "container/blast_furnace/burn_progress";
                break;
            case Game::MenuType::Smoker:
                m_texture    = "assets/textures/gui/container/smoker.png";
                m_litSprite  = "container/smoker/lit_progress";
                m_burnSprite = "container/smoker/burn_progress";
                break;
            default:
                m_texture    = "assets/textures/gui/container/furnace.png";
                m_litSprite  = "container/furnace/lit_progress";
                m_burnSprite = "container/furnace/burn_progress";
                break;
        }
        m_title = title;
        m_background = INVALID_TEXTURE;
        m_backgroundTried = false;
    }

    TextureHandle FurnaceScreen::EnsureBackground() {
        if (m_backgroundTried) return m_background;
        m_backgroundTried = true;
        int w = 0, h = 0;
        m_background = LoadStandaloneGuiTexture(m_texture, w, h);
        return m_background;
    }

    void FurnaceScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        const TextureHandle bg = EnsureBackground();
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
            return;
        }
        constexpr float SHEET = 256.0f;
        g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
               0.0f, 0.0f, (float)IMAGE_W / SHEET, (float)IMAGE_H / SHEET);

        auto* furnace = dynamic_cast<Game::FurnaceMenu*>(PlayerContainerMenu());
        if (!furnace) return;

        // Flame — MC draws the LIT portion bottom-up: a k-pixel-tall flame is
        // the BOTTOM k rows of the sprite, blitted k pixels lower down. Taking
        // the top k instead is the classic way to get a flame that burns
        // upside-down.
        //
        // The 13.0f + 1 is MC's, not a typo for 14: it keeps one row of flame
        // lit for the whole final tick of fuel, so the flame never blinks out
        // a tick before the furnace actually goes dark.
        if (furnace->IsLit()) {
            const int litH = std::min(
                FLAME_H, static_cast<int>(std::ceil(furnace->LitProgress() * 13.0f)) + 1);
            const int dy = FLAME_H - litH;
            g.BlitSprite(m_litSprite, FLAME_W, FLAME_H,
                         0, dy,
                         leftPos + FLAME_X, topPos + FLAME_Y + dy,
                         FLAME_W, litH);
        }

        // Arrow — grows left to right. MC blits this unconditionally; we skip
        // the zero-width case rather than submit a degenerate quad.
        const int burnW = std::min(
            ARROW_W, static_cast<int>(std::ceil(furnace->BurnProgress() * 24.0f)));
        if (burnW > 0) {
            g.BlitSprite(m_burnSprite, ARROW_W, ARROW_H,
                         0, 0,
                         leftPos + ARROW_X, topPos + ARROW_Y,
                         burnW, ARROW_H);
        }
    }

    void FurnaceScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        g.DrawString(m_title, leftPos + TITLE_X, topPos + TITLE_Y, LABEL_COLOR, false);
        g.DrawString("Inventory", leftPos + INV_LABEL_X, topPos + INV_LABEL_Y,
                     LABEL_COLOR, false);
    }

} // namespace Render
