// File: src/client/renderer/gui/FurnaceScreen.hpp
//
// Mirrors net.minecraft.client.gui.screens.inventory.AbstractFurnaceScreen —
// the furnace, blast furnace and smoker UI. One class covers all three; only
// the panel texture differs (furnace.png / blast_furnace.png / smoker.png).
//
// The two moving parts are the flame under the fuel slot and the arrow between
// input and output. Both are sub-rect blits sized from the menu's ContainerData
// — the counters the block entity ticks — so they animate without the screen
// knowing anything about smelting.
//
// Both come from the GUI SPRITE ATLAS, not from the panel sheet. Modern MC
// moved every moving GUI element out of the container .png into
// textures/gui/sprites/** (renderBg blits the panel, then blitSprite's the
// flame and arrow). The panel png is still padded out to 256x256, so the old
// pre-1.20.2 atlas coordinates — flame at u=176,v=0 — still sample IN BOUNDS
// and silently return transparent padding. That reads as "the animation is
// broken" rather than as a missing texture, so check the sprite ids first if
// these ever stop drawing.
#pragma once

#include "AbstractContainerScreen.hpp"
#include "../backend/RenderTypes.hpp"
#include "common/inventory/MenuType.hpp"
#include <string>

namespace Render {

    class FurnaceScreen : public AbstractContainerScreen {
    public:
        static constexpr int IMAGE_W = 176;
        static constexpr int IMAGE_H = 166;
        static constexpr int TITLE_X = 8;
        static constexpr int TITLE_Y = 6;
        static constexpr int INV_LABEL_X = 8;
        static constexpr int INV_LABEL_Y = IMAGE_H - 94;
        static constexpr uint32_t LABEL_COLOR = 0xFF404040;

        // Sprite geometry, from MC AbstractFurnaceScreen.renderBg. The flame
        // sprite is 14x14 and drawn BOTTOM-UP — a k-pixel flame is the BOTTOM
        // k rows of the sprite, blitted k pixels further down — which is why
        // both the source v and the destination y advance as fuel burns off.
        static constexpr int FLAME_X = 56, FLAME_Y = 36, FLAME_W = 14, FLAME_H = 14;
        static constexpr int ARROW_X = 79, ARROW_Y = 34, ARROW_W = 24, ARROW_H = 16;

        void Configure(Game::MenuType type, const std::string& title);

    protected:
        int ImageWidth()  const override { return IMAGE_W; }
        int ImageHeight() const override { return IMAGE_H; }

        void RenderBg(GuiGraphics& g, int leftPos, int topPos) override;
        void RenderLabels(GuiGraphics& g, int leftPos, int topPos) override;

    private:
        TextureHandle EnsureBackground();

        const char*   m_texture = "assets/textures/gui/container/furnace.png";
        TextureHandle m_background = INVALID_TEXTURE;
        bool          m_backgroundTried = false;
        std::string   m_title = "Furnace";

        // GUI-atlas sprite ids. Each of the three cookers ships its OWN pair
        // (MC FurnaceScreen / BlastFurnaceScreen / SmokerScreen each name
        // container/<block>/{lit,burn}_progress) — the blast furnace's flame
        // and the smoker's are drawn differently, so these are not shareable.
        std::string   m_litSprite  = "container/furnace/lit_progress";
        std::string   m_burnSprite = "container/furnace/burn_progress";
    };

    FurnaceScreen& GetFurnaceScreen();

} // namespace Render
