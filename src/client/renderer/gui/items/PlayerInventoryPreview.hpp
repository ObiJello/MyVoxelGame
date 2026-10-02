// File: src/client/renderer/gui/items/PlayerInventoryPreview.hpp
//
// Renders the local player into the inventory's preview box — the stick
// figure (its colour, or the launcher's painted figure), or MC's player model
// in their skin (PlayerSkins: the same capture MountInventoryScreen draws a
// mount with) — mirroring
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
#include "common/entity/PlayerColors.hpp"

namespace Game { class ClientPlayer; }

namespace Render {

    struct StickFigurePose {
        float bodyYawDeg;
        float headYawDeg;
        float headPitchDeg;
        bool  isCrouching;
        // MC isPassenger: the seated pose (a player on a cushion).
        bool  isSitting = false;
        // The local player, when the caller has one: a Minecraft-skin look
        // (Client::PlayerSkins) draws MC's player model with what they hold
        // and wear, their sneak and walk — InventoryScreen draws the entity
        // as it is.
        const Game::ClientPlayer* player = nullptr;
    };

    // MC: InventoryScreen.renderEntityInInventoryFollowsMouse (lines 83-108).
    // `size` is pixels-per-meter for the projection; MC uses 20 for the creative
    // survival tab (CreativeModeInventoryScreen.java:702).
    // `colorId` selects the stick-figure colour from the shared palette; defaults
    // to the historical neon green so callers that don't care can omit it.
    void RenderStickFigureInInventory(GuiGraphics& g,
                                      int x0, int y0, int x1, int y1,
                                      int size, float offsetY,
                                      float mouseX, float mouseY,
                                      const StickFigurePose& pose,
                                      Game::PlayerColorId colorId = Game::PlayerColorId::Default);

} // namespace Render
