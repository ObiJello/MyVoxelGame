// File: src/client/renderer/gui/GuiGraphics.cpp
#include "GuiGraphics.hpp"
#include "FontRenderer.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/entity/Inventory.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockModel.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_set>

namespace Render {

    GuiGraphics::GuiGraphics(int guiWidth, int guiHeight, GuiAtlas* atlas,
                             GuiRenderState* renderState, FontRenderer* fontRenderer)
        : m_guiWidth(guiWidth), m_guiHeight(guiHeight)
        , m_atlas(atlas), m_renderState(renderState), m_fontRenderer(fontRenderer)
        , m_currentTransform(1.0f) {
    }

    BlitCommand GuiGraphics::MakeBlit(TextureHandle tex, float x0, float y0, float x1, float y1,
                                       float u0, float v0, float u1, float v1, uint32_t color) const {
        BlitCommand cmd;
        cmd.texture = tex;
        cmd.x0 = x0; cmd.y0 = y0; cmd.x1 = x1; cmd.y1 = y1;
        cmd.u0 = u0; cmd.v0 = v0; cmd.u1 = u1; cmd.v1 = v1;
        cmd.color = color;
        cmd.transform = m_currentTransform;
        cmd.hasScissor = m_scissorActive;
        cmd.scissor = m_currentScissor;
        return cmd;
    }

    // --- Sprite rendering ---

    void GuiGraphics::BlitSprite(const std::string& spriteId, int x, int y, int width, int height) {
        BlitSprite(spriteId, x, y, width, height, 0xFFFFFFFF);
    }

    void GuiGraphics::BlitSprite(const std::string& spriteId, int x, int y, int width, int height, uint32_t color) {
        if (!m_atlas) return;
        const SpriteInfo* sprite = m_atlas->GetSprite(spriteId);
        if (!sprite) return;

        switch (sprite->scaling) {
            case SpriteScaling::Stretch:
                m_renderState->SubmitBlit(MakeBlit(
                    m_atlas->GetTextureHandle(),
                    static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(x + width), static_cast<float>(y + height),
                    sprite->u0, sprite->v0, sprite->u1, sprite->v1, color));
                break;
            case SpriteScaling::Tile:
                BlitTiled(*sprite, x, y, width, height, color);
                break;
            case SpriteScaling::NineSlice:
                BlitNineSlice(*sprite, x, y, width, height, color);
                break;
        }
    }

    void GuiGraphics::BlitSprite(const std::string& spriteId, int spriteW, int spriteH,
                                  int texX, int texY, int x, int y, int width, int height) {
        BlitSprite(spriteId, spriteW, spriteH, texX, texY, x, y, width, height, 0xFFFFFFFF);
    }

    void GuiGraphics::BlitSprite(const std::string& spriteId, int spriteW, int spriteH,
                                  int texX, int texY, int x, int y, int width, int height, uint32_t color) {
        if (!m_atlas) return;
        const SpriteInfo* sprite = m_atlas->GetSprite(spriteId);
        if (!sprite) return;

        // Calculate sub-region UVs within the sprite
        float uRange = sprite->u1 - sprite->u0;
        float vRange = sprite->v1 - sprite->v0;
        float subU0 = sprite->u0 + uRange * (static_cast<float>(texX) / spriteW);
        float subV0 = sprite->v0 + vRange * (static_cast<float>(texY) / spriteH);
        float subU1 = sprite->u0 + uRange * (static_cast<float>(texX + width) / spriteW);
        float subV1 = sprite->v0 + vRange * (static_cast<float>(texY + height) / spriteH);

        m_renderState->SubmitBlit(MakeBlit(
            m_atlas->GetTextureHandle(),
            static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(x + width), static_cast<float>(y + height),
            subU0, subV0, subU1, subV1, color));
    }

    void GuiGraphics::Blit(TextureHandle texture, int x0, int y0, int x1, int y1,
                           float u0, float v0, float u1, float v1, uint32_t color) {
        m_renderState->SubmitBlit(MakeBlit(texture,
            static_cast<float>(x0), static_cast<float>(y0),
            static_cast<float>(x1), static_cast<float>(y1),
            u0, v0, u1, v1, color));
    }

    // --- Primitives ---

