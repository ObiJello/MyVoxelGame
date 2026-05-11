// File: src/client/renderer/gui/items/BannerItemRenderer.cpp
//
// Banner item renderer — equivalent to MC's BannerSpecialRenderer. Draws the
// 3D standing-banner model (pole + bar + flag) using `banner_base.png` and
// tints the flag with the dye color.
//
// MC sources mirrored:
//   • client/renderer/special/BannerSpecialRenderer.java
//   • client/renderer/blockentity/BannerRenderer.java::submitSpecial
//   • client/model/object/banner/BannerModel.java::createBodyLayer(true)
//   • client/model/object/banner/BannerFlagModel.java::createFlagLayer(true)
//
// Geometry (PIXEL space):
//   pole: addBox(-1,-42,-1, 2,42,2)   texOffs(44, 0)   PartPose.ZERO
//   bar:  addBox(-10,-44,-1, 20,2,2)  texOffs(0, 42)   PartPose.ZERO
//   flag: addBox(-10, 0,-2, 20,40,1)  texOffs(0, 0)    PartPose.offset(0, -44, 0)
//
// Pre-render pose (BannerRenderer.submitBanner with angle=0 for inventory):
//   translate(0.5, 0, 0.5)
//   mulPose(YP rotation 0)  ← identity
//   scale(0.6666667, -0.6666667, -0.6666667)
//
// Display.gui (template_banner.json): rotation [30, 20, 0], translation [0,-3.25,0],
// scale 0.5325. ItemTransform.apply contributes a trailing translate(-0.5,-0.5,-0.5).
//
// For inventory v1: render pole+bar without tint, render flag tinted with the
// dye color. Pattern layers (creeper face, brick pattern, etc.) NOT supported
// since BannerPatternLayers data isn't on inventory ItemStacks anyway.

