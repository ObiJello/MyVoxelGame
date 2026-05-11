// File: src/client/renderer/gui/items/ChestItemRenderer.cpp
//
// Chest item renderer — equivalent to MC's BlockEntityWithoutLevelRenderer rendering of
// Items.CHEST. Geometry mirrors net.minecraft.client.model.object.chest.ChestModel
// .createSingleBodyLayer() and the per-cube polygon/UV layout mirrors
// net.minecraft.client.model.geom.ModelPart.Cube (Cube constructor in ModelPart.java
// lines 245-310). UVs and vertex orders are copied verbatim from MC for correctness.
//
// Render pipeline differs from MC's only in that:
//   - MC uses entityCutout RenderType which has backface culling + depth test enabled.
//   - Our GUI pipeline has both DISABLED (no depth test, no culling).
// To get correct visibility without depth testing, we render all 6 faces of every cube
// and sort them back-to-front by face-center Z (after the iso transform). Painter's
// algorithm. The 6 polygons per cube replicate MC's `Cube.polygons` array exactly.
//
// Vertex naming (from MC ModelPart.java lines 268-275):
//   t0 = (minX, minY, minZ)   t1 = (maxX, minY, minZ)
//   t2 = (maxX, maxY, minZ)   t3 = (minX, maxY, minZ)
//   l0 = (minX, minY, maxZ)   l1 = (maxX, minY, maxZ)
//   l2 = (maxX, maxY, maxZ)   l3 = (minX, maxY, maxZ)
//
// Per-face polygon (vertex order + UV bounds) — MC ModelPart.java lines 286-308:
//   DOWN  : [l1, l0, t0, t1]   (u1, v0, u2,  v1)
//   UP    : [t2, t3, l3, l2]   (u2, v1, u22, v0)   ← v REVERSED
//   WEST  : [t0, l0, l3, t3]   (u0, v1, u1,  v2)
//   NORTH : [t1, t0, t3, t2]   (u1, v1, u2,  v2)
//   EAST  : [l1, t1, t2, l2]   (u2, v1, u3,  v2)
//   SOUTH : [l0, l1, l2, l3]   (u3, v1, u4,  v2)
// where u0=u, u1=u+d, u2=u+d+w, u22=u+d+w+w, u3=u+d+w+d, u4=u+d+w+d+w
//       v0=v, v1=v+d, v2=v+d+h
//
// Inventory transform: MC's `template_chest.json` GUI display: rotation [30, 45, 0],
// scale 0.625. Combined with GuiRenderer.renderItemToAtlas's scale(size, -size, size)
// (ModelPart.java is Y-up, screen is Y-down — the -size flips Y to make the chest
// upright in screen space). Our renderer does the Y-flip in the projection step
// (Project negates Y) so the iso matrix uses scale(0.625, 0.625, 0.625) directly.

#include "ChestItemRenderer.hpp"
#include "ItemLighting.hpp"
#include "../GuiGraphics.hpp"
#include "../GuiRenderState.hpp"
#include "../../backend/RenderBackend.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Log.hpp"

