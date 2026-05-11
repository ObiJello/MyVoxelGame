// File: src/client/renderer/gui/items/BedItemRenderer.cpp
//
// Bed item renderer — equivalent to MC's BedSpecialRenderer (BlockEntityWithoutLevelRenderer
// path for Items.RED_BED, Items.BLUE_BED, etc). Geometry mirrors net.minecraft.client.
// renderer.blockentity.BedRenderer (createHeadLayer + createFootLayer) and per-piece
// pose mirrors BedRenderer.preparePose().
//
// MC's bed has two pieces (head + foot). Each piece contains:
//   • one 16×16×6 mattress cube (no per-cube rotation)
//   • two 3×3×3 leg cubes (each with its own PartPose.rotation that lays the
//     leg flat and points it toward the correct corner)
//
// Texture layout (64×64 entity texture at assets/textures/entity/bed/{color}.png):
//   Head main:       texOffs(0, 0)    16×16×6
//   Foot main:       texOffs(0, 22)   16×16×6
//   Head left leg:   texOffs(50, 6)   3×3×3   PartPose.rotation(π/2, 0, π/2)
//   Head right leg:  texOffs(50, 18)  3×3×3   PartPose.rotation(π/2, 0, π)
//   Foot left leg:   texOffs(50, 0)   3×3×3   PartPose.rotation(π/2, 0, 0)
//   Foot right leg:  texOffs(50, 12)  3×3×3   PartPose.rotation(π/2, 0, 3π/2)
//
// PartPose.rotation(xRot, yRot, zRot) maps to ModelPart.translateAndRotate
// (line 167) which calls Quaternionf.rotationZYX(zRot, yRot, xRot). JOML's
// rotationZYX rotates around Z first, then Y, then X (applied to vertex), so
// the equivalent matrix is Rx * Ry * Rz applied as M*v.
//
// Per-face UV bounds for a (w, h, d) cube at (xTexOffs, yTexOffs) — MC ModelPart.java:
//   u0=u, u1=u+d, u2=u+d+w, u22=u+d+w+w, u3=u+d+w+d, u4=u+d+w+d+w
//   v0=v, v1=v+d, v2=v+d+h
//
// Inventory pose (BedRenderer.preparePose with Direction.SOUTH):
//   translate(0, 0.5625, translateZ ? -1 : 0)        // T1: lift mattress, foot offset
//   rotate X 90°                                       // Rx: lay bed flat
//   translate(0.5, 0.5, 0.5)                          // T2: shift pivot
//   rotate Z (180 + SOUTH.toYRot() = 180 + 0 = 180)   // Rz: orient head correctly
//   translate(-0.5, -0.5, -0.5)                       // T3: shift pivot back
// The T2*Rz(180°)*T3 sandwich is a 180° rotation around the line {x=0.5, y=0.5},
// effectively a mirror of (x, y) about (0.5, 0.5). This flips which end of the
// 16x16x6 mattress is the headboard. Without this, the foot/head ends are swapped
// AND the textures on side faces are mirrored.

