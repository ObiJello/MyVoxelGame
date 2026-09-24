// File: src/client/renderer/gui/EffectsInInventory.hpp
//
// MC net.minecraft.client.gui.screens.inventory.EffectsInInventory — the
// status-effect column to the right of the survival and creative inventory
// panels: one container/inventory/effect_background(_ambient) nine-slice
// per effect (32 high, as wide as the name or the duration needs up to the
// room available), the 18x18 mob_effect icon, the name ("Speed II") in
// white and the duration ("01:23" / "∞") in grey. When the column is too
// narrow for text it shrinks to the 32-px icon tiles and the name and
// duration move to a tooltip.
//
// Also MC Screen.showsActiveEffects for those two screens: while the column
// is visible, the HUD's top-right effect icons (HudRenderer::RenderEffects)
// are not drawn.
//
// Deviation: the mob_effect icons live in the GUI sprite atlas under
// "mob_effect/<name>" (assets/textures/gui/sprites/mob_effect/), where MC
// keeps them in their own mob_effect atlas under the same id.
#pragma once

#include "common/entity/effect/MobEffects.hpp"

#include <string>
#include <vector>

namespace Render {

    class GuiGraphics;

    namespace EffectsInInventory {

        // MC EffectsInInventory constants.
        inline constexpr int kSpacing          = 7;    // SPACING
        inline constexpr int kSpriteSquareSize = 32;   // SPRITE_SQUARE_SIZE
        inline constexpr int kTextXOffset      = 32;   // TEXT_X_OFFSET

        // MC canSeeEffects: at least 32 px right of the panel (+2).
        bool CanSeeEffects(int guiWidth, int leftPos, int imageWidth);

        // MC extractRenderState: the column at leftPos + imageWidth + 2 from
        // topPos down, sorted by MobEffectInstance.compareTo.
        void Render(GuiGraphics& g, const std::vector<Game::MobEffectInstance>& effects,
                    int guiWidth, int leftPos, int topPos, int imageWidth,
                    int mouseX, int mouseY);

        // MC Hud.getMobEffectSprite: "mob_effect/<registry path>".
        std::string EffectSprite(Game::MobEffectId id);

    } // namespace EffectsInInventory

    // MC Screen.showsActiveEffects for whichever container screen is open:
    // true while the survival or creative inventory shows the column.
    bool InventoryShowsActiveEffects(int guiWidth);

} // namespace Render
