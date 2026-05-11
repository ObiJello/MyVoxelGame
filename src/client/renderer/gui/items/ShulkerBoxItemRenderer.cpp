// File: src/client/renderer/gui/items/ShulkerBoxItemRenderer.cpp
//
// Shulker box item renderer — equivalent to MC's ShulkerBoxSpecialRenderer.
// Renders the 3D 2-cube model (base + lid, closed state) using a per-color
// 64×64 entity texture from assets/textures/entity/shulker/{texture}.png.
//
// MC sources mirrored:
//   • client/renderer/special/ShulkerBoxSpecialRenderer.java
//   • client/renderer/blockentity/ShulkerBoxRenderer.java          (.prepareModel + .submit)
//   • client/model/monster/shulker/ShulkerModel.java::createShellMesh
//
// Geometry (PIXEL space, 1 unit = 1 pixel = 1/16 block):
//   lid:  addBox(-8,-16,-8, 16,12,16)  texOffs(0, 0)   PartPose.offset(0, 24, 0)
//   base: addBox(-8, -8,-8, 16, 8,16)  texOffs(0, 28)  PartPose.offset(0, 24, 0)
//
// prepareModel for inventory (Direction.UP, progress=0.0 → closed):
//   translate(0.5, 0.5, 0.5)
//   scale(0.9995)
//   mulPose(Direction.UP.getRotation())  ← identity for UP
//   scale(1, -1, -1)                      ← Y and Z inverted
//   translate(0, -1, 0)
//
// setupAnim(progress=0): lid.setPos(0, 24, 0) [same as PartPose.offset], lid.yRot=0.
// So both lid and base get a translateAndRotate of T(0, 24/16, 0) = T(0, 1.5, 0).
//
// Display.gui (template_shulker_box.json): rotation [30, 45, 0], translation [0, 0, 0],
// scale 0.625 — same as the chest. ItemTransform.apply contributes the trailing
// translate(-0.5, -0.5, -0.5).

