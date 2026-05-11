// File: src/client/renderer/gui/items/ShieldItemRenderer.cpp
//
// Shield item renderer — equivalent to MC's ShieldSpecialRenderer
// (ShieldSpecialRenderer.java, ShieldModel.java). Two cubes:
//   • plate:  addBox(-6, -11, -2, 12, 22, 1)  texOffs(0, 0)
//   • handle: addBox(-1,  -3, -1,  2,  6, 6)  texOffs(26, 0)
// (ShieldModel.java lines 30-31, both texOffs and box dims verbatim.)
//
// Texture: assets/textures/entity/shield_base_nopattern.png (64×64). The
// banner-pattern variant (shield_base.png) is used when the stack carries
// BANNER_PATTERNS components — for v1 we always use the no-pattern texture.
//
// Inventory pose:
//   1. MC's BEWLR `submit()` does poseStack.scale(1, -1, -1)
//      (ShieldSpecialRenderer.java line 43) — flips Y and Z so the model is
//      upright in the GUI camera space.
//   2. The legacy assets/models/item/shield.json `display.gui` block:
//      rotation [15, -25, -5], translation [2, 3, 0], scale [0.65].
//      Display transforms are applied AFTER the BEWLR's own transform —
//      see ItemRenderer / ItemDisplayContext.GUI plumbing.
//
// Render pipeline mirrors ChestItemRenderer: per-cube 6 faces submitted to
// the GUI quad pipeline, sorted back-to-front by face-center Z (painter's
// algorithm — GUI pipeline has depth test disabled).