#include "stb_image.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <array>
#include <algorithm>
#include <unordered_map>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {
        // One cached GL texture per chest variant ("normal", "trapped", "ender",
        // "christmas", …). Each maps to assets/textures/entity/chest/{variant}.png.
        std::unordered_map<std::string, TextureHandle>& ChestTextureCache() {
            static std::unordered_map<std::string, TextureHandle> m;
            return m;
        }

        TextureHandle LoadChestTexture(const std::string& variant) {
            auto& cache = ChestTextureCache();
            auto it = cache.find(variant);
            if (it != cache.end()) return it->second;
            if (!g_renderBackend) return INVALID_TEXTURE;
            const std::string rel  = "assets/textures/entity/chest/" + variant + ".png";
            const std::string full = PlatformMain::GetAssetPath(rel);
            if (!std::filesystem::exists(full)) {
                Log::Warning("[ChestItemRenderer] %s not found at %s", rel.c_str(), full.c_str());
                cache[variant] = INVALID_TEXTURE; // negative-cache
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[ChestItemRenderer] stbi_load failed for %s: %s", full.c_str(), stbi_failure_reason());
                cache[variant] = INVALID_TEXTURE;
                return INVALID_TEXTURE;
            }
            TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap (tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            cache[variant] = tex;
            return tex;
        }

        // MC's `template_chest.json` display.gui: rotation [30, 45, 0], scale 0.625.
        // Quaternionf.rotationXYZ(angleX, angleY, angleZ) builds Rx * Ry * Rz; with Z=0 the
        // matrix is Rx(30°) * Ry(45°). Order on a vertex P: P → Ry(45°) → Rx(30°).
        const glm::mat4& IsoMatrix() {
            static glm::mat4 m = []{
                glm::mat4 mat(1.0f);
                mat = glm::rotate(mat, glm::radians(30.0f), glm::vec3(1, 0, 0));
                mat = glm::rotate(mat, glm::radians(45.0f), glm::vec3(0, 1, 0));
                mat = glm::scale(mat, glm::vec3(0.625f));
                mat = glm::translate(mat, glm::vec3(-0.5f, -0.5f, -0.5f));
                return mat;
            }();
            return m;
        }

        // Chest GUI display rotation from template_chest.json.
        constexpr float CHEST_GUI_ROT_X = 30.0f;
        constexpr float CHEST_GUI_ROT_Y = 45.0f;

        // Project a 3D point through the iso to GUI pixel coords. Negate Y because GUI
        // screen Y is down while world Y is up — equivalent to MC's scale(1, -1, 1) flip
        // in the GuiRenderer.renderItemToAtlas path.
        glm::vec2 Project(const glm::mat4& iso, glm::vec3 p, float cx, float cy, float scale) {
            glm::vec4 q = iso * glm::vec4(p, 1.0f);
            return glm::vec2(cx + q.x * scale, cy + (-q.y) * scale);
        }

        // Z value of a point AFTER the iso transform — used for back-to-front sorting of
        // the 6 cube faces (so painter's algorithm gives correct visibility despite depth
        // test being off).
        float TransformedZ(const glm::mat4& iso, glm::vec3 p) {
            glm::vec4 q = iso * glm::vec4(p, 1.0f);
            return q.z;
        }

        // One cube face: 4 vertices (in MC's polygon order) + 4 UV bounds (u_min, v_min,
        // u_max, v_max — may be reversed for UP face per MC).
        struct CubeFace {
            std::array<glm::vec3, 4> v;   // in MC's vertex order for this face
            float u_min, v_min, u_max, v_max;
            uint32_t color;
        };

        // Compute the screen-space ordering key for one face: average Z after iso. Higher
        // Z = farther from camera (drawn first in painter's algorithm).
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
            // MC's Polygon assigns UV corners in the order (vertex0=(u_min,v_min),
            // vertex1=(u_max,v_min), vertex2=(u_max,v_max), vertex3=(u_min,v_max)).
            q.u[0] = f.u_min/TEX; q.v[0] = f.v_min/TEX;
            q.u[1] = f.u_max/TEX; q.v[1] = f.v_min/TEX;
            q.u[2] = f.u_max/TEX; q.v[2] = f.v_max/TEX;
            q.u[3] = f.u_min/TEX; q.v[3] = f.v_max/TEX;
            rs->SubmitQuad(q);
        }

        // Build all 6 faces of one cube using MC's exact polygon layout from
        // ModelPart.Cube. Hidden faces are still built — we sort by depth and let painter's
        // algorithm hide them.
        void BuildCubeFaces(std::array<CubeFace, 6>& out,
                            glm::vec3 from, glm::vec3 to,
                            float xTexOffs, float yTexOffs, float w, float h, float d)
        {
            const float S = 16.0f;
            glm::vec3 mn = from / S, mx = to / S;
            float minX=mn.x, minY=mn.y, minZ=mn.z;
            float maxX=mx.x, maxY=mx.y, maxZ=mx.z;
            // MC vertex names (ModelPart.java line 268-275):
            glm::vec3 t0(minX, minY, minZ), t1(maxX, minY, minZ);
            glm::vec3 t2(maxX, maxY, minZ), t3(minX, maxY, minZ);
            glm::vec3 l0(minX, minY, maxZ), l1(maxX, minY, maxZ);
            glm::vec3 l2(maxX, maxY, maxZ), l3(minX, maxY, maxZ);
            // MC UV bounds (ModelPart.java line 276-284):
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
            // Per-face shading via MC's actual ITEMS_3D Lambertian lighting (see
            // ComputeShade above). The face normals are MC's world-axis-aligned face
            // directions; ComputeShade returns the same shading the chest gets in MC's
            // inventory render via the entity shader's diffuse-light dot product.
            using ItemLighting::ComputeShade;
            using ItemLighting::ShadeAsColor;
            const uint32_t SHADE_DOWN  = ShadeAsColor(ComputeShade({ 0,-1, 0}, CHEST_GUI_ROT_X, CHEST_GUI_ROT_Y));
            const uint32_t SHADE_UP    = ShadeAsColor(ComputeShade({ 0, 1, 0}, CHEST_GUI_ROT_X, CHEST_GUI_ROT_Y));
            const uint32_t SHADE_WEST  = ShadeAsColor(ComputeShade({-1, 0, 0}, CHEST_GUI_ROT_X, CHEST_GUI_ROT_Y));
            const uint32_t SHADE_EAST  = ShadeAsColor(ComputeShade({ 1, 0, 0}, CHEST_GUI_ROT_X, CHEST_GUI_ROT_Y));
            const uint32_t SHADE_NORTH = ShadeAsColor(ComputeShade({ 0, 0,-1}, CHEST_GUI_ROT_X, CHEST_GUI_ROT_Y));
            const uint32_t SHADE_SOUTH = ShadeAsColor(ComputeShade({ 0, 0, 1}, CHEST_GUI_ROT_X, CHEST_GUI_ROT_Y));

            // DOWN — [l1, l0, t0, t1]   (u1, v0, u2, v1)
            out[0] = { {l1, l0, t0, t1}, u1, v0, u2,  v1, SHADE_DOWN };
            // UP   — [t2, t3, l3, l2]   (u2, v1, u22, v0)   ← v REVERSED
            out[1] = { {t2, t3, l3, l2}, u2, v1, u22, v0, SHADE_UP };
            // WEST — [t0, l0, l3, t3]   (u0, v1, u1, v2)
            out[2] = { {t0, l0, l3, t3}, u0, v1, u1,  v2, SHADE_WEST };
            // NORTH— [t1, t0, t3, t2]   (u1, v1, u2, v2)
            out[3] = { {t1, t0, t3, t2}, u1, v1, u2,  v2, SHADE_NORTH };
            // EAST — [l1, t1, t2, l2]   (u2, v1, u3, v2)
            out[4] = { {l1, t1, t2, l2}, u2, v1, u3,  v2, SHADE_EAST };
            // SOUTH— [l0, l1, l2, l3]   (u3, v1, u4, v2)
            out[5] = { {l0, l1, l2, l3}, u3, v1, u4,  v2, SHADE_SOUTH };
        }

        void RenderCube(GuiRenderState* rs, TextureHandle tex,
                        const glm::mat4& iso, float cx, float cy, float scale,
                        glm::vec3 from, glm::vec3 to,
                        float xTexOffs, float yTexOffs, float w, float h, float d)
        {
            std::array<CubeFace, 6> faces;
            BuildCubeFaces(faces, from, to, xTexOffs, yTexOffs, w, h, d);

            // Sort back-to-front by face-center Z (after iso). In our ortho
            // `glm::ortho(0, w, h, 0, -1000, 1000)` HIGHER world Z = CLOSER to camera
            // (verified from glm::orthoRH_NO matrix: z_clip = -0.001 * z_world). Painter's
            // algorithm requires drawing FARTHEST first (lowest Z) and CLOSEST last (highest
            // Z) so the closer face wins. Sort ASCENDING by Z. (Was descending — that drew
            // closest first and farthest last, which made the back of the cube cover the
            // front, presenting as if the chest was upside-down.)
            std::array<int, 6> order = {0, 1, 2, 3, 4, 5};
            std::array<float, 6> depth;
            for (int i = 0; i < 6; ++i) depth[i] = FaceDepthKey(iso, faces[i]);
            std::sort(order.begin(), order.end(),
                      [&](int a, int b) { return depth[a] < depth[b]; });
            for (int i : order) SubmitFace(rs, tex, iso, cx, cy, scale, faces[i]);
        }

        void RenderChestInventory(GuiGraphics& g, const Game::ItemStack& stack,
                                  int x, int y) {
            // Per-item variant: normal / trapped / ender / christmas. ItemRegistry
            // captures this from items/{slug}.json's nested special.model.texture.
            const auto& item = Game::ItemRegistry::Get(stack.itemId);
            const std::string variant = item.specialTexture.empty() ? "normal" : item.specialTexture;
            TextureHandle tex = LoadChestTexture(variant);
            if (tex == INVALID_TEXTURE) return;
            GuiRenderState* rs = g.GetRenderState();
            if (!rs) return;

            const glm::mat4& iso = IsoMatrix();
            const float scale = 16.0f;
            const float cx = static_cast<float>(x) + 8.0f;
            const float cy = static_cast<float>(y) + 8.0f;

            // ChestModel.createSingleBodyLayer (line 26-29 of ChestModel.java):
            //   bottom: addBox(1, 0, 1,   14, 10, 14)  texOffs(0, 19)  no offset
            //   lid:    addBox(1, 0, 0,   14,  5, 14)  texOffs(0,  0)  offset(0, 9, 1)
            //   lock:   addBox(7,-2,14,    2,  4,  1)  texOffs(0,  0)  offset(0, 9, 1)
            // After offsets: bottom (1,0,1)→(15,10,15);
            //                lid    (1,9,1)→(15,14,15);
            //                lock   (7,7,15)→(9,11,16).

            RenderCube(rs, tex, iso, cx, cy, scale,
                       glm::vec3(1, 0, 1), glm::vec3(15, 10, 15),
                       /*texOffs*/0, 19, /*w,h,d*/14, 10, 14);
            RenderCube(rs, tex, iso, cx, cy, scale,
                       glm::vec3(1, 9, 1), glm::vec3(15, 14, 15),
                       /*texOffs*/0, 0, /*w,h,d*/14, 5, 14);
            RenderCube(rs, tex, iso, cx, cy, scale,
                       glm::vec3(7, 7, 15), glm::vec3(9, 11, 16),
                       /*texOffs*/0, 0, /*w,h,d*/2, 4, 1);
        }
    } // namespace

    void RegisterChestItemRenderer() {
        // Walk every block item and hook the chest BEWLR for any whose
        // assets/items/{slug}.json declared specialKind == "chest". Covers vanilla
        // chest, trapped_chest, ender_chest, and any modded chest variant. MUST be
        // called AFTER ItemRegistry::Initialize() so the registry is populated.
        const int total = static_cast<int>(Game::BlockID::Count);
        for (int i = 1; i < total; ++i) {
            const auto& it = Game::ItemRegistry::Get(static_cast<Game::ItemID>(i));
            if (it.specialKind == "chest") {
                GuiGraphics::RegisterCustomItemRenderer(static_cast<Game::ItemID>(i), &RenderChestInventory);
            }
        }
    }

} // namespace Render
