// File: src/client/renderer/gui/ContainerScreen.cpp
#include "ContainerScreen.hpp"
#include "GuiGraphics.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture
#include "common/inventory/SystemMenus.hpp"   // BrewingStandMenu

#include <algorithm>

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
            if (auto* stand = dynamic_cast<Game::BrewingStandMenu*>(PlayerContainerMenu())) {
                RenderBrewingStandProgress(g, *stand, leftPos, topPos);
            }
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

    void ContainerScreen::RenderBrewingStandProgress(GuiGraphics& g,
                                                     const Game::BrewingStandMenu& menu,
                                                     int leftPos, int topPos) {
        // MC BrewingStandScreen.extractBackground, after the panel blit.
        // Fuel bar: Mth.positiveCeilDiv(18 * fuel, totalFuel), clamped 0..18.
        const int fuel = menu.GetFuel();
        const int totalFuel = menu.GetTotalFuel();
        int fuelLength = 0;
        if (totalFuel > 0 && fuel > 0) {
            fuelLength = std::clamp((18 * fuel + totalFuel - 1) / totalFuel, 0, 18);
        }
        if (fuelLength > 0) {
            g.BlitSprite("container/brewing_stand/fuel_length", 18, 4, 0, 0,
                         leftPos + 60, topPos + 44, fuelLength, 4);
        }

        const int tickCount = menu.GetBrewingTicks();
        const int totalTickCount = menu.GetTotalBrewingTicks();
        if (tickCount > 0 && totalTickCount > 0) {
            int length = static_cast<int>(
                28.0f * (1.0f - static_cast<float>(tickCount) / static_cast<float>(totalTickCount)));
            if (length > 0) {
                g.BlitSprite("container/brewing_stand/brew_progress", 9, 28, 0, 0,
                             leftPos + 97, topPos + 16, 9, length);
            }
            static constexpr int kBubbleLengths[7] = {29, 24, 20, 16, 11, 6, 0};
            length = kBubbleLengths[tickCount / 2 % 7];
            if (length > 0) {
                g.BlitSprite("container/brewing_stand/bubbles", 12, 29, 0, 29 - length,
                             leftPos + 63, topPos + 14 + 29 - length, 12, length);
            }
        }
    }

    void ContainerScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        // MC AbstractContainerScreen.renderLabels: title then the player's
        // inventory title, dark grey, no drop shadow. BrewingStandScreen.init
        // centres its title (titleLabelX = (imageWidth - width) / 2).
        int titleX = m_layout.titleX;
        if (dynamic_cast<Game::BrewingStandMenu*>(PlayerContainerMenu())) {
            titleX = (m_layout.imageWidth - g.GetStringWidth(m_title)) / 2;
        }
        g.DrawString(m_title, leftPos + titleX, topPos + m_layout.titleY,
                     LABEL_COLOR, false);
        g.DrawString("Inventory", leftPos + m_layout.invLabelX,
                     topPos + m_layout.invLabelY, LABEL_COLOR, false);
    }

} // namespace Render