    void GuiGraphics::Fill(int x0, int y0, int x1, int y1, uint32_t color) {
        FillCommand cmd;
        cmd.x0 = static_cast<float>(x0); cmd.y0 = static_cast<float>(y0);
        cmd.x1 = static_cast<float>(x1); cmd.y1 = static_cast<float>(y1);
        cmd.color0 = color;
        cmd.color1 = color;
        cmd.transform = m_currentTransform;
        cmd.hasScissor = m_scissorActive;
        cmd.scissor = m_currentScissor;
        m_renderState->SubmitFill(cmd);
    }

    void GuiGraphics::FillGradient(int x0, int y0, int x1, int y1, uint32_t colorTop, uint32_t colorBottom) {
        FillCommand cmd;
        cmd.x0 = static_cast<float>(x0); cmd.y0 = static_cast<float>(y0);
        cmd.x1 = static_cast<float>(x1); cmd.y1 = static_cast<float>(y1);
        cmd.color0 = colorTop;
        cmd.color1 = colorBottom;
        cmd.transform = m_currentTransform;
        cmd.hasScissor = m_scissorActive;
        cmd.scissor = m_currentScissor;
        m_renderState->SubmitFill(cmd);
    }

    void GuiGraphics::HLine(int x0, int x1, int y, uint32_t color) {
        Fill(x0, y, x1 + 1, y + 1, color);
    }

    void GuiGraphics::VLine(int x, int y0, int y1, uint32_t color) {
        Fill(x, y0, x + 1, y1 + 1, color);
    }

    void GuiGraphics::RenderOutline(int x, int y, int width, int height, uint32_t color) {
        HLine(x, x + width - 1, y, color);
        HLine(x, x + width - 1, y + height - 1, color);
        VLine(x, y + 1, y + height - 2, color);
        VLine(x + width - 1, y + 1, y + height - 2, color);
    }

    // --- Text rendering ---

    void GuiGraphics::DrawString(const std::string& text, int x, int y, uint32_t color, bool dropShadow) {
        TextCommand cmd;
        cmd.text = text;
        cmd.x = static_cast<float>(x);
        cmd.y = static_cast<float>(y);
        cmd.color = color;
        cmd.dropShadow = dropShadow;
        cmd.transform = m_currentTransform;
        cmd.hasScissor = m_scissorActive;
        cmd.scissor = m_currentScissor;
        m_renderState->SubmitText(cmd);
    }

    void GuiGraphics::DrawCenteredString(const std::string& text, int x, int y, uint32_t color) {
        int width = GetStringWidth(text);
        DrawString(text, x - width / 2, y, color);
    }

    void GuiGraphics::DrawStringWithBackdrop(const std::string& text, int x, int y, int width, uint32_t color) {
        // Semi-transparent black backdrop behind text (MC style)
        uint8_t alpha = (color >> 24) & 0xFF;
        uint32_t backdropColor = (static_cast<uint32_t>(alpha) << 24); // Black with same alpha
        Fill(x - 2, y - 2, x + width + 2, y + FontRenderer::LINE_HEIGHT + 1,
             (backdropColor & 0xFF000000) | 0x00000000);
        DrawString(text, x, y, color);
    }

    int GuiGraphics::GetStringWidth(const std::string& text) const {
        if (m_fontRenderer) return m_fontRenderer->GetStringWidth(text);
        return static_cast<int>(text.size()) * 6; // Fallback estimate
    }

    // --- Item rendering ---

    // Default plains biome grass/foliage color for GUI tinting (MC default)
    static constexpr uint32_t GUI_GRASS_TINT = 0xFF7CBE6B;  // R=124, G=190, B=107

    // Multiply two ARGB colors component-wise (used for tint * shading)
    static uint32_t MultiplyColors(uint32_t c1, uint32_t c2) {
        uint32_t a = ((c1 >> 24) & 0xFF) * ((c2 >> 24) & 0xFF) / 255;
        uint32_t r = ((c1 >> 16) & 0xFF) * ((c2 >> 16) & 0xFF) / 255;
        uint32_t g = ((c1 >> 8) & 0xFF) * ((c2 >> 8) & 0xFF) / 255;
        uint32_t b = (c1 & 0xFF) * (c2 & 0xFF) / 255;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }

    struct FaceLookupResult {
        AtlasUVRect uv;
        int tintIndex = -1;
        bool found = false;
    };

