// File: src/client/renderer/gui/ContainerScreen.cpp
#include "ContainerScreen.hpp"
#include "GuiGraphics.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture

namespace Render {

    ContainerScreen& GetContainerScreen() {
        static ContainerScreen s;
        return s;
    }

    ContainerScreen::Layout ContainerScreen::LayoutFor(Game::MenuType type, int rows) {
        switch (type) {
            case Game::MenuType::Generic3x3:
                // MC DispenserScreen: the standard 176x166 panel.
                return {"assets/textures/gui/container/dispenser.png",
                        176, 166, 8, 6, 8, 166 - 94};

            case Game::MenuType::Hopper:
                // MC HopperScreen: imageHeight 133, inventory label at 39.
                return {"assets/textures/gui/container/hopper.png",
                        176, 133, 8, 6, 8, 133 - 94};

            // The utility blocks (MC gives each its own Screen class, but they
            // are all the default panel with a different texture — the moving
            // parts each one adds are its own feature, not layout).
            case Game::MenuType::Stonecutter:
                return {"assets/textures/gui/container/stonecutter.png",
                        176, 166, 8, 6, 8, 166 - 94};
            case Game::MenuType::Grindstone:
                return {"assets/textures/gui/container/grindstone.png",
                        176, 166, 8, 6, 8, 166 - 94};
            case Game::MenuType::CartographyTable:
                return {"assets/textures/gui/container/cartography_table.png",
                        176, 166, 8, 6, 8, 166 - 94};
            case Game::MenuType::Loom:
                return {"assets/textures/gui/container/loom.png",
                        176, 166, 8, 6, 8, 166 - 94};
            case Game::MenuType::Smithing:
                return {"assets/textures/gui/container/smithing.png",
                        176, 166, 8, 6, 8, 166 - 94};
            case Game::MenuType::Anvil:
                return {"assets/textures/gui/container/anvil.png",
                        176, 166, 8, 6, 8, 166 - 94};

            case Game::MenuType::Enchantment:
                return {"assets/textures/gui/container/enchanting_table.png",
                        176, 166, 8, 6, 8, 166 - 94};
            case Game::MenuType::BrewingStand:
                return {"assets/textures/gui/container/brewing_stand.png",
                        176, 166, 8, 6, 8, 166 - 94};
            case Game::MenuType::Beacon:
                // MC BeaconScreen is a wide 230x219 panel.
                return {"assets/textures/gui/container/beacon.png",
                        230, 219, 8, 6, 36, 137};
            case Game::MenuType::Crafter3x3:
                return {"assets/textures/gui/container/crafter.png",
                        176, 166, 8, 6, 8, 166 - 94};

            default: {
                // MC ContainerScreen: imageHeight = 114 + rows*18, and the
                // player-inventory label sits imageHeight-94 from the top
                // (AbstractContainerScreen's default). generic_54.png is one
                // sheet holding six rows of slots; a 3-row chest samples the
                // top part of it, which is why the blit below is height-driven
                // rather than a fixed sub-rect.
                const int imageHeight = 114 + rows * 18;
                return {"assets/textures/gui/container/generic_54.png",
                        176, imageHeight, 8, 6, 8, imageHeight - 94};
            }
        }
    }

    TextureHandle ContainerScreen::EnsureBackground() {
        if (m_backgroundTried) return m_background;
        m_backgroundTried = true;
        int w = 0, h = 0;
        m_background = LoadStandaloneGuiTexture(m_layout.texture, w, h);
        return m_background;
    }

    void ContainerScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        const TextureHandle bg = EnsureBackground();
        const int iw = m_layout.imageWidth;
        const int ih = m_layout.imageHeight;
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + iw, topPos + ih, 0xC0202020);
            return;
        }

        const bool generic = m_layout.imageHeight != 166 && m_layout.imageHeight != 133;
        if (!generic) {
            g.Blit(bg, leftPos, topPos, leftPos + iw, topPos + ih,
                   0.0f, 0.0f, (float)iw / 256.0f, (float)ih / 256.0f);
            return;
        }

        // MC ContainerScreen.renderBg draws the chest sheet in TWO blits: the
        // container half (17 + rows*18 tall) from the top of the sheet, then
        // the player half from a fixed offset 126px down. Sampling one
        // contiguous rect instead would stretch a 6-row sheet into a 3-row
        // panel and misalign every slot below the grid.
        const int containerH = ih - 96;                 // 18 + rows*18
        g.Blit(bg, leftPos, topPos, leftPos + iw, topPos + containerH,
               0.0f, 0.0f, (float)iw / 256.0f, (float)containerH / 256.0f);
        g.Blit(bg, leftPos, topPos + containerH, leftPos + iw, topPos + ih,
               0.0f, 126.0f / 256.0f,
               (float)iw / 256.0f, (126.0f + (float)(ih - containerH)) / 256.0f);
    }

    void ContainerScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        // MC AbstractContainerScreen.renderLabels: title then the player's
        // inventory title, dark grey, no drop shadow.
        g.DrawString(m_title, leftPos + m_layout.titleX, topPos + m_layout.titleY,
                     LABEL_COLOR, false);
        g.DrawString("Inventory", leftPos + m_layout.invLabelX,
                     topPos + m_layout.invLabelY, LABEL_COLOR, false);
    }

} // namespace Render
