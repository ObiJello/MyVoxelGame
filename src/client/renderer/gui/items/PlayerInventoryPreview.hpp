// File: src/client/renderer/gui/items/PlayerInventoryPreview.hpp
//
// Renders our stick-figure player into the inventory's preview box, mirroring
// MC's `InventoryScreen.renderEntityInInventoryFollowsMouse()` behavior:
// dampened-atan cursor tracking, body and head yaw bias, head-pitch tilt.
//
// Implementation: CPU-projects the stick figure's 3D vertices to 2D screen
// coords and submits them as `QuadCommand`s through `GuiRenderState`. Each line
// segment becomes a 1.5 px-wide rotated quad; each filled triangle becomes a
// degenerate quad. This integrates with the existing GUI Z-stratum + scissor
// system without needing GPU scissor or PIP infrastructure.
#pragma once

#include "../GuiGraphics.hpp"

namespace Render {

    struct StickFigurePose {
        float bodyYawDeg;
        float headYawDeg;
        float headPitchDeg;
        bool  isCrouching;
    };

    // MC: InventoryScreen.renderEntityInInventoryFollowsMouse (lines 83-108).
    // `size` is pixels-per-meter for the projection; MC uses 20 for the creative
    // survival tab (CreativeModeInventoryScreen.java:702).
    void RenderStickFigureInInventory(GuiGraphics& g,
                                      int x0, int y0, int x1, int y1,
                                      int size, float offsetY,
                                      float mouseX, float mouseY,
                                      const StickFigurePose& pose);

} // namespace Render
