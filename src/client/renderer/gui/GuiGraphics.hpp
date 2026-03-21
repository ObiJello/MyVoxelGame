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

namespace Game { struct InventorySlot; }

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
        void RenderItem(const Game::InventorySlot& slot, int x, int y);
        void RenderItemDecorations(const Game::InventorySlot& slot, int x, int y);

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