#include "BedItemRenderer.hpp"
#include "ItemLighting.hpp"
#include "../GuiGraphics.hpp"
#include "../GuiRenderState.hpp"
#include "../../backend/RenderBackend.hpp"
#include "common/entity/Item.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/BlockModel.hpp"
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
        // Per-color cached entity texture. Each color is loaded once on first use.
        std::unordered_map<std::string, TextureHandle>& BedTextureCache() {
            static std::unordered_map<std::string, TextureHandle> m;
            return m;
        }

        TextureHandle LoadBedTexture(const std::string& color) {
            auto& cache = BedTextureCache();
            auto it = cache.find(color);
            if (it != cache.end()) return it->second;
            if (!g_renderBackend) return INVALID_TEXTURE;
            const std::string rel  = "assets/textures/entity/bed/" + color + ".png";
            const std::string full = PlatformMain::GetAssetPath(rel);
            if (!std::filesystem::exists(full)) {
                Log::Warning("[BedItemRenderer] %s not found at %s", rel.c_str(), full.c_str());
                cache[color] = INVALID_TEXTURE;
                return INVALID_TEXTURE;
            }
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[BedItemRenderer] stbi_load failed for %s: %s", full.c_str(), stbi_failure_reason());
                cache[color] = INVALID_TEXTURE;
                return INVALID_TEXTURE;
            }
            TextureHandle tex = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                g_renderBackend->SetTextureFilter(tex, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap (tex, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
            }
            cache[color] = tex;
            return tex;
        }

        // Bed's display.gui transform — values verbatim from MC's template_bed.json
        // (the parent of every {color}_bed.json item model). NOT the standard
        // block/block defaults: bed uses a different rotation (160° not 225°), a
        // smaller scale (0.5325 not 0.625) to fit the 2-block-long bed in a 16px
        // slot, and a (2,3,0) pixel-unit translation to position it correctly.
        const glm::mat4& OuterIsoMatrix() {
            static glm::mat4 m = []{
                glm::mat4 mat(1.0f);
                // Mirrors MC's ItemTransform.apply (decompiled
                // net/minecraft/client/renderer/block/model/ItemTransform.java line 31-40):
                //   pose.translate(translation.x, translation.y, translation.z);
                //   pose.rotate(Quaternionf.rotationXYZ(rx, ry, rz));   // XYZ-intrinsic
                //   pose.scale(scale.x, scale.y, scale.z);
                //   pose.translate(-0.5F, -0.5F, -0.5F);   ← centering AT THE END
                // PoseStack post-multiplies, so the matrix on the vertex applies these
                // in REVERSE: T(-0.5) first (centers model at origin), then S, then R,
                // then T_disp.
                mat = glm::translate(mat, glm::vec3(2.0f/16.0f, 3.0f/16.0f, 0.0f));    // T_disp
                mat = glm::rotate(mat, glm::radians(30.0f),  glm::vec3(1, 0, 0));      // Rx
                mat = glm::rotate(mat, glm::radians(160.0f), glm::vec3(0, 1, 0));      // Ry
                mat = glm::scale(mat, glm::vec3(0.5325f));                              // S
                mat = glm::translate(mat, glm::vec3(-0.5f, -0.5f, -0.5f));              // T(-0.5) — applied FIRST to vertex
                return mat;
            }();
            return m;
        }

        constexpr float BED_GUI_ROT_X = 30.0f;
        constexpr float BED_GUI_ROT_Y = 160.0f;

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

        // Build all 6 faces of one cube using MC's exact polygon layout (matches our
        // chest renderer). Cube positions and texOffsets are in MC PIXEL space; the
        // poseMatrix transforms each vertex into the final iso space.
        void BuildCubeFaces(std::vector<CubeFace>& out,
                            const glm::mat4& poseMatrix,
                            glm::vec3 from, glm::vec3 to,
                            float xTexOffs, float yTexOffs, float w, float h, float d)
        {
            float minX=from.x, minY=from.y, minZ=from.z;
            float maxX=to.x,   maxY=to.y,   maxZ=to.z;
            // Vertex names match MC ModelPart.Cube:
            glm::vec3 t0(minX, minY, minZ), t1(maxX, minY, minZ);
            glm::vec3 t2(maxX, maxY, minZ), t3(minX, maxY, minZ);
            glm::vec3 l0(minX, minY, maxZ), l1(maxX, minY, maxZ);
            glm::vec3 l2(maxX, maxY, maxZ), l3(minX, maxY, maxZ);
            // Apply pose so all subsequent depth/projection math operates in world space.
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
            // Compute per-face shading from the cube's actual poseMatrix. The cube's
            // local face normal gets transformed by mat3(poseMatrix) (rotation only,
            // no translation) to get the world-space normal BEFORE the outer iso
            // rotation. Then ComputeShade applies the outer rotation (BED_GUI_ROT_X/Y)
            // on top to produce the final view-space normal that gets dotted with the
            // ITEMS_3D lights.
            //
            // This is critical for the LEGS — each leg has its own local PartPose
            // rotation that combines with the piece's preparePose differently from the
            // mattress, so a fixed SHADE_xx table per face direction would shade them
            // wrong (legs have totally different face orientations after their rotation).
            const glm::mat3 normalMat(poseMatrix);
            auto computeShadeFor = [&](glm::vec3 cubeLocalNormal) {
                glm::vec3 worldNormal = normalMat * cubeLocalNormal;
                return ShadeAsColor(ComputeShade(worldNormal, BED_GUI_ROT_X, BED_GUI_ROT_Y));
            };
            const uint32_t SHADE_DOWN  = computeShadeFor({ 0,-1, 0});
            const uint32_t SHADE_UP    = computeShadeFor({ 0, 1, 0});
            const uint32_t SHADE_WEST  = computeShadeFor({-1, 0, 0});
            const uint32_t SHADE_EAST  = computeShadeFor({ 1, 0, 0});
            const uint32_t SHADE_NORTH = computeShadeFor({ 0, 0,-1});
            const uint32_t SHADE_SOUTH = computeShadeFor({ 0, 0, 1});

            // Per-face polygon order (MC ModelPart.java lines 286-308).
            // We compute each face's depth here so the global sort below works.
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
            pushFace({l1, l0, t0, t1}, u1, v0, u2,  v1, SHADE_DOWN);   // DOWN
            pushFace({t2, t3, l3, l2}, u2, v1, u22, v0, SHADE_UP);     // UP (v reversed)
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

        void RenderBedInventory(GuiGraphics& g, const Game::ItemStack& stack, int x, int y) {
            const auto& item = Game::ItemRegistry::Get(stack.itemId);
            const std::string color = item.specialTexture.empty() ? "red" : item.specialTexture;
            TextureHandle tex = LoadBedTexture(color);
            if (tex == INVALID_TEXTURE) return;
            GuiRenderState* rs = g.GetRenderState();
            if (!rs) return;

            // Bed's internal pose for each piece — mirrors MC's BedRenderer.preparePose
            // with Direction.SOUTH. The Rz(180°) sandwiched in T2/T3 is critical:
            // without it, the head and foot ends are swapped AND face textures are
            // mirrored. Working in BLOCK units (1.0 = 1 block = 16 pixels).
            auto buildPose = [](float zOffset) {
                glm::mat4 m(1.0f);
                m = glm::translate(m, glm::vec3(0.0f, 0.5625f, zOffset));    // T1
                m = glm::rotate(m, glm::radians(90.0f), glm::vec3(1, 0, 0));  // Rx 90
                m = glm::translate(m, glm::vec3(0.5f, 0.5f, 0.5f));           // T2
                m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0, 0, 1)); // Rz 180 (= 180 + SOUTH.yRot=0)
                m = glm::translate(m, glm::vec3(-0.5f, -0.5f, -0.5f));        // T3
                return m;
            };
            const glm::mat4 headPose = buildPose(0.0f);
            const glm::mat4 footPose = buildPose(-1.0f);

            // OuterIsoMatrix already includes display.gui scale (0.5325 — set per
            // MC's template_bed.json). The 16x outer multiplier converts from
            // [0..1] block units to GUI pixels.
            const float scale = 16.0f;
            const float cx = static_cast<float>(x) + 8.0f;
            const float cy = static_cast<float>(y) + 8.0f;

            std::vector<CubeFace> faces;
            faces.reserve(36); // 6 cubes (2 mattresses + 4 legs) × 6 faces each

            // Build a leg's PartPose rotation matrix. MC ModelPart.translateAndRotate
            // line 167: Quaternionf.rotationZYX(zRot, yRot, xRot). This is INTRINSIC
            // ZYX, equivalent to matrix M = Rz * Ry * Rx (applied to vertex with order
            // Rx first, then Ry, then Rz). In glm post-mul, build Rz then Ry then Rx.
            auto legRotation = [](float xRot, float yRot, float zRot) {
                glm::mat4 m(1.0f);
                m = glm::rotate(m, zRot, glm::vec3(0, 0, 1));
                m = glm::rotate(m, yRot, glm::vec3(0, 1, 0));
                m = glm::rotate(m, xRot, glm::vec3(1, 0, 0));
                return m;
            };
            const float PI    = 3.14159265358979323846f;
            const float HALFPI = PI * 0.5f;

            // Cube vertices are in BLOCK units (divided by 16 from MC pixel space).
            // ── Head piece ──────────────────────────────────────────────────────
            // Head main: pixel (0,0,0)→(16,16,6) → block (0,0,0)→(1, 1, 0.375)
            BuildCubeFaces(faces, headPose,
                           glm::vec3(0, 0, 0), glm::vec3(1, 1, 0.375f),
                           /*texOffs*/0, 0, /*w,h,d in pixels*/16, 16, 6);
            // Head left leg: pixel (0,6,0)→(3,9,3) → block (0,0.375,0)→(0.1875,0.5625,0.1875)
            //   Apply leg's PartPose rotation BEFORE the piece's pose so the leg
            //   gets oriented relative to the un-Rx'd head, then the head pose
            //   lays the whole assembly flat.
            BuildCubeFaces(faces, headPose * legRotation(HALFPI, 0, HALFPI),
                           glm::vec3(0, 6.0f/16, 0), glm::vec3(3.0f/16, 9.0f/16, 3.0f/16),
                           /*texOffs*/50, 6, 3, 3, 3);
            // Head right leg: pixel (-16,6,0)→(-13,9,3)
            BuildCubeFaces(faces, headPose * legRotation(HALFPI, 0, PI),
                           glm::vec3(-16.0f/16, 6.0f/16, 0), glm::vec3(-13.0f/16, 9.0f/16, 3.0f/16),
                           /*texOffs*/50, 18, 3, 3, 3);

            // ── Foot piece ──────────────────────────────────────────────────────
            BuildCubeFaces(faces, footPose,
                           glm::vec3(0, 0, 0), glm::vec3(1, 1, 0.375f),
                           /*texOffs*/0, 22, /*w,h,d*/16, 16, 6);
            // Foot left leg: pixel (0,6,-16)→(3,9,-13)
            BuildCubeFaces(faces, footPose * legRotation(HALFPI, 0, 0),
                           glm::vec3(0, 6.0f/16, -16.0f/16), glm::vec3(3.0f/16, 9.0f/16, -13.0f/16),
                           /*texOffs*/50, 0, 3, 3, 3);
            // Foot right leg: pixel (-16,6,-16)→(-13,9,-13)
            BuildCubeFaces(faces, footPose * legRotation(HALFPI, 0, 1.5f*PI),
                           glm::vec3(-16.0f/16, 6.0f/16, -16.0f/16), glm::vec3(-13.0f/16, 9.0f/16, -13.0f/16),
                           /*texOffs*/50, 12, 3, 3, 3);

            // Painter's: ascending depth (back first → front last). stable_sort
            // preserves submission order on ties (foot drawn after head where depths
            // happen to equal — matches MC's piece order).
            std::stable_sort(faces.begin(), faces.end(),
                             [](const CubeFace& a, const CubeFace& b) { return a.depth < b.depth; });
            for (const auto& f : faces) {
                SubmitFace(rs, tex, cx, cy, scale, f);
            }
        }
    } // namespace

    void RegisterBedItemRenderer() {
        const int total = static_cast<int>(Game::BlockID::Count);
        for (int i = 1; i < total; ++i) {
            const auto& it = Game::ItemRegistry::Get(static_cast<Game::ItemID>(i));
            if (it.specialKind == "bed") {
                GuiGraphics::RegisterCustomItemRenderer(static_cast<Game::ItemID>(i), &RenderBedInventory);
            }
        }
    }

} // namespace Render
