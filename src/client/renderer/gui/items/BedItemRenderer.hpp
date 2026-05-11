// File: src/client/renderer/gui/items/BedItemRenderer.hpp
// MC's BedSpecialRenderer equivalent: draws a 3D bed model using the per-color
// entity texture (assets/textures/entity/bed/{color}.png) as the GUI icon.
// Mirrors net.minecraft.client.renderer.blockentity.BedRenderer geometry.
#pragma once

#include "../GuiGraphics.hpp"

namespace Render {

    // Walk the item registry and hook this renderer onto every item whose
    // specialKind == "bed" (red_bed, blue_bed, …). MUST be called AFTER
    // ItemRegistry::Initialize() so the registry is populated.
    void RegisterBedItemRenderer();

} // namespace Render
