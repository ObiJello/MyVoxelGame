// File: src/client/renderer/gui/GuiGraphics.cpp
#include "GuiGraphics.hpp"
#include "FontRenderer.hpp"
#include "items/ItemLighting.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/entity/Inventory.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockModel.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <cmath>
#include <GLFW/glfw3.h>
#include "stb_image.h"

// PlatformMain provides bundle-aware asset path resolution.
namespace PlatformMain { std::string GetAssetPath(const std::string&); }

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

    // Transform a 3D point through the isometric matrix and project to 2D screen coords
    static glm::vec2 ProjectIsometric(const glm::mat4& isoMat, const glm::vec3& point) {
        glm::vec4 p = isoMat * glm::vec4(point, 1.0f);
        return glm::vec2(p.x, -p.y); // Negate Y: 3D Y-up → screen Y-down
    }

    // Internal: render a 3D isometric block face stack for a BlockID. Defined first so
    // both the dispatcher AND the LoadItemTexture-using sprite path below can call it.
    static void RenderBlockItemImpl(GuiGraphics& self,
                                    GuiRenderState* renderState,
                                    Game::BlockID blockId,
                                    const std::string& modelOverride,
                                    int x, int y) {
        const auto& block = Game::BlockRegistry::Get(blockId);
        // MC's items/{slug}.json may override which block model the inventory
        // renderer uses (trapdoors → `<wood>_trapdoor_bottom`, doors → `<wood>_door_bottom`,
        // etc.). When set, prefer it over the block's own modelName.
        const std::string& modelName = !modelOverride.empty() ? modelOverride : block.modelName;
        if (modelName.empty()) return;
        if (!g_atlasBuilder) return;

        // Try inventory-specific model first (e.g., red_mushroom_block_inventory,
        // oak_fence_inventory), fall back to the regular block model. MC uses
        // separate inventory models for blocks whose world model doesn't show
        // all faces (mushroom blocks) AND whose inventory orientation differs
        // from the world block (fences).
        const Game::BlockModel* modelPtr = nullptr;
        if (Game::BlockModelRegistry::HasModel(modelName + "_inventory")) {
            modelPtr = &Game::BlockModelRegistry::GetModel(modelName + "_inventory");
        } else if (!modelOverride.empty() && Game::BlockModelRegistry::HasModel(modelName)) {
            // The override targets a SPECIFIC variant model (e.g. oak_trapdoor_bottom),
            // not whatever BlockRegistry::GetBlockModel returns for this BlockID
            // (which is keyed off the stateful block name). Look up the override directly.
            modelPtr = &Game::BlockModelRegistry::GetModel(modelName);
        } else {
            modelPtr = &Game::BlockRegistry::GetBlockModel(blockId);
        }
        const auto& model = *modelPtr;

        // MC isometric: build the iso matrix from the model's `display.gui` transform,
        // mirroring MC's `ItemTransform.apply` (translate → rotate → scale, applied to
        // the model AFTER it's centered at the origin). Most blocks use the defaults
        // from `block/block` (rotation [30,225,0], scale 0.625), but fences override
        // to [30,135,0], gates to [30,45,0]+scale 0.8+translation [0,-1,0], etc.
        // Translation values are in MC "pixel units" (1/16 of a block); divide by 16
        // to get the [0..1] model space we use here.
        const Game::GuiDisplay& gui = model.guiDisplay;
        glm::mat4 isoMat(1.0f);
        isoMat = glm::translate(isoMat, gui.translation / 16.0f);
        isoMat = glm::rotate(isoMat, glm::radians(gui.rotation.x), glm::vec3(1, 0, 0));
        isoMat = glm::rotate(isoMat, glm::radians(gui.rotation.y), glm::vec3(0, 1, 0));
        isoMat = glm::rotate(isoMat, glm::radians(gui.rotation.z), glm::vec3(0, 0, 1));
        isoMat = glm::scale(isoMat, gui.scale);
        isoMat = glm::translate(isoMat, glm::vec3(-0.5f, -0.5f, -0.5f));

        // Depth matrix used ONLY for back-to-front sort. We deliberately omit the
        // X rotation so the iso tilt's Y-into-Z mixing doesn't dominate the sort
        // key. Without this, a face with high Y-extent (e.g. a shelf's back panel,
        // y=0..16) gets a higher transformed Z than a smaller face that is
        // geometrically in front of it but at low Y (e.g. a bottom shelf board,
        // y=0..4) — and the back panel paints over the board.
        // MC doesn't hit this because it uses a real Z-buffer; we use painter's
        // algorithm with face-center depth, which only orders by world-Z direction
        // (post-Y-rotation) reliably. The full iso transform is still used for
        // VERTEX projection so positions on screen remain identical.
        glm::mat4 depthMat(1.0f);
        depthMat = glm::rotate(depthMat, glm::radians(gui.rotation.y), glm::vec3(0, 1, 0));
        depthMat = glm::translate(depthMat, glm::vec3(-0.5f, -0.5f, -0.5f));

        // Blocks without a real model file (water_still, lava_still) get the default
        // cube which has unresolvable textures. Render flat 2D texture instead.
        if (!Game::BlockModelRegistry::HasModel(modelName) &&
            !Game::BlockModelRegistry::HasModel(modelName + "_inventory")) {
            AtlasUVRect uvRect;
            bool found = false;
            std::string texKey = model.ResolveTexture("#particle");
            if (texKey != "missingno") found = g_atlasBuilder->GetUVRect(texKey, uvRect);
            if (!found) found = g_atlasBuilder->GetUVRect("block/" + modelName, uvRect);
            if (!found && modelName.size() > 6 && modelName.substr(modelName.size() - 6) == "_still") {
                found = g_atlasBuilder->GetUVRect("block/" + modelName.substr(0, modelName.size() - 6), uvRect);
            }
            if (found) {
                // For animated textures, clamp UV to first frame (square)
                float frameHeight = uvRect.uvMax.x - uvRect.uvMin.x;
                float clampedV1 = uvRect.uvMin.y + frameHeight;
                if (clampedV1 < uvRect.uvMax.y) {
                    uvRect.uvMax.y = clampedV1;
                }
                // Water is a greyscale texture tinted blue
                uint32_t tint = 0xFFFFFFFF;
                if (modelName.find("water") != std::string::npos) {
                    tint = 0xFF4C7FFF; // R=76, G=127, B=255 (matches FluidMeshBuilder waterTint)
                }
                self.Blit(g_atlasBuilder->GetBackendTextureHandle(),
                          x, y, x + 16, y + 16,
                          uvRect.uvMin.x, uvRect.uvMin.y, uvRect.uvMax.x, uvRect.uvMax.y, tint);
            }
            return;
        }

        // Sanity: any visible face on any element? If not, this is a BEWLR block
        // (chest/sign/banner/bed) — fall back to flat particle sprite as before.
        bool anyVisible = false;
        for (const auto& elem : model.elements) {
            if (elem.HasFace(Game::FaceDir::Up) || elem.HasFace(Game::FaceDir::East)
                || elem.HasFace(Game::FaceDir::North) || elem.HasFace(Game::FaceDir::South)
                || elem.HasFace(Game::FaceDir::West) || elem.HasFace(Game::FaceDir::Down)) {
                anyVisible = true;
                break;
            }
        }
        if (!anyVisible) {
            AtlasUVRect uvRect;
            std::string particleTex = model.ResolveTexture("#particle");
            bool found = false;
            if (particleTex != "missingno") {
                found = g_atlasBuilder->GetUVRect(particleTex, uvRect);
            }
            if (!found) {
                found = g_atlasBuilder->GetUVRect("block/" + modelName, uvRect);
            }
            if (found) {
                self.Blit(g_atlasBuilder->GetBackendTextureHandle(),
                          x, y, x + 16, y + 16,
                          uvRect.uvMin.x, uvRect.uvMin.y, uvRect.uvMax.x, uvRect.uvMax.y,
                          0xFFFFFFFF);
            }
            return;
        }

        // MC uses scale(16, -16, 16) in renderItemToAtlas — the model's 0.625 scale
        // already sizes the block to fit in 16 GUI pixels.
        const float scale = 16.0f;
        const float cx = static_cast<float>(x) + 8.0f;  // Center of 16x16 slot
        const float cy = static_cast<float>(y) + 8.0f;

        auto proj = [&](float mx, float my, float mz) -> glm::vec2 {
            glm::vec2 p = ProjectIsometric(isoMat, glm::vec3(mx, my, mz));
            return glm::vec2(cx + p.x * scale, cy + p.y * scale);
        };

        TextureHandle tex = g_atlasBuilder->GetBackendTextureHandle();

        // MC ITEMS_3D Lambertian shading per face — uses the model's actual GUI
        // rotation so 135°/45° rotations (fences, walls, stairs, gates) get the
        // right per-face shading too, not just the default 225°.
        const float ROT_X = gui.rotation.x;
        const float ROT_Y = gui.rotation.y;
        const uint32_t SHADE_UP    = ItemLighting::ShadeAsColor(ItemLighting::ComputeShade({ 0, 1, 0}, ROT_X, ROT_Y));
        const uint32_t SHADE_DOWN  = ItemLighting::ShadeAsColor(ItemLighting::ComputeShade({ 0,-1, 0}, ROT_X, ROT_Y));
        const uint32_t SHADE_EAST  = ItemLighting::ShadeAsColor(ItemLighting::ComputeShade({ 1, 0, 0}, ROT_X, ROT_Y));
        const uint32_t SHADE_WEST  = ItemLighting::ShadeAsColor(ItemLighting::ComputeShade({-1, 0, 0}, ROT_X, ROT_Y));
        const uint32_t SHADE_NORTH = ItemLighting::ShadeAsColor(ItemLighting::ComputeShade({ 0, 0,-1}, ROT_X, ROT_Y));
        const uint32_t SHADE_SOUTH = ItemLighting::ShadeAsColor(ItemLighting::ComputeShade({ 0, 0, 1}, ROT_X, ROT_Y));

        // Map a (faceUV pixel space 0-16) coord to an atlas UV using the face's tile.
        auto atlasU = [](const AtlasUVRect& tile, float px) {
            return tile.uvMin.x + (px / 16.0f) * (tile.uvMax.x - tile.uvMin.x);
        };
        auto atlasV = [](const AtlasUVRect& tile, float px) {
            return tile.uvMin.y + (px / 16.0f) * (tile.uvMax.y - tile.uvMin.y);
        };

        auto biomeTinted = [](int tintIndex, uint32_t shading) -> uint32_t {
            if (tintIndex >= 0) return ItemLighting::MultiplyColor(GUI_GRASS_TINT, shading);
            return shading;
        };

        // One pre-computed face quad: 4 model-space corners + per-corner atlas UVs +
        // shading colour. Built once per element-face, then sorted by post-iso depth.
        struct FaceQuad {
            glm::vec3 v[4];
            float     u[4], v_[4];
            uint32_t  color;
            float     depth;  // average post-iso z; back-to-front = ascending z
        };

        auto resolveFace = [&](const Game::Element& elem, Game::FaceDir dir,
                               AtlasUVRect& outAtlas, glm::vec4& outFaceUV, int& outTint) -> bool {
            if (!elem.HasFace(dir)) return false;
            const auto& face = elem.GetFace(dir);
            std::string texPath = model.ResolveTexture(face.textureRef);
            if (texPath == "missingno") return false;
            if (!g_atlasBuilder->GetUVRect(texPath, outAtlas)) {
                if (!g_atlasBuilder->GetUVRect("block/" + modelName, outAtlas)) return false;
            }
            outFaceUV = face.uv;
            outTint = face.tintIndex;
            return true;
        };

        // Back-face culling — skip faces whose world-space normal points AWAY
        // from camera. Essential for translucent blocks (glass, stained glass,
        // ice) so back faces don't show THROUGH the front ones' alpha-blended
        // pixels and accumulate extra shading. For opaque/cutout blocks it's
        // a no-op visually (back faces would have been hidden by front faces
        // in painter's order anyway).
        const glm::mat3 isoNormalMat(isoMat);
        auto isFrontFacing = [&](glm::vec3 cubeLocalNormal) -> bool {
            // Our ortho convention (per chest renderer comment): higher world z
            // = closer to camera. So a face with world-normal.z > 0 faces the
            // camera. Use a tiny epsilon to avoid edge cases at z ≈ 0.
            glm::vec3 worldNormal = isoNormalMat * cubeLocalNormal;
            return worldNormal.z > 1e-4f;
        };

        // Build a FaceQuad with the right vertex order + UV mapping for the given
        // direction. Vertex order matches MC's per-face winding so the texture
        // appears upright after iso projection. Each face's 4 vertices are
        // arranged TL → TR → BR → BL of the face as the artist would see it
        // looking AT that face from outside the cube.
        auto buildFace = [&](FaceQuad& fq, glm::vec3 v0, glm::vec3 v1,
                             glm::vec3 v2, glm::vec3 v3,
                             const AtlasUVRect& tile, glm::vec4 fu,
                             uint32_t color) {
            fq.v[0] = v0; fq.v[1] = v1; fq.v[2] = v2; fq.v[3] = v3;
            fq.u[0] = atlasU(tile, fu.x); fq.v_[0] = atlasV(tile, fu.y);
            fq.u[1] = atlasU(tile, fu.z); fq.v_[1] = atlasV(tile, fu.y);
            fq.u[2] = atlasU(tile, fu.z); fq.v_[2] = atlasV(tile, fu.w);
            fq.u[3] = atlasU(tile, fu.x); fq.v_[3] = atlasV(tile, fu.w);
            fq.color = color;
            // Depth = average z of the 4 corners after the Ry-only depth matrix.
            // See the comment above where depthMat is built for why we don't include
            // the Rx tilt here.
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i) {
                glm::vec4 t = depthMat * glm::vec4(fq.v[i], 1.0f);
                sum += t.z;
            }
            fq.depth = sum * 0.25f;
        };

        // Build ALL faces from ALL elements first, then sort once globally back-to-front.
        // Per-element sorting is wrong for two reasons:
        //   1. Multi-element models like grass_block (base cube + overlay cube at same
        //      coords) need overlay-front to draw AFTER base-front, but per-element sort
        //      ends up drawing overlay-back AFTER base-front because element 1 starts
        //      from its own back face.
        //   2. Fences/walls/gates have multiple posts and bars at different depths that
        //      occlude each other in non-trivial ways — only a global depth sort gets
        //      the visibility right.
        // For TIES (overlapping coplanar faces from different elements), std::stable_sort
        // preserves submission order — and we submit base elements before overlays, so
        // overlay correctly draws over base on the front-visible faces.
        std::vector<FaceQuad> quads;
        quads.reserve(model.elements.size() * 6);
        for (const auto& elem : model.elements) {
            const float fx = elem.from.x / 16.0f, fy = elem.from.y / 16.0f, fz = elem.from.z / 16.0f;
            const float tx = elem.to.x   / 16.0f, ty = elem.to.y   / 16.0f, tz = elem.to.z   / 16.0f;

            // Per-element rotation (MC's `rotation` block — chains, rails, etc.).
            // Identity if elem.rotation.axis == 0.
            glm::mat4 rotMat(1.0f);
            glm::mat3 rotNormalMat(1.0f);
            if (elem.rotation.axis != 0) {
                glm::vec3 origin = elem.rotation.origin / 16.0f;
                glm::vec3 axisVec(0.0f);
                switch (elem.rotation.axis) {
                    case 'x': axisVec = glm::vec3(1, 0, 0); break;
                    case 'y': axisVec = glm::vec3(0, 1, 0); break;
                    case 'z': axisVec = glm::vec3(0, 0, 1); break;
                }
                rotMat = glm::translate(rotMat, origin);
                rotMat = glm::rotate(rotMat, glm::radians(elem.rotation.angle), axisVec);
                rotMat = glm::translate(rotMat, -origin);
                rotNormalMat = glm::mat3(glm::rotate(glm::mat4(1.0f),
                                                     glm::radians(elem.rotation.angle), axisVec));
            }
            auto rv = [&](glm::vec3 v) -> glm::vec3 {
                return glm::vec3(rotMat * glm::vec4(v, 1.0f));
            };
            auto isFrontFacingRot = [&](glm::vec3 normal) -> bool {
                return isFrontFacing(rotNormalMat * normal);
            };
            // shade=false elements (chains, leaves) skip directional shading — render
            // at full brightness instead, matching MC's `BakedQuad.shade=false` behaviour.
            const uint32_t SHADE_FLAT = 0xFFFFFFFFu;
            auto pickShade = [&](uint32_t directional) -> uint32_t {
                return elem.shade ? directional : SHADE_FLAT;
            };

            AtlasUVRect tile; glm::vec4 fu; int tint;
            // Per-face vertex order (artist looking AT the face from outside the cube,
            // TL → TR → BR → BL):
            //   UP    (+Y at ty): TL=front-left, TR=front-right, BR=back-right, BL=back-left
            //   DOWN  (-Y at fy): TL=back-left, TR=back-right, BR=front-right, BL=front-left
            //   NORTH (-Z at fz): TL=top-left(-X), TR=top-right(+X), BR=bot-right, BL=bot-left
            //   SOUTH (+Z at tz): TL=top-right(-X), TR=top-left(+X) — mirrored vs north
            //   EAST  (+X at tx): TL=top-front, TR=top-back, BR=bot-back, BL=bot-front
            //   WEST  (-X at fx): TL=top-back, TR=top-front, BR=bot-front, BL=bot-back
            if (isFrontFacingRot({0, 1, 0}) && resolveFace(elem, Game::FaceDir::Up, tile, fu, tint)) {
                // MC FaceBakery: UP face has minZ (north) → vMin (top of texture).
                FaceQuad fq;
                buildFace(fq, rv({fx,ty,fz}), rv({tx,ty,fz}), rv({tx,ty,tz}), rv({fx,ty,tz}),
                          tile, fu, biomeTinted(tint, pickShade(SHADE_UP)));
                quads.push_back(fq);
            }
            if (isFrontFacingRot({0, -1, 0}) && resolveFace(elem, Game::FaceDir::Down, tile, fu, tint)) {
                // MC FaceBakery: DOWN face has maxZ → vMin (inverse of UP).
                FaceQuad fq;
                buildFace(fq, rv({fx,fy,tz}), rv({tx,fy,tz}), rv({tx,fy,fz}), rv({fx,fy,fz}),
                          tile, fu, biomeTinted(tint, pickShade(SHADE_DOWN)));
                quads.push_back(fq);
            }
            if (isFrontFacingRot({0, 0, -1}) && resolveFace(elem, Game::FaceDir::North, tile, fu, tint)) {
                // MC FaceBakery NORTH: maxX → uMin (texture's left).
                FaceQuad fq;
                buildFace(fq, rv({tx,ty,fz}), rv({fx,ty,fz}), rv({fx,fy,fz}), rv({tx,fy,fz}),
                          tile, fu, biomeTinted(tint, pickShade(SHADE_NORTH)));
                quads.push_back(fq);
            }
            if (isFrontFacingRot({0, 0, 1}) && resolveFace(elem, Game::FaceDir::South, tile, fu, tint)) {
                // MC FaceBakery SOUTH: minX → uMin.
                FaceQuad fq;
                buildFace(fq, rv({fx,ty,tz}), rv({tx,ty,tz}), rv({tx,fy,tz}), rv({fx,fy,tz}),
                          tile, fu, biomeTinted(tint, pickShade(SHADE_SOUTH)));
                quads.push_back(fq);
            }
            if (isFrontFacingRot({1, 0, 0}) && resolveFace(elem, Game::FaceDir::East, tile, fu, tint)) {
                FaceQuad fq;
                buildFace(fq, rv({tx,ty,tz}), rv({tx,ty,fz}), rv({tx,fy,fz}), rv({tx,fy,tz}),
                          tile, fu, biomeTinted(tint, pickShade(SHADE_EAST)));
                quads.push_back(fq);
            }
            if (isFrontFacingRot({-1, 0, 0}) && resolveFace(elem, Game::FaceDir::West, tile, fu, tint)) {
                FaceQuad fq;
                buildFace(fq, rv({fx,ty,fz}), rv({fx,ty,tz}), rv({fx,fy,tz}), rv({fx,fy,fz}),
                          tile, fu, biomeTinted(tint, pickShade(SHADE_WEST)));
                quads.push_back(fq);
            }
        }

        // Global back-to-front sort across all elements' faces. Our ortho is
        // glm::ortho(0,w,h,0,-1000,1000) so higher world z = closer to camera;
        // painter's algorithm draws farthest first → sort ASCENDING by depth.
        // stable_sort preserves submission order on ties so an overlay element
        // declared AFTER its base draws ON TOP of the base for coplanar faces
        // (matches MC's "later layers win" convention for grass_block etc.).
        std::stable_sort(quads.begin(), quads.end(),
                         [](const FaceQuad& a, const FaceQuad& b) { return a.depth < b.depth; });

        // Per-vertex iso-projected Z for the depth buffer. Our convention (per the
        // chest renderer comment) is "higher world Z = closer to camera". With our
        // ortho `glm::ortho(0,w,h,0,-1000,1000)` (Z scale -0.001), higher world Z
        // maps to smaller (more negative) NDC Z, which passes the GL_LESS depth
        // test → closer wins. So we just pass the iso-transformed Z straight
        // through; the ortho matrix in the shader does the rest.
        auto isoZ = [&](glm::vec3 v) -> float {
            glm::vec4 t = isoMat * glm::vec4(v, 1.0f);
            return t.z;
        };

        for (const auto& fq : quads) {
            glm::vec2 p0 = proj(fq.v[0].x, fq.v[0].y, fq.v[0].z);
            glm::vec2 p1 = proj(fq.v[1].x, fq.v[1].y, fq.v[1].z);
            glm::vec2 p2 = proj(fq.v[2].x, fq.v[2].y, fq.v[2].z);
            glm::vec2 p3 = proj(fq.v[3].x, fq.v[3].y, fq.v[3].z);
            QuadCommand q;
            q.texture = tex;
            q.color = fq.color;
            q.useDepth = true;
            q.px[0] = p0.x; q.py[0] = p0.y; q.pz[0] = isoZ(fq.v[0]); q.u[0] = fq.u[0]; q.v[0] = fq.v_[0];
            q.px[1] = p1.x; q.py[1] = p1.y; q.pz[1] = isoZ(fq.v[1]); q.u[1] = fq.u[1]; q.v[1] = fq.v_[1];
            q.px[2] = p2.x; q.py[2] = p2.y; q.pz[2] = isoZ(fq.v[2]); q.u[2] = fq.u[2]; q.v[2] = fq.v_[2];
            q.px[3] = p3.x; q.py[3] = p3.y; q.pz[3] = isoZ(fq.v[3]); q.u[3] = fq.u[3]; q.v[3] = fq.v_[3];
            renderState->SubmitQuad(q);
        }
    }

    // ─── Flat 2D item textures (compass, sword, etc.) ─────────────────────
    // Process-global cache so the texture survives across GuiGraphics instances (which are
    // recreated every frame) and across Open/Close cycles. Loaded once on first use.
    namespace {
        std::unordered_map<std::string, TextureHandle>& ItemTextureCache() {
            static std::unordered_map<std::string, TextureHandle> cache;
            return cache;
        }

        TextureHandle LoadItemTexture(const std::string& itemName) {
            if (!g_renderBackend) return INVALID_TEXTURE;
            auto& cache = ItemTextureCache();
            auto it = cache.find(itemName);
            if (it != cache.end()) return it->second;

            // Item models reference textures by category (e.g. layer0 = "item/oak_door"
            // vs. "block/torch"). The loader strips the prefix, so we don't know which
            // dir the file lives in — try item/ first (most common), then block/.
            std::string full = PlatformMain::GetAssetPath("assets/textures/item/" + itemName + ".png");
            if (!std::filesystem::exists(full)) {
                std::string blockPath = PlatformMain::GetAssetPath("assets/textures/block/" + itemName + ".png");
                if (std::filesystem::exists(blockPath)) {
                    full = std::move(blockPath);
                } else {
                    Log::Warning("[GuiGraphics] Item texture not found: %s (and not in textures/block/)", full.c_str());
                    cache[itemName] = INVALID_TEXTURE; // negative-cache so we don't re-stat every frame
                    return INVALID_TEXTURE;
                }
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0); // GUI ortho is top-down — no flip
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[GuiGraphics] stbi_load failed for %s: %s",
                             full.c_str(), stbi_failure_reason());
                cache[itemName] = INVALID_TEXTURE;
                return INVALID_TEXTURE;
            }
            TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            cache[itemName] = tex;
            return tex;
        }
    } // namespace

    void GuiGraphics::PreloadItem(Game::ItemID itemId) {
        const auto& item = Game::ItemRegistry::Get(itemId);
        if (item.renderType == Game::ItemRenderType::Sprite) {
            // Static sprite (single texture).
            if (!item.spriteName.empty()) LoadItemTexture(item.spriteName);
            // Animated frames — preload all so any selected frame is ready immediately.
            // (MC's compass has 32 frames; loading all on first inventory open avoids the
            // first-frame texture-creation race that hides the icon until the next frame.)
            for (const auto& frame : item.spriteFrames) {
                if (!frame.empty()) LoadItemTexture(frame);
            }
        }
        // Block items don't need preloading — they render from the block atlas which is
        // already initialized at startup.
    }

    // ─── Custom item renderers (MC's BlockEntityWithoutLevelRenderer equivalent) ────────
    // Process-global table mapping ItemID → custom-renderer function. Looked up first by
    // the RenderItem dispatcher; if a match is found, the standard block/sprite path is
    // bypassed entirely. Registered from client startup via RegisterCustomItemRenderer().
    namespace {
        std::unordered_map<Game::ItemID, GuiGraphics::CustomItemRenderer>& CustomRenderers() {
            static std::unordered_map<Game::ItemID, GuiGraphics::CustomItemRenderer> m;
            return m;
        }
    }

    void GuiGraphics::RegisterCustomItemRenderer(Game::ItemID id, CustomItemRenderer renderer) {
        CustomRenderers()[id] = renderer;
    }

    // MC-style RenderItem dispatcher: custom renderers first, then renderType.
    //   1. Custom renderer (BEWLR — chest, sign, banner, etc.)
    //   2. ItemRenderType::Block  → 3D isometric (RenderBlockItemImpl)
    //   3. ItemRenderType::Sprite → flat 16×16 blit
    // Render a flat sprite icon at (x, y, size) with depth-write enabled. Used
    // when the stack is foil — opaque pixels write Z=0 (transparent corners are
    // discarded in the fragment shader via uAlphaTest, so they don't write
    // depth), and then RenderItemGlintOverlay runs with depth-test EQUAL at
    // Z=0 so the glint masks itself to the icon's silhouette. Mirrors MC's
    // `RenderPipelines.GLINT.withDepthTestFunction(EQUAL_DEPTH_TEST)` trick
    // (RenderPipelines.java:197).
    static void BlitSpriteWithDepth(GuiRenderState* rs, TextureHandle tex,
                                    int x, int y, int size) {
        if (!rs) return;
        QuadCommand q;
        q.texture    = tex;
        q.color      = 0xFFFFFFFFu;
        q.useDepth   = true;
        q.depthFunc  = CompareOp::LessEqual;
        q.depthWrite = true;
        q.blendMode  = QuadBlendMode::AlphaBlend;
        q.px[0] = (float)x;        q.py[0] = (float)y;        q.pz[0] = 0.0f;
        q.px[1] = (float)(x+size); q.py[1] = (float)y;        q.pz[1] = 0.0f;
        q.px[2] = (float)(x+size); q.py[2] = (float)(y+size); q.pz[2] = 0.0f;
        q.px[3] = (float)x;        q.py[3] = (float)(y+size); q.pz[3] = 0.0f;
        q.u[0] = 0.0f; q.v[0] = 0.0f;
        q.u[1] = 1.0f; q.v[1] = 0.0f;
        q.u[2] = 1.0f; q.v[2] = 1.0f;
        q.u[3] = 0.0f; q.v[3] = 1.0f;
        rs->SubmitQuad(q);
    }

    namespace {
        // Lazy-loaded glint texture, mirrors MC `ItemRenderer.ENCHANTED_GLINT_ITEM`
        // ("textures/misc/enchanted_glint_item.png"). Wrap mode REPEAT so the
        // scrolling UVs tile seamlessly when they exceed [0,1].
        TextureHandle LoadGlintTexture() {
            static TextureHandle s_tex = INVALID_TEXTURE;
            static bool          s_tried = false;
            if (s_tried) return s_tex;
            s_tried = true;
            if (!g_renderBackend) return INVALID_TEXTURE;
            const std::string full = PlatformMain::GetAssetPath("assets/textures/misc/enchanted_glint_item.png");
            if (!std::filesystem::exists(full)) {
                Log::Warning("[GuiGraphics] glint texture missing at %s", full.c_str());
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[GuiGraphics] stbi_load glint failed: %s", stbi_failure_reason());
                return INVALID_TEXTURE;
            }
            s_tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (s_tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(s_tex, TextureFilter::Linear, TextureFilter::Linear);
                g_renderBackend->SetTextureWrap  (s_tex, TextureWrap::Repeat,  TextureWrap::Repeat);
            }
            return s_tex;
        }
    } // namespace

    void GuiGraphics::RenderItemGlintOverlay(int x, int y, int size) {
        // (Mirrors MC RenderType.glint; see header docs.)
        TextureHandle tex = LoadGlintTexture();
        if (tex == INVALID_TEXTURE) return;
        if (!m_renderState) return;

        // Time-driven UV transform — verbatim from MC's
        // TextureTransform.setupGlintTexturing (TextureTransform.java:30-37):
        //   millis    = current_ms * glintSpeed * MAX_ENCHANTMENT_GLINT_SPEED_MILLIS
        //   layer0    = (millis % 110000) / 110000   ← X scroll cycle 110s
        //   layer1    = (millis %  30000) /  30000   ← Y scroll cycle 30s
        //   uvMatrix  = translate(-layer0, layer1) * rotateZ(0.17453292) * scale(8.0)
        // 0.17453292 rad = π/18 = 10°. Scale 8.0 is the GLINT_TEXTURING constant
        // (TextureTransform.java:13). glintSpeed is a user setting; we use the
        // MC default (1.0) until we expose it as an option.
        constexpr double MAX_GLINT_SPEED = 8.0;          // TextureTransform.java:9
        constexpr double GLINT_SPEED     = 0.5;          // MC default (Options.java:1080)
        constexpr float  GLINT_ROT_RAD   = 0.17453292f;  // π/18 = 10°
        // GLINT_SCALE — careful derivation from MC's setup.
        //
        // MC `GLINT_TEXTURING` uses scale 8.0 (TextureTransform.java:13). But
        // its scale=8 is applied to ITEM ATLAS UVs — at a typical 1024-px
        // atlas, each 16-px sprite spans only 16/1024 = 0.015625 in UV. So
        // the effective sampled-texture span per icon is
        //   8 × 0.015625 = 0.125
        // = 0.125 × 128-px glint texture = 16 texture pixels per 16-px icon
        // = ONE-TO-ONE native pixel mapping (the icon shows the lattice at
        //   its full crisp resolution, with each scrolling tick visibly
        //   changing the pattern in front of you).
        //
        // Our items are STANDALONE textures with full UV (0..1). To hit the
        // same 1:1 mapping we want `scale × 1.0 = 0.125`:
        //   GLINT_SCALE = 16 / 128 = 0.125
        //
        // (Earlier we tried 0.5, which sampled 64 texture pixels into a 16-px
        // icon — 4× minification + linear filter blurred all the bright/dark
        // contrast into a uniform purple mush, so the natural intensity
        // pulsing as bright streaks scrolled through went invisible.)
        constexpr float  GLINT_SCALE     = 0.125f;

        const long long now_ms   = static_cast<long long>(glfwGetTime() * 1000.0);
        const long long millis   = static_cast<long long>(static_cast<double>(now_ms) * GLINT_SPEED * MAX_GLINT_SPEED);
        const float layer0       = static_cast<float>(millis % 110000LL) / 110000.0f;
        const float layer1       = static_cast<float>(millis %  30000LL) /  30000.0f;
        const float c            = std::cos(GLINT_ROT_RAD);
        const float s            = std::sin(GLINT_ROT_RAD);

        // ── SINGLE-PASS GLINT (RenderTypes.java:392) ───────────────────────
        // Modern MC renders item glint as ONE draw with the GLINT render
        // type. The "intensity goes in and out" pulsing the user sees is a
        // natural byproduct of the texture's bright streaks scrolling across
        // the small sample window — there are moments when only the dimmer
        // background lattice is in view (= relaxed glint) and moments when
        // a bright streak is centered on the icon (= peak glint). The two
        // layer offsets (cycling at 110000ms / 30000ms in different axes)
        // ensure the crossings never settle into a static pattern.
        //
        // We previously tried a two-pass version — that filled in the dim
        // moments because pass B always had a streak in view when pass A
        // didn't, killing the relax phase the user wants.
        auto transformUV = [&](float u, float v, float& outU, float& outV) {
            const float su = u * GLINT_SCALE;
            const float sv = v * GLINT_SCALE;
            const float ru = su * c - sv * s;
            const float rv = su * s + sv * c;
            outU = ru - layer0;
            outV = rv + layer1;
        };

        QuadCommand q;
        q.texture    = tex;
        // `core/glint.fsh` outputs `color.rgb * fade` where
        // `fade = (1 - fog) * GlintAlpha` and `GlintAlpha` comes from the
        // `glintStrength` option (Options.java:1082, default 0.75). Our
        // additive (SrcColor, One) blend yields `(color * 0.75)² + dst`.
        // Bake the 0.75 dampener into vColor so the textured shader's
        // `texColor * vColor` produces MC's exact contribution.
        // 0.75 * 255 = 191 = 0xBF.
        q.color      = 0xBFBFBFBFu;
        // Depth-equal mask trick (RenderPipelines.GLINT, RenderPipelines.java:197).
        q.useDepth   = true;
        q.depthFunc  = CompareOp::Equal;
        q.depthWrite = false;
        q.blendMode  = QuadBlendMode::Additive;
        q.px[0] = static_cast<float>(x);          q.py[0] = static_cast<float>(y);          q.pz[0] = 0.0f;
        q.px[1] = static_cast<float>(x + size);   q.py[1] = static_cast<float>(y);          q.pz[1] = 0.0f;
        q.px[2] = static_cast<float>(x + size);   q.py[2] = static_cast<float>(y + size);   q.pz[2] = 0.0f;
        q.px[3] = static_cast<float>(x);          q.py[3] = static_cast<float>(y + size);   q.pz[3] = 0.0f;
        transformUV(0.0f, 0.0f, q.u[0], q.v[0]);
        transformUV(1.0f, 0.0f, q.u[1], q.v[1]);
        transformUV(1.0f, 1.0f, q.u[2], q.v[2]);
        transformUV(0.0f, 1.0f, q.u[3], q.v[3]);
        m_renderState->SubmitQuad(q);
    }

    void GuiGraphics::RenderItem(const Game::ItemStack& stack, int x, int y) {
        if (stack.IsEmpty()) return;
        // 1. Custom renderer takes precedence (matches MC's BEWLR dispatch).
        auto& crs = CustomRenderers();
        auto crIt = crs.find(stack.itemId);
        if (crIt != crs.end() && crIt->second) {
            crIt->second(*this, stack, x, y);
            // TODO(glint): the BEWLR path (chest/banner/head/bed/shulker) doesn't
            // write Z=0; it writes per-vertex iso depths. The glint pass uses
            // depth-test EQUAL at Z=0, so masked-glint won't match here. No
            // current vanilla item routed through BEWLR is enchantable, so this
            // is dormant — when one appears (e.g. enchanted shulker box), the
            // BEWLR renderers will need to also write depth at a known Z (or
            // the glint pass will need to read the per-vertex Z those renderers
            // emit and use that for the masked pass).
            return;
        }
        const auto& item = Game::ItemRegistry::Get(stack.itemId);
        switch (item.renderType) {
            case Game::ItemRenderType::Block:
                RenderBlockItemImpl(*this, m_renderState, item.blockId, item.blockModelOverride, x, y);
                // TODO(glint): block icons write per-vertex iso Z values, not Z=0,
                // so the depth-test EQUAL mask the glint pass uses won't match.
                // No vanilla block item is enchantable today; revisit when one is.
                return;
            case Game::ItemRenderType::Sprite: {
                // MC-style frame resolution: if the item has a frame selector (e.g. compass),
                // ask it which frame to draw based on the current per-frame render context
                // (player position + yaw + target). Otherwise fall back to the static sprite.
                std::string spriteName;
                bool useFrameSelector = false;
                if (item.selectFrame && !item.spriteFrames.empty()) {
                    int frame = item.selectFrame(Game::ItemRegistry::GetRenderContext());
                    if (frame < 0) frame = 0;
                    if (frame >= (int)item.spriteFrames.size()) frame = (int)item.spriteFrames.size() - 1;
                    spriteName = item.spriteFrames[frame];
                    useFrameSelector = true;
                } else {
                    spriteName = item.spriteName;
                }

                // ── Multi-layer / tinted rendering (leather armor + dye,
                //    leather horse armor, spawn eggs, potions, fireworks, …).
                //    MC stacks layerN textures back to front; each layer
                //    carries a tint color from the items JSON's `tints` array
                //    (index N → layerN). 0 = untinted.
                //
                //    Take this path whenever there's >1 layer OR any layer
                //    needs a tint — so single-layer dyed items (leather horse
                //    armor — model only declares layer0 even though the items
                //    JSON has a dye tint) still get tinted.
                bool hasAnyTint = false;
                for (uint32_t t : item.layerTints) { if (t != 0) { hasAnyTint = true; break; } }
                if (!useFrameSelector && !item.spriteLayers.empty()
                    && (item.spriteLayers.size() > 1 || hasAnyTint)) {
                    bool foil = stack.HasFoil();
                    for (size_t i = 0; i < item.spriteLayers.size(); ++i) {
                        const std::string& layerName = item.spriteLayers[i];
                        if (layerName.empty()) continue;
                        TextureHandle ltex = LoadItemTexture(layerName);
                        if (ltex == INVALID_TEXTURE) continue;
                        // Tint resolution: explicit tint > untinted (white).
                        // Stored value is ARGB; alpha bit must be set for the
                        // textured shader's `texColor * vColor` to keep the
                        // sprite opaque (raw 0xFF.... from MC's Java int
                        // already has alpha=0xFF — see ClientItemLoader).
                        uint32_t tint = 0xFFFFFFFFu;
                        if (i < item.layerTints.size() && item.layerTints[i] != 0) {
                            tint = item.layerTints[i];
                            // Force alpha=0xFF in case the JSON value lacked
                            // the high byte (MC's "minecraft:dye" tint type
                            // operates on RGB only — alpha comes from the
                            // texture, not the tint).
                            tint |= 0xFF000000u;
                        }
                        if (foil && i == 0) {
                            // Foil mask trick uses the FIRST layer's silhouette
                            // as the depth mask. Subsequent layers stack on top
                            // with normal alpha-blend — they don't need to
                            // re-write depth.
                            BlitSpriteWithDepth(m_renderState, ltex, x, y, 16);
                        } else {
                            Blit(ltex, x, y, x + 16, y + 16, 0.0f, 0.0f, 1.0f, 1.0f, tint);
                        }
                    }
                    if (foil) RenderItemGlintOverlay(x, y, 16);
                    return;
                }

                if (spriteName.empty()) {
                    static bool warnedEmpty = false;
                    if (!warnedEmpty) {
                        Log::Warning("[GuiGraphics] RenderItem(item=%u): empty spriteName (frames=%zu, selector=%p)",
                                     (unsigned)stack.itemId, item.spriteFrames.size(),
                                     (void*)item.selectFrame);
                        warnedEmpty = true;
                    }
                    return;
                }
                TextureHandle tex = LoadItemTexture(spriteName);
                if (tex == INVALID_TEXTURE) {
                    static std::unordered_set<std::string> warnedMissing;
                    if (warnedMissing.insert(spriteName).second) {
                        Log::Warning("[GuiGraphics] RenderItem(item=%u): missing texture for sprite '%s'",
                                     (unsigned)stack.itemId, spriteName.c_str());
                    }
                    return;
                }
                if (stack.HasFoil()) {
                    // Foil path: write Z=0 on opaque pixels (alpha-discard
                    // gates the depth write). The glint pass that follows
                    // uses depth-test EQUAL to limit itself to the icon's
                    // silhouette — see RenderItemGlintOverlay above.
                    BlitSpriteWithDepth(m_renderState, tex, x, y, 16);
                    RenderItemGlintOverlay(x, y, 16);
                } else {
                    Blit(tex, x, y, x + 16, y + 16, 0.0f, 0.0f, 1.0f, 1.0f);
                }
                return;
            }
        }
    }

    void GuiGraphics::RenderItemDecorations(const Game::ItemStack& slot, int x, int y) {
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
