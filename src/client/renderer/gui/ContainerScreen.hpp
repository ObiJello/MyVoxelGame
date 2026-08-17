// File: src/client/renderer/gui/ContainerScreen.hpp
//
// The generic block-container UI — MC's ContainerScreen (the 9xN chest family)
// plus DispenserScreen and HopperScreen, which differ only in their panel
// texture and height.
//
// MC needs a class per screen because each hangs off a differently-typed menu.
// Ours are all ChestMenu, so a screen is nothing but DATA: which texture, how
// tall, where the labels sit. One class driven by a descriptor replaces the
// three, and every later container that is "a grid plus the player's rows"
// (barrel, shulker box, ender chest) is a table row rather than a new file.
//
// Slot positions are NOT here — they come from the menu, which carries MC's
// coordinates verbatim. This class only paints the panel and the two labels,
// exactly like CraftingScreen.
#pragma once

#include "AbstractContainerScreen.hpp"
#include "../backend/RenderTypes.hpp"
#include "common/inventory/MenuType.hpp"
#include <string>

namespace Render {

    class ContainerScreen : public AbstractContainerScreen {
    public:
        // Everything that varies between container screens.
        struct Layout {
            const char* texture;      // assets/textures/gui/container/*.png
            int imageWidth;
            int imageHeight;
            int titleX;
            int titleY;
            int invLabelX;
            int invLabelY;
        };

        // The panel for a menu type. Rows drive the chest family's height the
        // same way MC's ContainerScreen does (imageHeight = 114 + rows*18).
        static Layout LayoutFor(Game::MenuType type, int rows);

        void Configure(const Layout& layout, const std::string& title) {
            m_layout = layout;
            m_title  = title;
            // A different panel means a different texture; drop the cached one.
            m_background      = INVALID_TEXTURE;
            m_backgroundTried = false;
        }

        static constexpr uint32_t LABEL_COLOR = 0xFF404040;   // MC's -12566464

    protected:
        int ImageWidth()  const override { return m_layout.imageWidth; }
        int ImageHeight() const override { return m_layout.imageHeight; }

        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;

    private:
        TextureHandle EnsureBackground();

        Layout        m_layout{"assets/textures/gui/container/generic_54.png",
                               176, 168, 8, 6, 8, 74};
        TextureHandle m_background = INVALID_TEXTURE;
        bool          m_backgroundTried = false;
        std::string   m_title;
    };

    ContainerScreen& GetContainerScreen();

} // namespace Render
