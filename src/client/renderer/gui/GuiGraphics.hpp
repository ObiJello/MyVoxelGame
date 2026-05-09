// File: src/client/renderer/gui/GuiGraphics.hpp
// Immediate-mode GUI drawing API matching MC's GuiGraphics.
// All methods submit commands to GuiRenderState for deferred rendering.
#pragma once

#include "GuiRenderState.hpp"
#include "GuiAtlas.hpp"
#include "../backend/RenderTypes.hpp"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>

#include "common/entity/Item.hpp"

namespace Render {

    class FontRenderer;

    class GuiGraphics {
    public:
        GuiGraphics(int guiWidth, int guiHeight, GuiAtlas* atlas,
                    GuiRenderState* renderState, FontRenderer* fontRenderer);

        // --- Sprite rendering (MC: blitSprite) ---
        void BlitSprite(const std::string& spriteId, int x, int y, int width, int height);
        void BlitSprite(const std::string& spriteId, int x, int y, int width, int height, uint32_t color);
        // Partial sprite sub-region (for progress bars)
        void BlitSprite(const std::string& spriteId, int spriteW, int spriteH,
                       int texX, int texY, int x, int y, int width, int height);
        void BlitSprite(const std::string& spriteId, int spriteW, int spriteH,
                       int texX, int texY, int x, int y, int width, int height, uint32_t color);

        // Raw texture blit with explicit UVs
        void Blit(TextureHandle texture, int x0, int y0, int x1, int y1,
                 float u0, float v0, float u1, float v1, uint32_t color = 0xFFFFFFFF);

        // --- Primitives ---
        void Fill(int x0, int y0, int x1, int y1, uint32_t color);
        void FillGradient(int x0, int y0, int x1, int y1, uint32_t colorTop, uint32_t colorBottom);
        void HLine(int x0, int x1, int y, uint32_t color);
        void VLine(int x, int y0, int y1, uint32_t color);
        void RenderOutline(int x, int y, int width, int height, uint32_t color);

        // --- Text rendering ---
        void DrawString(const std::string& text, int x, int y, uint32_t color, bool dropShadow = true);
        void DrawCenteredString(const std::string& text, int x, int y, uint32_t color);
        void DrawStringWithBackdrop(const std::string& text, int x, int y, int width, uint32_t color);
        int GetStringWidth(const std::string& text) const;

        // --- Item rendering ---
        // Single entry point matching MC's GuiGraphics.renderItem(ItemStack, x, y) — dispatches
        // on the item's renderType OR a registered custom renderer:
        //   1. If the item has a CustomItemRenderer registered, that runs (BEWLR-style —
        //      e.g. chest, sign, banner, head, bed, all use this in MC).
        //   2. ItemRenderType::Block  → 3D isometric mini-cube (uses BlockModel + atlas)
        //   3. ItemRenderType::Sprite → flat 16×16 texture (lazy-loaded from assets/textures/item/)
        void RenderItem(const Game::ItemStack& stack, int x, int y);
        // Pre-load any sprite-type item textures so the first render frame after Open()
        // can use them without a one-frame latency.
        static void PreloadItem(Game::ItemID itemId);
        void RenderItemDecorations(const Game::ItemStack& stack, int x, int y);

        // Custom item renderers — MC's BlockEntityWithoutLevelRenderer equivalent. Items
        // that are normally drawn by a block-entity renderer in the world (chest, sign,
        // bell, banner, etc.) can register a function here to draw their inventory icon
        // however they want (typically a 3D entity-textured model). Called from RenderItem
        // BEFORE the standard block/sprite dispatch.
        using CustomItemRenderer = void(*)(GuiGraphics& g, const Game::ItemStack& stack, int x, int y);
        static void RegisterCustomItemRenderer(Game::ItemID id, CustomItemRenderer renderer);

        // Direct access to the underlying render state — needed by custom item renderers
        // that submit 3D quads (BEWLR-style chest, banner, etc.). Mirrors how MC's BEWLR
        // gets a `MultiBufferSource` from the GuiGraphics during inventory rendering.
        GuiRenderState* GetRenderState() { return m_renderState; }

        // --- Scissors/Clipping ---
        void EnableScissor(int x0, int y0, int x1, int y1);
        void DisableScissor();

        // --- Transform stack (MC: Matrix3x2fStack) ---
        void PushMatrix();
        void PopMatrix();
        void Translate(float x, float y);
        void Scale(float sx, float sy);

        // --- Z-ordering ---
        void NextStratum();

        // --- Dimensions ---
        int GuiWidth() const { return m_guiWidth; }
        int GuiHeight() const { return m_guiHeight; }

        // --- Font ---
        const FontRenderer* GetFontRenderer() const { return m_fontRenderer; }

    private:
        int m_guiWidth, m_guiHeight;
        GuiAtlas* m_atlas;
        GuiRenderState* m_renderState;
        FontRenderer* m_fontRenderer;

        // Matrix stack
        std::vector<glm::mat3x2> m_matrixStack;
        glm::mat3x2 m_currentTransform{1.0f};

        // Scissor stack
        bool m_scissorActive = false;
        std::vector<ScissorRect> m_scissorStack;
        ScissorRect m_currentScissor;

        // Helper: create a BlitCommand with current state
        BlitCommand MakeBlit(TextureHandle tex, float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1, uint32_t color) const;

        // Nine-slice rendering
        void BlitNineSlice(const SpriteInfo& sprite, int x, int y, int width, int height, uint32_t color);
        // Tile rendering
        void BlitTiled(const SpriteInfo& sprite, int x, int y, int width, int height, uint32_t color);
    };

} // namespace Render
