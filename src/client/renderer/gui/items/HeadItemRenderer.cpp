// File: src/client/renderer/gui/items/HeadItemRenderer.cpp
//
// Mob head item renderer — equivalent to MC's SkullBlockRenderer rendering
// of Items.SKELETON_SKULL, CREEPER_HEAD, ZOMBIE_HEAD, etc. via
// BlockEntityWithoutLevelRenderer when shown in inventory slots.
//
// Geometry (from net.minecraft.client.model.object.skull.SkullModel.createHeadModel):
//   • One 8×8×8 cube at PartPose.ZERO with addBox(-4, -8, -4, 8, 8, 8) and texOffs(0, 0).
//     In MC's Y-down model space this puts the head at (-4..4, -8..0, -4..4).
//   • Mob heads use a 64×32 texture; humanoid heads (player/zombie) ship a 64×64
//     atlas with a hat overlay at texOffs(32, 0) — we omit the hat for v1 since
//     it requires a second cube with CubeDeformation(0.25) and most inventory
//     icons are recognizable without it.
//
// Texture map (verbatim from SkullBlockRenderer.SKIN_BY_TYPE):
//   skeleton        → assets/textures/entity/skeleton/skeleton.png         (64×32)
//   wither_skeleton → assets/textures/entity/skeleton/wither_skeleton.png  (64×32)
//   zombie          → assets/textures/entity/zombie/zombie.png             (64×64)
//   creeper         → assets/textures/entity/creeper/creeper.png           (64×32)
//   piglin          → assets/textures/entity/piglin/piglin.png             (64×64)
//   dragon          → uses DragonHeadModel (different geometry); we render the
//                     8×8×8 head cube against the dragon texture as a passable
//                     fallback rather than write a separate dragon model.
//   player_head     → assets/textures/entity/player/wide/steve.png         (64×64)
//
// Inventory pose (mirrors SkullSpecialRenderer.submit → SkullBlockRenderer.submitSkull
// with rot=180.0F, then ItemTransform.apply for the GUI display):
//   poseStack.translate(0.5, 0, 0.5);            ← BEWLR T_bewlr
//   poseStack.scale(-1, -1, 1);                  ← BEWLR S_bewlr
//   model.head.yRot = 180°                       ← head PartPose rotation (CRITICAL — without
//                                                  this the FACE side of the cube ends up away
//                                                  from the camera and you see the back of the
//                                                  skull instead of the eyes/mouth)
//   then display.gui from template_skull.json:
//      translation [0, 3, 0]   (pixel units → /16 = 0.1875)
//      rotation    [30, 45, 0]
//      scale       [1, 1, 1]
//
// PoseStack post-multiplies, so vertex transform order (rightmost first):
//   M = T_disp * R_disp * S_disp * T(-0.5,-0.5,-0.5) * T_bewlr * S_bewlr * R_head_y180 * vertex
// The inner T(-0.5,-0.5,-0.5) is the standard re-center MC bakes into ItemTransform
// (decompiled net/minecraft/client/renderer/block/model/ItemTransform.java line 31-40).

