// File: src/client/renderer/gui/items/ShieldItemRenderer.hpp
// MC's ShieldSpecialRenderer (BlockEntityWithoutLevelRenderer path for
// Items.SHIELD) equivalent: draws a 3D shield model in the inventory using
// the entity texture (assets/textures/entity/shield_base_nopattern.png).
// Geometry mirrors net.minecraft.client.model.object.equipment.ShieldModel
// (a 12×22×1 plate + a 2×6×6 handle). Pose mirrors the GUI display from
// assets/models/item/shield.json (rotation [15, -25, -5], translation
// [2, 3, 0], scale 0.65) plus MC's BEWLR scale(1, -1, -1) flip.
#pragma once

#include "../GuiGraphics.hpp"

namespace Render {

    // Wire up the shield BEWLR for any item whose items/{slug}.json declared
    // specialKind == "shield". Call AFTER ItemRegistry::Initialize().
    void RegisterShieldItemRenderer();

} // namespace Render