#include "ShulkerBoxItemRenderer.hpp"
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
        std::unordered_map<std::string, TextureHandle>& ShulkerTextureCache() {
            static std::unordered_map<std::string, TextureHandle> m;
            return m;
        }

        TextureHandle LoadShulkerTexture(const std::string& texName) {
            auto& cache = ShulkerTextureCache();
            auto it = cache.find(texName);
            if (it != cache.end()) return it->second;
            if (!g_renderBackend) return INVALID_TEXTURE;
            const std::string rel  = "assets/textures/entity/shulker/" + texName + ".png";
            const std::string full = PlatformMain::GetAssetPath(rel);
            if (!std::filesystem::exists(full)) {
                Log::Warning("[ShulkerBoxItemRenderer] %s not found at %s", rel.c_str(), full.c_str());
                cache[texName] = INVALID_TEXTURE;
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[ShulkerBoxItemRenderer] stbi_load failed for %s: %s", full.c_str(), stbi_failure_reason());
                cache[texName] = INVALID_TEXTURE;
                return INVALID_TEXTURE;
            }
            TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap (tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            cache[texName] = tex;
            return tex;
        }

        // Outer iso = ItemTransform.apply with template_shulker_box.json's display.gui:
        //   rotation [30, 45, 0], translation [0,0,0], scale 0.625
        // PoseStack composition: T_disp * R * S * T(-0.5).
        const glm::mat4& OuterIsoMatrix() {
            static glm::mat4 m = []{
                glm::mat4 mat(1.0f);
                // T_disp = (0,0,0) — skip.
                mat = glm::rotate(mat, glm::radians(30.0f), glm::vec3(1, 0, 0));
                mat = glm::rotate(mat, glm::radians(45.0f), glm::vec3(0, 1, 0));
                mat = glm::scale(mat, glm::vec3(0.625f));
                mat = glm::translate(mat, glm::vec3(-0.5f, -0.5f, -0.5f));
                return mat;
            }();
            return m;
        }

        constexpr float SHULKER_GUI_ROT_X = 30.0f;
        constexpr float SHULKER_GUI_ROT_Y = 45.0f;

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

        // BuildCubeFaces — same logic as BedItemRenderer / ChestItemRenderer.
        // Cube vertices in BLOCK units; poseMatrix transforms them into world space.
        // Per-face shading is computed from the actual poseMatrix so each cube
        // (lid / base) lights up correctly even if their poses differ.
        void BuildCubeFaces(std::vector<CubeFace>& out,
                            const glm::mat4& poseMatrix,
                            glm::vec3 from, glm::vec3 to,
                            float xTexOffs, float yTexOffs, float w, float h, float d)
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

            using ItemLighting::ComputeShade;
            using ItemLighting::ShadeAsColor;
            const glm::mat3 normalMat(poseMatrix);
            auto computeShadeFor = [&](glm::vec3 cubeLocalNormal) {
                glm::vec3 worldNormal = normalMat * cubeLocalNormal;
                return ShadeAsColor(ComputeShade(worldNormal, SHULKER_GUI_ROT_X, SHULKER_GUI_ROT_Y));
            };
            const uint32_t SHADE_DOWN  = computeShadeFor({ 0,-1, 0});
            const uint32_t SHADE_UP    = computeShadeFor({ 0, 1, 0});
            const uint32_t SHADE_WEST  = computeShadeFor({-1, 0, 0});
            const uint32_t SHADE_EAST  = computeShadeFor({ 1, 0, 0});
            const uint32_t SHADE_NORTH = computeShadeFor({ 0, 0,-1});
            const uint32_t SHADE_SOUTH = computeShadeFor({ 0, 0, 1});

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

        void RenderShulkerBoxInventory(GuiGraphics& g, const Game::ItemStack& stack, int x, int y) {
            const auto& item = Game::ItemRegistry::Get(stack.itemId);
            // texture variant: "shulker" (uncolored), "shulker_red", "shulker_blue", …
            const std::string texName = item.specialTexture.empty() ? "shulker" : item.specialTexture;
            TextureHandle tex = LoadShulkerTexture(texName);
            if (tex == INVALID_TEXTURE) return;
            GuiRenderState* rs = g.GetRenderState();
            if (!rs) return;

            // Combined per-piece pose:
            //   prepareModel + partPose
            //   = T(0.5, 0.5, 0.5) * S(1, -1, -1) * T(0, -1, 0) * T(0, 1.5, 0)
            //   = T(0.5, 0.5, 0.5) * S(1, -1, -1) * T(0, 0.5, 0)
            // (0.9995 scale ignored — visually negligible.)
            // Both lid and base share this same pose since both have PartPose.offset(0, 24, 0)
            // and setupAnim(progress=0) leaves lid.x/y/z at the same 0,24,0.
            auto piecePose = []{
                glm::mat4 m(1.0f);
                m = glm::translate(m, glm::vec3(0.5f, 0.5f, 0.5f));
                m = glm::scale(m, glm::vec3(1.0f, -1.0f, -1.0f));
                m = glm::translate(m, glm::vec3(0.0f, 0.5f, 0.0f));
                return m;
            }();

            const float scale = 16.0f;
            const float cx = static_cast<float>(x) + 8.0f;
            const float cy = static_cast<float>(y) + 8.0f;

            std::vector<CubeFace> faces;
            faces.reserve(12);

            // Cube vertices in BLOCK units.
            // Lid:  pixel (-8,-16,-8) to (8,-4,8)  →  block (-0.5,-1.0,-0.5) to (0.5,-0.25,0.5)
            BuildCubeFaces(faces, piecePose,
                           glm::vec3(-0.5f, -1.0f, -0.5f), glm::vec3(0.5f, -0.25f, 0.5f),
                           /*texOffs*/0, 0, /*w,h,d*/16, 12, 16);
            // Base: pixel (-8, -8,-8) to (8, 0,8)  →  block (-0.5,-0.5,-0.5) to (0.5, 0,  0.5)
            BuildCubeFaces(faces, piecePose,
                           glm::vec3(-0.5f, -0.5f, -0.5f), glm::vec3(0.5f, 0.0f, 0.5f),
                           /*texOffs*/0, 28, /*w,h,d*/16, 8, 16);

            std::stable_sort(faces.begin(), faces.end(),
                             [](const CubeFace& a, const CubeFace& b) { return a.depth < b.depth; });
            for (const auto& f : faces) {
                SubmitFace(rs, tex, cx, cy, scale, f);
            }
        }
    } // namespace

    void RegisterShulkerBoxItemRenderer() {
        const int total = static_cast<int>(Game::BlockID::Count);
        for (int i = 1; i < total; ++i) {
            const auto& it = Game::ItemRegistry::Get(static_cast<Game::ItemID>(i));
            if (it.specialKind == "shulker_box") {
                GuiGraphics::RegisterCustomItemRenderer(static_cast<Game::ItemID>(i), &RenderShulkerBoxInventory);
            }
        }
    }

} // namespace Render