#include "HeadItemRenderer.hpp"
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
#include <vector>
#include <algorithm>
#include <unordered_map>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {
        // Variant key → relative texture path. Mirrors SkullBlockRenderer.SKIN_BY_TYPE.
        const std::string& TexturePathFor(const std::string& kind) {
            static const std::unordered_map<std::string, std::string> map = {
                {"skeleton",        "assets/textures/entity/skeleton/skeleton.png"},
                {"wither_skeleton", "assets/textures/entity/skeleton/wither_skeleton.png"},
                {"zombie",          "assets/textures/entity/zombie/zombie.png"},
                {"creeper",         "assets/textures/entity/creeper/creeper.png"},
                {"piglin",          "assets/textures/entity/piglin/piglin.png"},
                {"dragon",          "assets/textures/entity/enderdragon/dragon.png"},
                {"player",          "assets/textures/entity/player/wide/steve.png"},
            };
            static const std::string empty;
            auto it = map.find(kind);
            return it != map.end() ? it->second : empty;
        }

        // Texture height for each variant — humanoid heads are 64×64, mob heads 64×32.
        // Affects the V coordinate normalization for the 8×8×8 head cube.
        float TextureHeightFor(const std::string& kind) {
            if (kind == "zombie" || kind == "piglin" || kind == "player") return 64.0f;
            return 32.0f;
        }

        std::unordered_map<std::string, TextureHandle>& HeadTextureCache() {
            static std::unordered_map<std::string, TextureHandle> m;
            return m;
        }

        TextureHandle LoadHeadTexture(const std::string& kind) {
            auto& cache = HeadTextureCache();
            auto it = cache.find(kind);
            if (it != cache.end()) return it->second;
            if (!g_renderBackend) return INVALID_TEXTURE;
            const std::string& rel = TexturePathFor(kind);
            if (rel.empty()) {
                Log::Warning("[HeadItemRenderer] unknown head kind '%s'", kind.c_str());
                cache[kind] = INVALID_TEXTURE;
                return INVALID_TEXTURE;
            }
            const std::string full = PlatformMain::GetAssetPath(rel);
            if (!std::filesystem::exists(full)) {
                Log::Warning("[HeadItemRenderer] %s not found at %s", rel.c_str(), full.c_str());
                cache[kind] = INVALID_TEXTURE;
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[HeadItemRenderer] stbi_load failed for %s: %s", full.c_str(), stbi_failure_reason());
                cache[kind] = INVALID_TEXTURE;
                return INVALID_TEXTURE;
            }
            TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap (tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            cache[kind] = tex;
            return tex;
        }

        // Display.gui values from template_skull.json (used by mob head items).
        // The dragon_head item model overrides scale to 0.6 and translation to (-2, 2, 0)
        // so we branch on the kind below.
        constexpr float HEAD_GUI_ROT_X = 30.0f;
        constexpr float HEAD_GUI_ROT_Y = 45.0f;

        struct DisplayGui { float tx, ty, tz, scale; };
        DisplayGui DisplayFor(const std::string& kind) {
            if (kind == "dragon") {
                // dragon_head.json: translation [-2, 2, 0], scale 0.6 (verbatim).
                return { -2.0f/16.0f, 2.0f/16.0f, 0.0f, 0.6f };
            }
            // template_skull.json: translation [0, 3, 0], scale 1.0
            return { 0.0f, 3.0f/16.0f, 0.0f, 1.0f };
        }

        // Build the outer iso matrix for a given variant. Same structure as the bed
        // and shulker box renderers — see BedItemRenderer.cpp for the derivation
        // notes on T_disp * R_disp * S_disp * T(-0.5) ordering.
        glm::mat4 BuildOuterIso(const std::string& kind) {
            const DisplayGui d = DisplayFor(kind);
            glm::mat4 m(1.0f);
            m = glm::translate(m, glm::vec3(d.tx, d.ty, d.tz));
            m = glm::rotate(m, glm::radians(HEAD_GUI_ROT_X), glm::vec3(1, 0, 0));
            m = glm::rotate(m, glm::radians(HEAD_GUI_ROT_Y), glm::vec3(0, 1, 0));
            m = glm::scale(m, glm::vec3(d.scale));
            m = glm::translate(m, glm::vec3(-0.5f, -0.5f, -0.5f));
            return m;
        }

        glm::vec2 Project(const glm::mat4& iso, glm::vec3 p, float cx, float cy, float scale) {
            glm::vec4 q = iso * glm::vec4(p, 1.0f);
            return glm::vec2(cx + q.x * scale, cy + (-q.y) * scale);
        }
        float TransformedZ(const glm::mat4& iso, glm::vec3 p) {
            return (iso * glm::vec4(p, 1.0f)).z;
        }

        struct CubeFace {
            std::array<glm::vec3, 4> v;
            float u_min, v_min, u_max, v_max;
            uint32_t color;
            float depth;
        };

        // Build all 6 faces of the head cube using MC's exact polygon layout (matches
        // the chest/bed renderers). Cube positions are in BLOCK units; texOffs / w / h / d
        // are in PIXEL units against `texHeight`-tall texture (64 wide always).
        void BuildCubeFaces(std::vector<CubeFace>& out,
                            const glm::mat4& outerIso, const glm::mat4& poseMatrix,
                            glm::vec3 from, glm::vec3 to,
                            float xTexOffs, float yTexOffs, float w, float h, float d,
                            float texWidth, float texHeight)
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
                return ShadeAsColor(ComputeShade(worldNormal, HEAD_GUI_ROT_X, HEAD_GUI_ROT_Y));
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
                // Normalize UVs to [0,1] using the variant's actual texture dimensions
                // (mob heads = 64×32, humanoid heads = 64×64).
                f.u_min = um / texWidth;  f.v_min = vm / texHeight;
                f.u_max = uM / texWidth;  f.v_max = vM / texHeight;
                f.color = col;
                float sum = 0.0f;
                for (int i = 0; i < 4; ++i) sum += TransformedZ(outerIso, verts[i]);
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
                        const glm::mat4& outerIso,
                        float cx, float cy, float scale, const CubeFace& f)
        {
            QuadCommand q;
            q.texture = tex;
            q.color = f.color;
            for (int i = 0; i < 4; ++i) {
                glm::vec2 p = Project(outerIso, f.v[i], cx, cy, scale);
                q.px[i] = p.x;
                q.py[i] = p.y;
            }
            // UVs already normalized in BuildCubeFaces. Order matches MC's
            // ModelPart.Polygon.remap (ModelPart.java line 339-342), which assigns
            // vertex[0]=(u_max, v_min), [1]=(u_min, v_min), [2]=(u_min, v_max),
            // [3]=(u_max, v_max) — CCW starting from the top-right of the texture
            // rect. The other BEWLR renderers (chest/bed/banner/shulker_box) use a
            // CW-from-top-left order which U-mirrors the texture; that's invisible
            // on those mostly-symmetric textures, but the head's asymmetric face
            // (eyes/ear direction) needs the exact MC mapping.
            q.u[0] = f.u_max; q.v[0] = f.v_min;
            q.u[1] = f.u_min; q.v[1] = f.v_min;
            q.u[2] = f.u_min; q.v[2] = f.v_max;
            q.u[3] = f.u_max; q.v[3] = f.v_max;
            rs->SubmitQuad(q);
        }

        // Map an item's specialKind/specialTexture to the variant string used by
        // the texture map. specialKind == "player_head" is treated as kind="player".
        // specialKind == "head" uses specialTexture as the kind (creeper, skeleton, …).
        std::string ResolveKind(const Game::Item& item) {
            if (item.specialKind == "player_head") return "player";
            if (!item.specialTexture.empty()) return item.specialTexture;
            return "skeleton"; // safest fallback — exists in every MC asset dump
        }

        void RenderHeadInventory(GuiGraphics& g, const Game::ItemStack& stack, int x, int y) {
            const auto& item = Game::ItemRegistry::Get(stack.itemId);
            const std::string kind = ResolveKind(item);
            TextureHandle tex = LoadHeadTexture(kind);
            if (tex == INVALID_TEXTURE) return;
            GuiRenderState* rs = g.GetRenderState();
            if (!rs) return;

            const glm::mat4 outerIso = BuildOuterIso(kind);
            const float scale = 16.0f;
            const float cx = static_cast<float>(x) + 8.0f;
            const float cy = static_cast<float>(y) + 8.0f;

            // BEWLR pose for the head: translate(0.5, 0, 0.5) then scale(-1, -1, 1),
            // then the head PartPose's yRot of 180° (set by SkullModel.setupAnim with
            // rot=180.0F passed by SkullSpecialRenderer). Without the 180° rotation the
            // FACE-textured face ends up at the back of the icon and you see only the
            // back-of-head texture.
            glm::mat4 bewlrPose(1.0f);
            bewlrPose = glm::translate(bewlrPose, glm::vec3(0.5f, 0.0f, 0.5f));
            bewlrPose = glm::scale(bewlrPose, glm::vec3(-1.0f, -1.0f, 1.0f));
            bewlrPose = glm::rotate(bewlrPose, glm::radians(180.0f), glm::vec3(0, 1, 0));

            // Build a child PartPose matrix matching MC's
            // ModelPart.translateAndRotate (line 167):
            //   poseStack.translate(x/16, y/16, z/16);
            //   poseStack.mulPose(Quaternionf.rotationZYX(zRot, yRot, xRot));
            //   poseStack.scale(xs, ys, zs);
            // Quaternionf.rotationZYX is INTRINSIC ZYX → matrix Rz * Ry * Rx applied
            // as M*v with Rx FIRST on the vertex. PoseStack post-multiplies, so
            // build glm with translate, then Rz, then Ry, then Rx, then scale.
            auto partPose = [](float x, float y, float z,
                               float xRot, float yRot, float zRot,
                               float xs = 1.0f, float ys = 1.0f, float zs = 1.0f) {
                glm::mat4 m(1.0f);
                m = glm::translate(m, glm::vec3(x/16.0f, y/16.0f, z/16.0f));
                if (zRot != 0) m = glm::rotate(m, zRot, glm::vec3(0, 0, 1));
                if (yRot != 0) m = glm::rotate(m, yRot, glm::vec3(0, 1, 0));
                if (xRot != 0) m = glm::rotate(m, xRot, glm::vec3(1, 0, 0));
                if (xs != 1 || ys != 1 || zs != 1)
                    m = glm::scale(m, glm::vec3(xs, ys, zs));
                return m;
            };

            // Helper to add one MC-style cube using its PIXEL-space addBox args.
            auto addBoxPixels = [&](std::vector<CubeFace>& faces, const glm::mat4& pose,
                                    float minX, float minY, float minZ,
                                    float w, float h, float d,
                                    float xTexOffs, float yTexOffs,
                                    float texW, float texH) {
                BuildCubeFaces(faces, outerIso, pose,
                               glm::vec3(minX/16.0f, minY/16.0f, minZ/16.0f),
                               glm::vec3((minX+w)/16.0f, (minY+h)/16.0f, (minZ+d)/16.0f),
                               xTexOffs, yTexOffs, w, h, d, texW, texH);
            };

            std::vector<CubeFace> faces;
            const float PI = 3.14159265358979323846f;

            if (kind == "dragon") {
                // DragonHeadModel.createHeadLayer (DragonHeadModel.java line 20-27):
                //   root part `head` at PartPose.offset(0, -7.986666, 0).scaled(0.75)
                //   addBox("upper_lip",   -6, -1, -24, 12, 5, 16, texOffs(176, 44))
                //   addBox("upper_head",  -8, -8, -10, 16, 16, 16, texOffs(112, 30))  [mirror]
                //   addBox("scale",       -5, -12, -4, 2, 4, 6,    texOffs(0,   0))   [mirror]
                //   addBox("nostril",     -5, -3, -22, 2, 2, 4,    texOffs(112, 0))   [mirror]
                //   addBox("scale",        3, -12, -4, 2, 4, 6,    texOffs(0,   0))
                //   addBox("nostril",      3, -3, -22, 2, 2, 4,    texOffs(112, 0))
                //   child "jaw" at offset(0, 4, -8):
                //     addBox("jaw",       -6,  0, -16, 12, 4, 16,  texOffs(176, 65))
                // Texture is 256×256. We skip MC's mirror flag (subtle U-axis flip on
                // the left-side cubes) for v1 — at inventory size it's invisible.
                const glm::mat4 headPose = bewlrPose * partPose(0.0f, -7.986666f, 0.0f, 0, 0, 0, 0.75f, 0.75f, 0.75f);
                // Jaw rotation matches MC's DragonHeadModel.setupAnim line 31:
                //   jaw.xRot = (sin(animationPos * π * 0.2) + 1) * 0.2
                // SkullSpecialRenderer passes the JSON's "animation" field (default 0)
                // → jaw.xRot at rest = (sin(0) + 1) * 0.2 = 0.2 radians (≈11.5°).
                // Without this the mouth renders shut.
                const float jawRestXRot = 0.2f;
                const glm::mat4 jawPose  = headPose * partPose(0.0f, 4.0f, -8.0f, jawRestXRot, 0, 0);
                const float TW = 256.0f, TH = 256.0f;
                // Build BODY group (lip, upper_head, jaw) — the big cubes. Painter's
                // sort within this group works fine since they don't overlap each other
                // in screen space.
                std::vector<CubeFace> bodyFaces;
                bodyFaces.reserve(18);
                addBoxPixels(bodyFaces, headPose, -6, -1, -24, 12, 5, 16, 176, 44, TW, TH); // upper_lip
                addBoxPixels(bodyFaces, headPose, -8, -8, -10, 16, 16, 16, 112, 30, TW, TH); // upper_head
                addBoxPixels(bodyFaces, jawPose,  -6,  0, -16, 12, 4, 16, 176, 65, TW, TH); // jaw

                // Build ORNAMENT group (scales, nostrils). These are small protrusions
                // that geometrically poke ABOVE / IN FRONT of the head body, so they
                // should always render on top. Sort by depth within the group, then
                // draw AFTER the body group — painter's algorithm with face-center
                // depth alone puts the back-side scale BEHIND the head body (its center
                // really is behind), which makes it disappear behind the silhouette.
                // Always-on-top is the right call for these inventory horns.
                std::vector<CubeFace> ornamentFaces;
                ornamentFaces.reserve(24);
                addBoxPixels(ornamentFaces, headPose, -5, -12, -4, 2, 4, 6,    0,  0, TW, TH); // left scale
                addBoxPixels(ornamentFaces, headPose,  3, -12, -4, 2, 4, 6,    0,  0, TW, TH); // right scale
                addBoxPixels(ornamentFaces, headPose, -5, -3, -22, 2, 2, 4,  112,  0, TW, TH); // left nostril
                addBoxPixels(ornamentFaces, headPose,  3, -3, -22, 2, 2, 4,  112,  0, TW, TH); // right nostril

                std::stable_sort(bodyFaces.begin(), bodyFaces.end(),
                                 [](const CubeFace& a, const CubeFace& b) { return a.depth < b.depth; });
                std::stable_sort(ornamentFaces.begin(), ornamentFaces.end(),
                                 [](const CubeFace& a, const CubeFace& b) { return a.depth < b.depth; });
                for (const auto& f : bodyFaces)     SubmitFace(rs, tex, outerIso, cx, cy, scale, f);
                for (const auto& f : ornamentFaces) SubmitFace(rs, tex, outerIso, cx, cy, scale, f);
                return; // skip the generic sort/submit at the bottom
            } else if (kind == "piglin") {
                // PiglinModel.addHead (AbstractPiglinModel.java line 59-63):
                //   head root: addBox(-5, -8, -4, 10, 8, 8, texOffs(0, 0))
                //              addBox(-2, -4, -5, 4, 4, 1, texOffs(31, 1))   ← snout
                //              addBox( 2, -2, -5, 1, 2, 1, texOffs(2, 4))    ← left tusk
                //              addBox(-3, -2, -5, 1, 2, 1, texOffs(2, 0))    ← right tusk
                //   left_ear:  offset(4.5, -6, 0), zRot=-π/6, addBox(0, 0, -2, 1, 5, 4, texOffs(51, 6))
                //   right_ear: offset(-4.5, -6, 0), zRot= π/6, addBox(-1, 0, -2, 1, 5, 4, texOffs(39, 6))
                // Texture is 64×64. Tusks/snout sit on the front face (z=-5..-4).
                const float TW = 64.0f, TH = 64.0f;
                const glm::mat4 leftEarPose  = bewlrPose * partPose( 4.5f, -6.0f, 0.0f, 0, 0, -PI/6.0f);
                const glm::mat4 rightEarPose = bewlrPose * partPose(-4.5f, -6.0f, 0.0f, 0, 0,  PI/6.0f);
                faces.reserve(36); // 6 cubes × 6 faces
                addBoxPixels(faces, bewlrPose, -5, -8, -4, 10, 8, 8,  0, 0, TW, TH); // head
                addBoxPixels(faces, bewlrPose, -2, -4, -5,  4, 4, 1, 31, 1, TW, TH); // snout
                addBoxPixels(faces, bewlrPose,  2, -2, -5,  1, 2, 1,  2, 4, TW, TH); // left tusk
                addBoxPixels(faces, bewlrPose, -3, -2, -5,  1, 2, 1,  2, 0, TW, TH); // right tusk
                addBoxPixels(faces, leftEarPose,  0,  0, -2, 1, 5, 4, 51, 6, TW, TH); // left ear
                addBoxPixels(faces, rightEarPose,-1,  0, -2, 1, 5, 4, 39, 6, TW, TH); // right ear
            } else {
                // SkullModel cube: addBox(-4, -8, -4, 8, 8, 8) → MC pixel-space cube.
                const float texW = 64.0f;
                const float texH = TextureHeightFor(kind);
                faces.reserve(6);
                addBoxPixels(faces, bewlrPose, -4, -8, -4, 8, 8, 8, 0, 0, texW, texH);
            }

            std::stable_sort(faces.begin(), faces.end(),
                             [](const CubeFace& a, const CubeFace& b) { return a.depth < b.depth; });
            for (const auto& f : faces) {
                SubmitFace(rs, tex, outerIso, cx, cy, scale, f);
            }
        }
    } // namespace

    void RegisterHeadItemRenderer() {
        const int total = static_cast<int>(Game::BlockID::Count);
        for (int i = 1; i < total; ++i) {
            const auto& it = Game::ItemRegistry::Get(static_cast<Game::ItemID>(i));
            if (it.specialKind == "head" || it.specialKind == "player_head") {
                GuiGraphics::RegisterCustomItemRenderer(static_cast<Game::ItemID>(i), &RenderHeadInventory);
            }
        }
    }

} // namespace Render
