// File: src/client/renderer/gui/items/ChestItemRenderer.hpp
// MC's BlockEntityWithoutLevelRenderer.renderChest() equivalent: draws a 3D chest
// model using the entity texture (assets/textures/entity/chest/normal.png) as the GUI
// icon. Mirrors the geometry from net.minecraft.client.model.object.chest.ChestModel
// — bottom + lid + lock cubes with MC's exact CubeListBuilder UV layout.
#pragma once

#include "../GuiGraphics.hpp"

namespace Render {

    // Wire up `ItemID` for chest. Called from PlatformMain after backend init / asset load.
    void RegisterChestItemRenderer();

} // namespace Render
