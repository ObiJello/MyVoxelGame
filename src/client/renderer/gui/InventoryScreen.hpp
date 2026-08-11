// File: src/client/renderer/gui/InventoryScreen.hpp
//
// The SURVIVAL inventory — mirrors
// net.minecraft.client.gui.screens.inventory.InventoryScreen.
//
// 176x166 panel (textures/gui/container/inventory.png) showing the player
// model, the armour column, the offhand slot, the 2x2 crafting grid and its
// result square, plus the 3x9 storage rows and the hotbar. Every slot position
// comes from Game::InventoryMenu, which carries MC's coordinates verbatim.
//
// Not ported (deliberately, with nothing behind them yet):
//   • the recipe-book button and panel — there is no recipe system, and a
//     button that opens nothing is worse than no button.
//   • EffectsInInventory — the status-effect strip beside the panel; we have no
//     mob effects.
// The crafting grid itself IS live: items can be parked in it and the server
// hands them back when the screen closes (MC InventoryMenu.removed), but no
// recipe ever resolves, so the result square stays empty.
//
// The creative counterpart lives in CreativeModeInventoryScreen.hpp. Which of
// the two opens is decided by game mode — see GetInventoryScreen /
// OpenInventoryScreen at the bottom of this header.
#pragma once

#include "AbstractContainerScreen.hpp"
#include "../backend/RenderTypes.hpp"

namespace Render {

    class InventoryScreen : public AbstractContainerScreen {
    public:
        // MC AbstractContainerScreen's default panel size (176x166), used by
        // InventoryScreen unchanged.
        static constexpr int IMAGE_W = 176;
        static constexpr int IMAGE_H = 166;

        // MC InventoryScreen.renderBg — renderEntityInInventoryFollowsMouse
        // over (leftPos+26, topPos+8)..(leftPos+75, topPos+78) at scale 30.
        static constexpr int PREVIEW_X0 = 26;
        static constexpr int PREVIEW_Y0 = 8;
        static constexpr int PREVIEW_X1 = 75;
        static constexpr int PREVIEW_Y1 = 78;
        static constexpr int PREVIEW_SIZE = 30;

        // MC InventoryScreen ctor: titleLabelX = 97, titleLabelY = 6 (the base
        // class's default). renderLabels is overridden there to draw ONLY the
        // title, which is why there is no "Inventory" label on this panel.
        static constexpr int TITLE_X = 97;
        static constexpr int TITLE_Y = 6;
        // MC's -12566464, the dark grey every container label uses.
        static constexpr uint32_t LABEL_COLOR = 0xFF404040;

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;
        void ContainerTick() override;

    private:
        TextureHandle EnsureBackground();
        TextureHandle m_background = INVALID_TEXTURE;
        bool          m_backgroundTried = false;
    };

    // ─── Screen selection ────────────────────────────────────────────────
    // The container screen that currently owns input and rendering — whichever
    // of the two is open, and the survival one when neither is (its handlers
    // all early-out on !IsOpen, so it is a safe stand-in).
    AbstractContainerScreen& GetInventoryScreen();

    // Opens the screen this player's game mode calls for. Mirrors MC's
    // InventoryScreen.init, which immediately replaces itself with
    // CreativeModeInventoryScreen when the player has infinite materials.
    void OpenInventoryScreen();

    InventoryScreen& GetSurvivalInventoryScreen();

} // namespace Render
