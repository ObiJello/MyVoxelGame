// File: src/client/renderer/gui/items/DecoratedPotItemRenderer.hpp
//
// The decorated pot's inventory icon — MC renders the item through
// DecoratedPotSpecialRenderer (the block entity renderer's pot, sides from
// the stack's POT_DECORATIONS) under template display.gui rotation
// [30, 45, 0], scale 0.6, gui_light front. Same painter's-algorithm GUI
// path as ChestItemRenderer.
#pragma once

#include "../GuiGraphics.hpp"

namespace Render {

    // Hooks the decorated pot item. Called from PlatformMain after the item
    // registry is up, beside the chest's.
    void RegisterDecoratedPotItemRenderer();

} // namespace Render
