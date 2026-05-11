// File: src/client/renderer/gui/items/HeadItemRenderer.hpp
// MC's SkullBlockRenderer equivalent: draws a 3D 8x8x8 head cube using the per-mob
// entity texture (assets/textures/entity/{mob}/{mob}.png) as the GUI icon.
// Mirrors net.minecraft.client.renderer.blockentity.SkullBlockRenderer geometry.
#pragma once

#include "../GuiGraphics.hpp"

namespace Render {

    // Walk the item registry and hook this renderer onto every item whose
    // specialKind == "head" or "player_head" (skeleton_skull, creeper_head, etc.).
    // MUST be called AFTER ItemRegistry::Initialize() so the registry is populated.
    void RegisterHeadItemRenderer();

} // namespace Render