#include "ShieldItemRenderer.hpp"
#include "ItemLighting.hpp"
#include "../GuiGraphics.hpp"
#include "../GuiRenderState.hpp"
#include "../../backend/RenderBackend.hpp"
#include "common/entity/Item.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <array>
#include <algorithm>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {
        TextureHandle& ShieldTexture() {
            static TextureHandle s = INVALID_TEXTURE;
            return s;
        }
        bool& ShieldTextureTried() {
            static bool b = false;
            return b;
        }

        TextureHandle LoadShieldTexture() {
            auto& tex = ShieldTexture();
            auto& tried = ShieldTextureTried();
            if (tried) return tex;
            tried = true;
            if (!g_renderBackend) return INVALID_TEXTURE;
            const std::string full = PlatformMain::GetAssetPath("assets/textures/entity/shield_base_nopattern.png");
            if (!std::filesystem::exists(full)) {
                Log::Warning("[ShieldItemRenderer] texture not found at %s", full.c_str());
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[ShieldItemRenderer] stbi_load failed: %s", stbi_failure_reason());
                return INVALID_TEXTURE;
            }
            tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap (tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            return tex;
        }

        // Combined transform: MC's BEWLR scale(1,-1,-1) ∘ shield.json gui display.
        // ShieldModel coords are in MC "model space" (16 units = 1 block). The
        // shield is centered around the origin: plate spans (-6..6, -11..11, -2..-1).
        // We don't divide by 16 — the iso applies the gui scale (0.65) directly
        // to model-space units, and Project below scales by `scale` (16 px per
        // block) to convert to screen pixels. Combined effect: 1 model unit ≈
        // 16 * 0.65 = ~10 GUI pixels, which fits the 22-tall plate into the
        // 16-pixel slot with a bit of overhang (matching MC's inventory look).
        constexpr float SHIELD_GUI_ROT_X =  15.0f;
        constexpr float SHIELD_GUI_ROT_Y = -25.0f;
        constexpr float SHIELD_GUI_ROT_Z =  -5.0f;
        constexpr float SHIELD_GUI_TX    =   2.0f / 16.0f;
        constexpr float SHIELD_GUI_TY    =   3.0f / 16.0f;
        constexpr float SHIELD_GUI_TZ    =   0.0f;
        constexpr float SHIELD_GUI_SCALE =   0.65f;

        const glm::mat4& IsoMatrix() {
            static glm::mat4 m = []{
                // Mirrors MC's full ItemTransform.apply (ItemTransform.java:37-40)
                // + ShieldSpecialRenderer.java:43 BEWLR pose.scale(1, -1, -1).
                // MC's effective vertex transform is:
                //   v → Sf → T(-0.5) → S_disp → R_disp → T_disp → outer
                // We compose the equivalent matrix here. T(-0.5) is the
                // "shift the model so the slot's origin (cx, cy) lines up
                // with the model's centroid" offset that MC adds to ALL
                // display.gui transforms (line 40 of ItemTransform.java).
                // Without it the shield's origin sits at the slot's TOP-LEFT
                // corner instead of center → shield drifts up-and-right.
                glm::mat4 mat(1.0f);
                mat = glm::translate(mat, glm::vec3(SHIELD_GUI_TX, SHIELD_GUI_TY, SHIELD_GUI_TZ));
                mat = glm::rotate(mat, glm::radians(SHIELD_GUI_ROT_X), glm::vec3(1, 0, 0));
                mat = glm::rotate(mat, glm::radians(SHIELD_GUI_ROT_Y), glm::vec3(0, 1, 0));
                mat = glm::rotate(mat, glm::radians(SHIELD_GUI_ROT_Z), glm::vec3(0, 0, 1));
                mat = glm::scale(mat, glm::vec3(SHIELD_GUI_SCALE));
                // T(-0.5, -0.5, -0.5) — universal MC display-context centering
                // offset. Same as the one ChestItemRenderer applies in its
                // IsoMatrix. Block units (vertices are pre-/16 by Si below).
                mat = glm::translate(mat, glm::vec3(-0.5f, -0.5f, -0.5f));
                // BEWLR scale(1,-1,-1) — ShieldSpecialRenderer.java:43.
                mat = glm::scale(mat, glm::vec3(1.0f, -1.0f, -1.0f));
                // Si: model units (16 units = 1 block) → block units. MC bakes
                // this into vertices via Vertex.worldX() = x/16 (ModelPart.java
                // line 368); we do it as a final scale here.
                mat = glm::scale(mat, glm::vec3(1.0f / 16.0f));
                return mat;
            }();
            return m;
        }

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
        };

        float FaceDepthKey(const glm::mat4& iso, const CubeFace& f) {
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i) sum += TransformedZ(iso, f.v[i]);
            return sum * 0.25f;
        }

        void SubmitFace(GuiRenderState* rs, TextureHandle tex,
                        const glm::mat4& iso, float cx, float cy, float scale,
                        const CubeFace& f)
        {
            const float TEX = 64.0f;
            QuadCommand q;
            q.texture = tex;
            q.color = f.color;
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

        // Build all 6 faces of one cube using MC's exact CubeListBuilder UV layout.
        // `from`/`to` are in MC model-space units (NOT divided by 16 — the iso matrix
        // handles the units→blocks scale).
        void BuildCubeFaces(std::array<CubeFace, 6>& out,
                            glm::vec3 from, glm::vec3 to,
                            float xTexOffs, float yTexOffs, float w, float h, float d)
        {
            float minX=from.x, minY=from.y, minZ=from.z;
            float maxX=to.x,   maxY=to.y,   maxZ=to.z;
            glm::vec3 t0(minX, minY, minZ), t1(maxX, minY, minZ);
            glm::vec3 t2(maxX, maxY, minZ), t3(minX, maxY, minZ);
            glm::vec3 l0(minX, minY, maxZ), l1(maxX, minY, maxZ);
            glm::vec3 l2(maxX, maxY, maxZ), l3(minX, maxY, maxZ);
            float u  = xTexOffs, v = yTexOffs;
            float u0 = u, u1 = u+d, u2 = u+d+w, u22 = u+d+w+w, u3 = u+d+w+d, u4 = u+d+w+d+w;
            float v0 = v, v1 = v+d, v2 = v+d+h;
            using ItemLighting::ComputeShadeFlat;
            using ItemLighting::ShadeAsColor;
            // ── Lighting: shield.json declares `gui_light: "front"`, which in
            // MC means `ModelRenderProperties.usesBlockLight() = false`, which
            // makes GuiRenderer.renderItemToAtlas (line 388) call
            // `Lighting.setupFor(Entry.ITEMS_FLAT)` instead of ITEMS_3D.
            //
            // ITEMS_FLAT lights illuminate front-facing surfaces strongly
            // (bright direct light from in front of the model) — which is
            // why MC's shield appears LIT UP. ITEMS_3D illuminates from the
            // upper-back, leaving front faces in shadow — which made our
            // shield appear DARK when we used the wrong shader.
            //
            // ── Pre-flip normals to compensate for our extra BEWLR scale flip.
            // BuildNormalMatrix bakes scale(1,-1,1) (the Y-flip from
            // renderItemToAtlas's scale(size,-size,size)). Our IsoMatrix has an
            // EXTRA scale(1,-1,-1) (BEWLR flip — ShieldSpecialRenderer.java:43)
            // so I need to pre-multiply the input normal by scale(1,-1,-1).
            // Net effect: negate Y AND Z on the original MC face normals:
            //   DOWN → UP, UP → DOWN, NORTH → SOUTH, SOUTH → NORTH.
            //   WEST/EAST unchanged (X not flipped).
            const uint32_t SHADE_DOWN  = ShadeAsColor(ComputeShadeFlat({ 0, 1, 0}, SHIELD_GUI_ROT_X, SHIELD_GUI_ROT_Y));
            const uint32_t SHADE_UP    = ShadeAsColor(ComputeShadeFlat({ 0,-1, 0}, SHIELD_GUI_ROT_X, SHIELD_GUI_ROT_Y));
            const uint32_t SHADE_WEST  = ShadeAsColor(ComputeShadeFlat({-1, 0, 0}, SHIELD_GUI_ROT_X, SHIELD_GUI_ROT_Y));
            const uint32_t SHADE_EAST  = ShadeAsColor(ComputeShadeFlat({ 1, 0, 0}, SHIELD_GUI_ROT_X, SHIELD_GUI_ROT_Y));
            const uint32_t SHADE_NORTH = ShadeAsColor(ComputeShadeFlat({ 0, 0, 1}, SHIELD_GUI_ROT_X, SHIELD_GUI_ROT_Y));
            const uint32_t SHADE_SOUTH = ShadeAsColor(ComputeShadeFlat({ 0, 0,-1}, SHIELD_GUI_ROT_X, SHIELD_GUI_ROT_Y));
            out[0] = { {l1, l0, t0, t1}, u1, v0, u2,  v1, SHADE_DOWN };
            out[1] = { {t2, t3, l3, l2}, u2, v1, u22, v0, SHADE_UP };
            out[2] = { {t0, l0, l3, t3}, u0, v1, u1,  v2, SHADE_WEST };
            out[3] = { {t1, t0, t3, t2}, u1, v1, u2,  v2, SHADE_NORTH };
            out[4] = { {l1, t1, t2, l2}, u2, v1, u3,  v2, SHADE_EAST };
            out[5] = { {l0, l1, l2, l3}, u3, v1, u4,  v2, SHADE_SOUTH };
        }

        void RenderCube(GuiRenderState* rs, TextureHandle tex,
                        const glm::mat4& iso, float cx, float cy, float scale,
                        glm::vec3 from, glm::vec3 to,
                        float xTexOffs, float yTexOffs, float w, float h, float d)
        {
            std::array<CubeFace, 6> faces;
            BuildCubeFaces(faces, from, to, xTexOffs, yTexOffs, w, h, d);
            std::array<int, 6> order = {0, 1, 2, 3, 4, 5};
            std::array<float, 6> depth;
            for (int i = 0; i < 6; ++i) depth[i] = FaceDepthKey(iso, faces[i]);
            std::sort(order.begin(), order.end(),
                      [&](int a, int b) { return depth[a] < depth[b]; });
            for (int i : order) SubmitFace(rs, tex, iso, cx, cy, scale, faces[i]);
        }

        void RenderShieldInventory(GuiGraphics& g, const Game::ItemStack& stack,
                                   int x, int y) {
            (void)stack; // banner patterns not yet wired — always render base
            TextureHandle tex = LoadShieldTexture();
            if (tex == INVALID_TEXTURE) return;
            GuiRenderState* rs = g.GetRenderState();
            if (!rs) return;

            // Clip to the 16×16 slot bounds — mirrors MC's atlas scissor at
            // GuiRenderer.java:395 (`enableScissorForRenderTypeDraws(renderX,
            // ..., singleItemTextureSize, singleItemTextureSize)`). Without
            // this, the shield's plate (which by spec extends ~2 pixels above
            // the slot top because shield.json display.gui translates the
            // model up by 3 pixels) would overflow into adjacent slots.
            g.EnableScissor(x, y, x + 16, y + 16);

            const glm::mat4& iso = IsoMatrix();
            const float scale = 16.0f;
            const float cx = static_cast<float>(x) + 8.0f;
            const float cy = static_cast<float>(y) + 8.0f;

            // ShieldModel.createLayer (lines 30-31):
            //   plate:  from(-6, -11, -2) → to(6, 11, -1)   texOffs(0, 0)   12×22×1
            //   handle: from(-1,  -3, -1) → to(1,  3,  5)   texOffs(26, 0)   2×6×6
            //
            // Draw HANDLE FIRST so the plate's later draw covers it. Without
            // this, the handle (rendered second) ends up on top regardless of
            // its actual Z — the GUI pipeline has no real depth test, only
            // per-cube painter sorting, so cross-cube ordering is determined
            // by the call order. After the BEWLR Z-flip the plate sits in
            // front of the handle in world space; matching that visually
            // requires drawing the plate last.
            RenderCube(rs, tex, iso, cx, cy, scale,
                       glm::vec3(-1,  -3, -1), glm::vec3( 1,  3,  5),
                       /*texOffs*/ 26, 0, /*w,h,d*/ 2,  6, 6);
            RenderCube(rs, tex, iso, cx, cy, scale,
                       glm::vec3(-6, -11, -2), glm::vec3( 6, 11, -1),
                       /*texOffs*/  0, 0, /*w,h,d*/12, 22, 1);

            g.DisableScissor();
        }
    } // namespace

    void RegisterShieldItemRenderer() {
        // Walk pure items (shield isn't a block) and hook the BEWLR for any
        // whose items/{slug}.json declared specialKind == "shield".
        const size_t total = Game::ItemRegistry::Size();
        // We don't have a direct iterator; just check the known shield ID.
        // For now this is hardcoded to the slug → ItemID mapping in
        // GeneratedItemList.hpp. (Same shape as the chest renderer's loop;
        // we just don't need to walk all blocks.)
        for (size_t base = Game::PURE_ITEM_BASE;
             base < Game::PURE_ITEM_BASE + total; ++base) {
            const auto& it = Game::ItemRegistry::Get(static_cast<Game::ItemID>(base));
            if (it.specialKind == "shield") {
                GuiGraphics::RegisterCustomItemRenderer(
                    static_cast<Game::ItemID>(base), &RenderShieldInventory);
            }
        }
    }

} // namespace Render