    // Helper: look up a face texture in the block atlas, including tint info
    static FaceLookupResult LookupFaceTexture(const Game::BlockModel& model, Game::FaceDir dir,
                                               const std::string& modelName) {
        FaceLookupResult result;
        if (!g_atlasBuilder) return result;

        // Try all elements (grass_block has 2: base + overlay)
        for (const auto& elem : model.elements) {
            if (elem.HasFace(dir)) {
                const auto& face = elem.GetFace(dir);
                std::string texPath = model.ResolveTexture(face.textureRef);
                if (texPath != "missingno" && g_atlasBuilder->GetUVRect(texPath, result.uv)) {
                    result.tintIndex = face.tintIndex;
                    result.found = true;
                    return result;
                }
            }
        }

        // Fallback: try common key patterns
        static const std::string suffixes[] = { "", "_top", "_side", "_front" };
        for (const auto& suffix : suffixes) {
            if (g_atlasBuilder->GetUVRect("block/" + modelName + suffix, result.uv)) {
                result.found = true;
                return result;
            }
        }
        return result;
    }

    // Transform a 3D point through the isometric matrix and project to 2D screen coords
    static glm::vec2 ProjectIsometric(const glm::mat4& isoMat, const glm::vec3& point) {
        glm::vec4 p = isoMat * glm::vec4(point, 1.0f);
        return glm::vec2(p.x, -p.y); // Negate Y: 3D Y-up → screen Y-down
    }

    void GuiGraphics::RenderItem(const Game::InventorySlot& slot, int x, int y) {
        if (slot.IsEmpty()) return;

        const auto& block = Game::BlockRegistry::Get(slot.blockId);
        const std::string& modelName = block.modelName;
        if (modelName.empty()) return;
        if (!g_atlasBuilder) return;

        // MC isometric: rotationXYZ(30°, 225°, 0°) * scale(0.625) * translate(-0.5)
        // Verified with numpy: R = Rx(30°) @ Ry(225°) gives visible Top, North, East faces.
        // In GLM (right-multiply): Rx first, then Ry → builds Rx * Ry.
        static glm::mat4 isoMat = [] {
            glm::mat4 m(1.0f);
            m = glm::rotate(m, glm::radians(30.0f),  glm::vec3(1, 0, 0)); // Rx
            m = glm::rotate(m, glm::radians(225.0f), glm::vec3(0, 1, 0)); // Rx * Ry
            m = glm::scale(m, glm::vec3(0.625f));
            m = glm::translate(m, glm::vec3(-0.5f, -0.5f, -0.5f));
            return m;
        }();

        const auto& model = Game::BlockRegistry::GetBlockModel(slot.blockId);

        // Look up textures for 3 visible faces (Top, East=left, North=right)
        auto topFace = LookupFaceTexture(model, Game::FaceDir::Up, modelName);
        auto leftFace = LookupFaceTexture(model, Game::FaceDir::East, modelName);
        auto rightFace = LookupFaceTexture(model, Game::FaceDir::North, modelName);

        if (!topFace.found && !leftFace.found && !rightFace.found) {
            auto fallback = LookupFaceTexture(model, Game::FaceDir::South, modelName);
            if (!fallback.found) return;
            topFace = leftFace = rightFace = fallback;
        }
        if (!topFace.found) topFace = leftFace.found ? leftFace : rightFace;
        if (!leftFace.found) leftFace = topFace.found ? topFace : rightFace;
        if (!rightFace.found) rightFace = topFace.found ? topFace : leftFace;

        // MC uses scale(16, -16, 16) in renderItemToAtlas — the model's 0.625 scale
        // already sizes the block to fit in 16 GUI pixels.
        float scale = 16.0f;
        float cx = static_cast<float>(x) + 8.0f;  // Center of 16x16 slot
        float cy = static_cast<float>(y) + 8.0f;

        auto proj = [&](float mx, float my, float mz) -> glm::vec2 {
            glm::vec2 p = ProjectIsometric(isoMat, glm::vec3(mx, my, mz));
            return glm::vec2(cx + p.x * scale, cy + p.y * scale);
        };

        TextureHandle tex = g_atlasBuilder->GetBackendTextureHandle();

        // Submit one face as a quad with 4 corners defined in 3D model space
        auto submitFace = [&](glm::vec3 v0, glm::vec3 v1, glm::vec3 v2, glm::vec3 v3,
                             const AtlasUVRect& uv, uint32_t color) {
            glm::vec2 p0 = proj(v0.x, v0.y, v0.z);
            glm::vec2 p1 = proj(v1.x, v1.y, v1.z);
            glm::vec2 p2 = proj(v2.x, v2.y, v2.z);
            glm::vec2 p3 = proj(v3.x, v3.y, v3.z);

            QuadCommand q;
            q.texture = tex;
            q.color = color;
            q.px[0] = p0.x; q.py[0] = p0.y;
            q.px[1] = p1.x; q.py[1] = p1.y;
            q.px[2] = p2.x; q.py[2] = p2.y;
            q.px[3] = p3.x; q.py[3] = p3.y;
            q.u[0] = uv.uvMin.x; q.v[0] = uv.uvMin.y;
            q.u[1] = uv.uvMax.x; q.v[1] = uv.uvMin.y;
            q.u[2] = uv.uvMax.x; q.v[2] = uv.uvMax.y;
            q.u[3] = uv.uvMin.x; q.v[3] = uv.uvMax.y;
            m_renderState->SubmitQuad(q);
        };

        // Compute tinted shading colors per face
        // MC shading: top=100%, right side=~80%, left side=~60%
        // If tintIndex >= 0, multiply by biome grass color
        auto faceColor = [](const FaceLookupResult& face, uint32_t shading) -> uint32_t {
            if (face.tintIndex >= 0) {
                return MultiplyColors(GUI_GRASS_TINT, shading);
            }
            return shading;
        };

        // Visible faces: Top(+Y), East(+X) left, North(-Z) right
        // Draw back-to-front. Vertex order: TL, TR, BR, BL (matching UV corners)

        // 1. East face (+X, left side) — darkest
        submitFace({1,1,1}, {1,1,0}, {1,0,0}, {1,0,1},
                   leftFace.uv, faceColor(leftFace, 0xFF999999));

        // 2. North face (-Z, right side) — medium
        submitFace({0,1,0}, {1,1,0}, {1,0,0}, {0,0,0},
                   rightFace.uv, faceColor(rightFace, 0xFFCCCCCC));

        // 3. Top face (+Y, on top) — brightest
        submitFace({0,1,1}, {1,1,1}, {1,1,0}, {0,1,0},
                   topFace.uv, faceColor(topFace, 0xFFFFFFFF));
    }