#include "BannerItemRenderer.hpp"
#include "ItemLighting.hpp"
#include "../GuiGraphics.hpp"
#include "../GuiRenderState.hpp"
#include "../../backend/RenderBackend.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <array>
#include <vector>
#include <algorithm>
#include <unordered_map>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {
        // banner_base.png — single shared texture (64×64), used for pole/bar/flag.
        TextureHandle g_bannerBaseTexture = INVALID_TEXTURE;

        TextureHandle LoadBannerBase() {
            if (g_bannerBaseTexture != INVALID_TEXTURE) return g_bannerBaseTexture;
            if (!g_renderBackend) return INVALID_TEXTURE;
            const std::string full = PlatformMain::GetAssetPath("assets/textures/entity/banner_base.png");
            if (!std::filesystem::exists(full)) {
                Log::Warning("[BannerItemRenderer] banner_base.png not found at %s", full.c_str());
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[BannerItemRenderer] stbi_load failed: %s", stbi_failure_reason());
                return INVALID_TEXTURE;
            }
            g_bannerBaseTexture = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (g_bannerBaseTexture != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(g_bannerBaseTexture, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap (g_bannerBaseTexture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return g_bannerBaseTexture;
        }

        // DyeColor.getTextureDiffuseColor() values from MC's DyeColor.java enum.
        // Used to tint the flag — multiplied with banner_base.png's white flag region.
        uint32_t DyeColorRGB(const std::string& color) {
            // 0xFF<RGB>  (alpha=255 + RGB)
            static const std::unordered_map<std::string, uint32_t> table = {
                {"white",       0xFFF9FFFE},
                {"orange",      0xFFF9801D},
                {"magenta",     0xFFC74EBD},
                {"light_blue",  0xFF3AB3DA},
                {"yellow",      0xFFFED83D},
                {"lime",        0xFF80C71F},
                {"pink",        0xFFF38BAA},
                {"gray",        0xFF474F52},
                {"light_gray",  0xFF9D9D97},
                {"cyan",        0xFF169C9C},
                {"purple",      0xFF8932B8},
                {"blue",        0xFF3C44AA},
                {"brown",       0xFF835432},
                {"green",       0xFF5E7C16},
                {"red",         0xFFB02E26},
                {"black",       0xFF1D1D21},
            };
            auto it = table.find(color);
            return it != table.end() ? it->second : 0xFFFFFFFF;
        }

        // Outer iso = ItemTransform.apply with template_banner.json's display.gui:
        //   rotation [30, 20, 0], translation [0, -3.25, 0], scale 0.5325
        // Translation is in pixel units → /16.
        const glm::mat4& OuterIsoMatrix() {
            static glm::mat4 m = []{
                glm::mat4 mat(1.0f);
                mat = glm::translate(mat, glm::vec3(0.0f, -3.25f/16.0f, 0.0f));         // T_disp
                mat = glm::rotate(mat, glm::radians(30.0f), glm::vec3(1, 0, 0));         // Rx
                mat = glm::rotate(mat, glm::radians(20.0f), glm::vec3(0, 1, 0));         // Ry
                mat = glm::scale(mat, glm::vec3(0.5325f));                                // S
                mat = glm::translate(mat, glm::vec3(-0.5f, -0.5f, -0.5f));                // T(-0.5)
                return mat;
            }();
            return m;
        }

        constexpr float BANNER_GUI_ROT_X = 30.0f;
        constexpr float BANNER_GUI_ROT_Y = 20.0f;

        glm::vec2 Project(const glm::mat4& iso, glm::vec3 p, float cx, float cy, float scale) {
            glm::vec4 q = iso * glm::vec4(p, 1.0f);
            return glm::vec2(cx + q.x * scale, cy + (-q.y) * scale);
        }

        float TransformedZ(const glm::mat4& iso, glm::vec3 p) {
            glm::vec4 q = iso * glm::vec4(p, 1.0f);
            return q.z;
        }

        struct CubeFace {
            std::array<glm::vec3, 4> v;
            float u_min, v_min, u_max, v_max;
            uint32_t color;
            float depth;
        };

        // Multiply two ARGB colors (component-wise). Used to combine per-face shading
        // with the dye color tint for the flag.
        uint32_t MulARGB(uint32_t a, uint32_t b) {
            uint32_t aa = (((a >> 24) & 0xFF) * ((b >> 24) & 0xFF)) / 255u;
            uint32_t ar = (((a >> 16) & 0xFF) * ((b >> 16) & 0xFF)) / 255u;
            uint32_t ag = (((a >>  8) & 0xFF) * ((b >>  8) & 0xFF)) / 255u;
            uint32_t ab = (((a      ) & 0xFF) * ((b      ) & 0xFF)) / 255u;
            return (aa << 24) | (ar << 16) | (ag << 8) | ab;
        }

        // Build all 6 faces of one cube using MC's exact polygon layout.
        // tintColor (default white) is multiplied into the per-face shading.
        void BuildCubeFaces(std::vector<CubeFace>& out,
                            const glm::mat4& poseMatrix,
                            glm::vec3 from, glm::vec3 to,
                            float xTexOffs, float yTexOffs, float w, float h, float d,
                            uint32_t tintColor)
        {
            float minX=from.x, minY=from.y, minZ=from.z;
            float maxX=to.x,   maxY=to.y,   maxZ=to.z;
            glm::vec3 t0(minX, minY, minZ), t1(maxX, minY, minZ);
            glm::vec3 t2(maxX, maxY, minZ), t3(minX, maxY, minZ);
            glm::vec3 l0(minX, minY, maxZ), l1(maxX, minY, maxZ);
            glm::vec3 l2(maxX, maxY, maxZ), l3(minX, maxY, maxZ);
            auto P = [&](glm::vec3 v) { return glm::vec3(poseMatrix * glm::vec4(v, 1.0f)); };
            t0=P(t0); t1=P(t1); t2=P(t2); t3=P(t3);
            l0=P(l0); l1=P(l1); l2=P(l2); l3=P(l3);

            float u  = xTexOffs;
            float v  = yTexOffs;
            float u0 = u;
            float u1 = u + d;
            float u2 = u + d + w;
            float u22= u + d + w + w;
            float u3 = u + d + w + d;
            float u4 = u + d + w + d + w;
            float v0 = v;
            float v1 = v + d;
            float v2 = v + d + h;

            // template_banner.json declares `gui_light: "front"` → MC uses ITEMS_FLAT
            // lights for banners (Lighting.java line 31). The flat pose lacks the
            // scaling(1,-1,1) and YXZ rotation that ITEMS_3D adds, so vertical
            // surfaces (the flag) get full front-on illumination instead of the
            // dim side-lit shading.
            using ItemLighting::ComputeShadeFlat;
            using ItemLighting::ShadeAsColor;
            const glm::mat3 normalMat(poseMatrix);
            auto shadeFor = [&](glm::vec3 cubeLocalNormal) -> uint32_t {
                glm::vec3 worldNormal = normalMat * cubeLocalNormal;
                uint32_t shade = ShadeAsColor(ComputeShadeFlat(worldNormal, BANNER_GUI_ROT_X, BANNER_GUI_ROT_Y));
                return MulARGB(shade, tintColor);
            };
            const uint32_t SHADE_DOWN  = shadeFor({ 0,-1, 0});
            const uint32_t SHADE_UP    = shadeFor({ 0, 1, 0});
            const uint32_t SHADE_WEST  = shadeFor({-1, 0, 0});
            const uint32_t SHADE_EAST  = shadeFor({ 1, 0, 0});
            const uint32_t SHADE_NORTH = shadeFor({ 0, 0,-1});
            const uint32_t SHADE_SOUTH = shadeFor({ 0, 0, 1});

            auto pushFace = [&](std::array<glm::vec3,4> verts, float um, float vm, float uM, float vM, uint32_t col) {
                CubeFace f;
                f.v = verts;
                f.u_min = um; f.v_min = vm; f.u_max = uM; f.v_max = vM;
                f.color = col;
                float sum = 0.0f;
                const auto& iso = OuterIsoMatrix();
                for (int i = 0; i < 4; ++i) sum += TransformedZ(iso, verts[i]);
                f.depth = sum * 0.25f;
                out.push_back(f);
            };
            pushFace({l1, l0, t0, t1}, u1, v0, u2,  v1, SHADE_DOWN);
            pushFace({t2, t3, l3, l2}, u2, v1, u22, v0, SHADE_UP);
            pushFace({t0, l0, l3, t3}, u0, v1, u1,  v2, SHADE_WEST);
            pushFace({t1, t0, t3, t2}, u1, v1, u2,  v2, SHADE_NORTH);
            pushFace({l1, t1, t2, l2}, u2, v1, u3,  v2, SHADE_EAST);
            pushFace({l0, l1, l2, l3}, u3, v1, u4,  v2, SHADE_SOUTH);
        }

        void SubmitFace(GuiRenderState* rs, TextureHandle tex,
                        float cx, float cy, float scale, const CubeFace& f)
        {
            const float TEX = 64.0f;
            QuadCommand q;
            q.texture = tex;
            q.color = f.color;
            const auto& iso = OuterIsoMatrix();
            for (int i = 0; i < 4; ++i) {
                glm::vec2 p = Project(iso, f.v[i], cx, cy, scale);
                q.px[i] = p.x;
                q.py[i] = p.y;
            }
            q.u[0] = f.u_min/TEX; q.v[0] = f.v_min/TEX;
            q.u[1] = f.u_max/TEX; q.v[1] = f.v_min/TEX;
            q.u[2] = f.u_max/TEX; q.v[2] = f.v_max/TEX;
            q.u[3] = f.u_min/TEX; q.v[3] = f.v_max/TEX;
            rs->SubmitQuad(q);
        }

        void RenderBannerInventory(GuiGraphics& g, const Game::ItemStack& stack, int x, int y) {
            const auto& item = Game::ItemRegistry::Get(stack.itemId);
            const std::string color = item.specialTexture.empty() ? "white" : item.specialTexture;
            TextureHandle tex = LoadBannerBase();
            if (tex == INVALID_TEXTURE) return;
            GuiRenderState* rs = g.GetRenderState();
            if (!rs) return;

            // Pre-render pose (BannerRenderer.submitBanner): T(0.5, 0, 0.5) * S(0.6667, -0.6667, -0.6667)
            const glm::mat4 prePose = []{
                glm::mat4 m(1.0f);
                m = glm::translate(m, glm::vec3(0.5f, 0.0f, 0.5f));
                m = glm::scale(m, glm::vec3(2.0f/3.0f, -2.0f/3.0f, -2.0f/3.0f));
                return m;
            }();
            // Flag has an additional PartPose.offset(0, -44, 0) — translate by -44/16 = -2.75 in Y.
            const glm::mat4 flagPose = prePose * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -44.0f/16.0f, 0.0f));

            const float scale = 16.0f;
            const float cx = static_cast<float>(x) + 8.0f;
            const float cy = static_cast<float>(y) + 8.0f;

            // Render layers IN DECLARED ORDER, sorting only within each layer's
            // own faces. MC does this by submitting BannerModel (pole+bar) first,
            // then BannerFlagModel (flag) second — so the flag overdraws the bar
            // at their overlap (top of flag = bottom of bar).
            //
            // Why per-layer sort instead of one global sort: my depth metric is
            // each face's average post-iso z. The bar is a thin strip with all-high
            // Y coords, so its average z (after Rx(30°) folds Y into Z) is higher
            // than the flag's average z (the flag spans low → high Y). A global
            // sort would put bar AFTER flag, drawing bar in front. By splitting
            // the sort per cube, we keep the right within-cube depth order AND
            // preserve cube-declaration order across cubes — exactly what MC does.
            auto renderCube = [&](const glm::mat4& pose, glm::vec3 from, glm::vec3 to,
                                  float xOff, float yOff, float w, float h, float d,
                                  uint32_t tint) {
                std::vector<CubeFace> faces;
                faces.reserve(6);
                BuildCubeFaces(faces, pose, from, to, xOff, yOff, w, h, d, tint);
                std::stable_sort(faces.begin(), faces.end(),
                                 [](const CubeFace& a, const CubeFace& b) { return a.depth < b.depth; });
                for (const auto& f : faces) SubmitFace(rs, tex, cx, cy, scale, f);
            };

            // Pole (BannerModel, declared first).
            renderCube(prePose,
                       glm::vec3(-1.0f/16, -42.0f/16, -1.0f/16),
                       glm::vec3( 1.0f/16,        0,  1.0f/16),
                       44, 0, 2, 42, 2, 0xFFFFFFFF);
            // Bar (BannerModel, declared second — drawn after pole).
            renderCube(prePose,
                       glm::vec3(-10.0f/16, -44.0f/16, -1.0f/16),
                       glm::vec3( 10.0f/16, -42.0f/16,  1.0f/16),
                       0, 42, 20, 2, 2, 0xFFFFFFFF);
            // Flag (BannerFlagModel, submitted SECOND in MC's submitBanner — drawn
            // after bar, so its top edge correctly overdraws the bar's front).
            renderCube(flagPose,
                       glm::vec3(-10.0f/16, 0,         -2.0f/16),
                       glm::vec3( 10.0f/16, 40.0f/16,  -1.0f/16),
                       0, 0, 20, 40, 1, DyeColorRGB(color));
        }
    } // namespace

    void RegisterBannerItemRenderer() {
        const int total = static_cast<int>(Game::BlockID::Count);
        for (int i = 1; i < total; ++i) {
            const auto& it = Game::ItemRegistry::Get(static_cast<Game::ItemID>(i));
            if (it.specialKind == "banner") {
                GuiGraphics::RegisterCustomItemRenderer(static_cast<Game::ItemID>(i), &RenderBannerInventory);
                continue;
            }
            // Fallback for banners without an items/X.json (e.g. ominous_banner —
            // MC normally generates its JSON with the illager pattern baked in,
            // but vanilla doesn't always ship it). Match standing banners by
            // modelName, skip wall_banners (auto-placed only).
            const auto& block = Game::BlockRegistry::Get(static_cast<Game::BlockID>(i));
            if (block.modelName.find("banner") != std::string::npos
                && block.modelName.find("wall_") == std::string::npos) {
                GuiGraphics::RegisterCustomItemRenderer(static_cast<Game::ItemID>(i), &RenderBannerInventory);
            }
        }
    }

} // namespace Render