    void GuiGraphics::RenderItemDecorations(const Game::InventorySlot& slot, int x, int y) {
        if (slot.IsEmpty()) return;
        if (slot.count <= 1) return;

        // Draw stack count in bottom-right corner (MC style)
        std::string countStr = std::to_string(slot.count);
        int textWidth = GetStringWidth(countStr);
        DrawString(countStr, x + 17 - textWidth, y + 9, 0xFFFFFFFF, true);
    }

    // --- Scissors ---

    void GuiGraphics::EnableScissor(int x0, int y0, int x1, int y1) {
        ScissorRect rect = { x0, y0, x1, y1 };

        // If there's already a scissor, intersect with it
        if (m_scissorActive) {
            rect.x0 = std::max(rect.x0, m_currentScissor.x0);
            rect.y0 = std::max(rect.y0, m_currentScissor.y0);
            rect.x1 = std::min(rect.x1, m_currentScissor.x1);
            rect.y1 = std::min(rect.y1, m_currentScissor.y1);
        }

        m_scissorStack.push_back(m_currentScissor);
        m_currentScissor = rect;
        m_scissorActive = true;
    }

    void GuiGraphics::DisableScissor() {
        if (!m_scissorStack.empty()) {
            m_currentScissor = m_scissorStack.back();
            m_scissorStack.pop_back();
            m_scissorActive = !m_scissorStack.empty();
        } else {
            m_scissorActive = false;
        }
    }

    // --- Transform stack ---

    void GuiGraphics::PushMatrix() {
        m_matrixStack.push_back(m_currentTransform);
    }

    void GuiGraphics::PopMatrix() {
        if (!m_matrixStack.empty()) {
            m_currentTransform = m_matrixStack.back();
            m_matrixStack.pop_back();
        }
    }

    void GuiGraphics::Translate(float x, float y) {
        m_currentTransform[2][0] += m_currentTransform[0][0] * x + m_currentTransform[1][0] * y;
        m_currentTransform[2][1] += m_currentTransform[0][1] * x + m_currentTransform[1][1] * y;
    }

    void GuiGraphics::Scale(float sx, float sy) {
        m_currentTransform[0][0] *= sx;
        m_currentTransform[0][1] *= sx;
        m_currentTransform[1][0] *= sy;
        m_currentTransform[1][1] *= sy;
    }

    void GuiGraphics::NextStratum() {
        m_renderState->NextStratum();
    }

    // --- Nine-slice rendering ---

    void GuiGraphics::BlitNineSlice(const SpriteInfo& sprite, int x, int y, int width, int height, uint32_t color) {
        TextureHandle tex = m_atlas->GetTextureHandle();
        float uRange = sprite.u1 - sprite.u0;
        float vRange = sprite.v1 - sprite.v0;

        // Border sizes in pixels (clamped to half of dimensions)
        int bl = std::min(sprite.border.left, width / 2);
        int br = std::min(sprite.border.right, width / 2);
        int bt = std::min(sprite.border.top, height / 2);
        int bb = std::min(sprite.border.bottom, height / 2);

        // UV border fractions
        float ubl = static_cast<float>(sprite.border.left) / sprite.width;
        float ubr = static_cast<float>(sprite.border.right) / sprite.width;
        float vbt = static_cast<float>(sprite.border.top) / sprite.height;
        float vbb = static_cast<float>(sprite.border.bottom) / sprite.height;

        auto blit = [&](int px0, int py0, int px1, int py1, float su0, float sv0, float su1, float sv1) {
            float fu0 = sprite.u0 + uRange * su0;
            float fv0 = sprite.v0 + vRange * sv0;
            float fu1 = sprite.u0 + uRange * su1;
            float fv1 = sprite.v0 + vRange * sv1;
            m_renderState->SubmitBlit(MakeBlit(tex,
                static_cast<float>(px0), static_cast<float>(py0),
                static_cast<float>(px1), static_cast<float>(py1),
                fu0, fv0, fu1, fv1, color));
        };

        // Corners
        blit(x, y, x + bl, y + bt, 0, 0, ubl, vbt);                                    // TL
        blit(x + width - br, y, x + width, y + bt, 1.0f - ubr, 0, 1, vbt);             // TR
        blit(x, y + height - bb, x + bl, y + height, 0, 1.0f - vbb, ubl, 1);           // BL
        blit(x + width - br, y + height - bb, x + width, y + height, 1.0f - ubr, 1.0f - vbb, 1, 1); // BR

        // Edges
        blit(x + bl, y, x + width - br, y + bt, ubl, 0, 1.0f - ubr, vbt);              // Top
        blit(x + bl, y + height - bb, x + width - br, y + height, ubl, 1.0f - vbb, 1.0f - ubr, 1); // Bottom
        blit(x, y + bt, x + bl, y + height - bb, 0, vbt, ubl, 1.0f - vbb);             // Left
        blit(x + width - br, y + bt, x + width, y + height - bb, 1.0f - ubr, vbt, 1, 1.0f - vbb); // Right

        // Center
        blit(x + bl, y + bt, x + width - br, y + height - bb, ubl, vbt, 1.0f - ubr, 1.0f - vbb);
    }

    // --- Tile rendering ---

    void GuiGraphics::BlitTiled(const SpriteInfo& sprite, int x, int y, int width, int height, uint32_t color) {
        TextureHandle tex = m_atlas->GetTextureHandle();
        int tw = sprite.tileWidth > 0 ? sprite.tileWidth : sprite.width;
        int th = sprite.tileHeight > 0 ? sprite.tileHeight : sprite.height;

        for (int ty = 0; ty < height; ty += th) {
            for (int tx = 0; tx < width; tx += tw) {
                int drawW = std::min(tw, width - tx);
                int drawH = std::min(th, height - ty);
                float uFrac = static_cast<float>(drawW) / tw;
                float vFrac = static_cast<float>(drawH) / th;
                float u1 = sprite.u0 + (sprite.u1 - sprite.u0) * uFrac;
                float v1 = sprite.v0 + (sprite.v1 - sprite.v0) * vFrac;
                m_renderState->SubmitBlit(MakeBlit(tex,
                    static_cast<float>(x + tx), static_cast<float>(y + ty),
                    static_cast<float>(x + tx + drawW), static_cast<float>(y + ty + drawH),
                    sprite.u0, sprite.v0, u1, v1, color));
            }
        }
    }

} // namespace Render
