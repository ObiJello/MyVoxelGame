// File: src/client/renderer/viewmodel/HeldItemRenderer.cpp
#include "HeldItemRenderer.hpp"
#include "HeldItemSpriteMesh.hpp"

#include "../backend/RenderBackend.hpp"
#include "../core/Vertex.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockModel.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "../blockentity/BlockEntityRenderer.hpp"
#include "../blockentity/BlockEntityRenderDispatcher.hpp"
#include "common/core/Log.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <cmath>

namespace Render {

    HeldItemRenderer g_heldItemRenderer;

    namespace {
        // ── Animation timing (in game ticks; one tick = 50ms / 20Hz). ──
        // Equip swap: pushes the displayed item down, swaps when fully
        // hidden, then brings the new item up. Step value matches the
        // vanilla 0.4-per-tick rate so a full hide-and-show takes ~5
        // ticks (~250ms).
        constexpr float kEquipStep = 0.4f;
        // Swing animation runs over 6 ticks (300 ms) — `attack` ramps
        // 0 → 1 across that span; vanilla speed.
        constexpr int kSwingTicks = 6;
        constexpr float kSwingStep = 1.0f / (float)kSwingTicks;

        // ── Per-item display transforms ─────────────────────────────
        // Values match the FIRST_PERSON_RIGHT_HAND transform from
        // Minecraft's vanilla item-model parents (item/generated,
        // item/handheld, block/block). Translations are in *block
        // units* (the model space the cube/sprite mesh lives in is
        // 16 units across, so divide JSON-space pixels by 16 here).
        struct DisplayXf {
            glm::vec3 rotationDeg;
            glm::vec3 translation;   // already in block units
            float     scale;
        };
        // item/generated and item/handheld both inherit the same
        // firstperson_righthand transform in vanilla — rotation
        // [0, -90, 25], translation [1.13, 3.2, 1.13]/16, scale 0.68.
        // Tools don't get a special tilt; the visual difference between
        // a sword and a stick comes entirely from the extruded sprite,
        // not the display transform. So one constant covers both.
        constexpr DisplayXf kSpriteXf {
            { 0.0f, -90.0f, 25.0f },
            { 1.13f / 16.0f, 3.2f / 16.0f, 1.13f / 16.0f },
            0.68f
        };
        // block/block parent firstperson_righthand.
        constexpr DisplayXf kBlockXf {
            { 0.0f, 45.0f, 0.0f },
            { 0.0f, 2.5f / 16.0f, 0.0f },
            0.40f
        };

        // Build the 4×4 model matrix for a display transform.
        //
        // Vanilla's ItemTransform.apply does (in pose-stack order):
        //   1. translate(translation / 16)         // in block units
        //   2. rotate(Quaternionf.rotationXYZ(rx, ry, rz))
        //   3. scale(scale)
        //   4. translate(-0.5, -0.5, -0.5)         // pre-display centering
        //
        // JOML's `rotationXYZ(rx, ry, rz)` builds a quaternion equivalent
        // to matrix Rx · Ry · Rz, so when applied to a vertex it rotates
        // first around Z, then Y, then X. We replicate that ordering
        // here. Pose stacks post-multiply, so the final composed matrix
        // is `T_trans · Rx · Ry · Rz · S · T_negHalf`, which applied to a
        // vertex P yields:
        //     T_trans(Rx(Ry(Rz(S(P - 0.5)))))
        // — exactly vanilla's chain.
        glm::mat4 buildDisplayMatrix(const DisplayXf& xf) {
            glm::mat4 m(1.0f);
            m = glm::translate(m, xf.translation);
            m = glm::rotate(m, glm::radians(xf.rotationDeg.x), {1,0,0});
            m = glm::rotate(m, glm::radians(xf.rotationDeg.y), {0,1,0});
            m = glm::rotate(m, glm::radians(xf.rotationDeg.z), {0,0,1});
            m = glm::scale(m, glm::vec3(xf.scale));
            return m;
        }

        // Apply the equip-swap drift: as the item leaves/re-enters the
        // hand the whole rig slides down out of frame.
        void applyEquipTransform(glm::mat4& m, float equipProgress) {
            m = glm::translate(m, glm::vec3(0.0f, -0.6f * equipProgress, 0.0f));
        }

        // Vanilla first-person swing. Decomposes into a per-axis
        // position offset plus the four-rotation arm-attack transform.
        // `attack` is the 0..1 swing progress.
        //
        //   xPos = -0.4 * sin(sqrt(p) * pi)
        //   yPos =  0.2 * sin(sqrt(p) * 2pi)
        //   zPos = -0.2 * sin(p * pi)
        //
        // followed by an extrinsic Y → Z → X → Y rotation chain that
        // sweeps the arm through the strike arc. The Y±45° brackets
        // re-orient the item between the world-up and arm-relative
        // frames so the X/Z rotations operate on the arm's local axes.
        // Right hand → invert = +1. We don't render a left hand.
        void applySwingTransform(glm::mat4& m, float attack) {
            const float pi  = 3.14159265358979323846f;
            const float sp  = std::sqrt(attack);
            // Translation
            const float dx = -0.4f * std::sin(sp * pi);
            const float dy =  0.2f * std::sin(sp * 2.0f * pi);
            const float dz = -0.2f * std::sin(attack * pi);
            m = glm::translate(m, glm::vec3(dx, dy, dz));
            // Rotation chain
            const float ySwingRot  = std::sin(attack * attack * pi);
            const float xzSwingRot = std::sin(sp * pi);
            m = glm::rotate(m, glm::radians(45.0f + ySwingRot * -20.0f), {0,1,0});
            m = glm::rotate(m, glm::radians(xzSwingRot * -20.0f),         {0,0,1});
            m = glm::rotate(m, glm::radians(xzSwingRot * -80.0f),         {1,0,0});
            m = glm::rotate(m, glm::radians(-45.0f),                       {0,1,0});
        }

        // Walk-bob: small periodic offsets driven by the player's
        // accumulated walked distance. Matches the vanilla "bobView"
        // behaviour at full strength (no settings toggle here).
        void applyBobTransform(glm::mat4& m, float walkDistance) {
            const float pi      = 3.14159265358979323846f;
            const float strength = 0.10f;            // amplitude
            const float dxSign   = std::sin(walkDistance * pi);
            const float dx = dxSign * 0.5f * strength;
            const float dy = -std::abs(std::cos(walkDistance * pi)) * strength;
            m = glm::translate(m, glm::vec3(dx, dy, 0.0f));
            m = glm::rotate(m, glm::radians(dxSign * 3.0f), {0,0,1});
            m = glm::rotate(m,
                glm::radians(std::abs(std::cos(walkDistance * pi) * 5.0f)),
                {1,0,0});
        }

        // ── 1×1 textured-cube mesh shared by every block item ──
        // Rebuilt per-frame as one of our scratch buffers so we can
        // re-UV each face from the relevant block's atlas entry without
        // baking N separate meshes. Lives in [0,1]³ local space.
        struct CubeVert { float x,y,z; float u,v; uint8_t r,g,b,a; };
        static_assert(sizeof(CubeVert) == 24, "CubeVert must be 24 bytes");

        // Append one cube face quad (4 verts + 6 indices) to the scratch
        // buffer. positions/UVs supplied in CCW order viewed from
        // outside the face so backface culling keeps the visible side.
        // Per-vertex tint colour multiplies the sampled texture in the
        // block fragment shader (`texColor * vertColor`); used for
        // grass-top biome colouring.
        void appendCubeFace(std::vector<CubeVert>& verts,
                            std::vector<uint32_t>& idx,
                            const glm::vec3 p[4], const glm::vec2 uv[4],
                            uint8_t r, uint8_t g, uint8_t b) {
            uint32_t base = (uint32_t)verts.size();
            for (int i = 0; i < 4; ++i) {
                verts.push_back({ p[i].x, p[i].y, p[i].z,
                                  uv[i].x, uv[i].y,
                                  r, g, b, 255 });
            }
            idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
            idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
        }

        // Default biome-tint colour applied to faces with tintindex=0
        // (grass top, vine, lily pad, …). Held items have no biome
        // context so we use the vanilla "default" grass/foliage colour
        // — the value sampled from the centre of grass.png at the
        // default plains-biome coords. Without this, grass tops render
        // grey because the texture itself is greyscale.
        constexpr uint8_t kDefaultGrassR = 124;
        constexpr uint8_t kDefaultGrassG = 189;
        constexpr uint8_t kDefaultGrassB = 107;

        // Per-element resolved face texture + tint. We collect one of
        // these per (element × face direction) so multi-layer cube
        // models (grass_block has a base cube + an overlay cube)
        // emit one quad per layer at the same world position; alpha
        // blending composites them correctly.
        struct FaceLayer {
            float u0, v0, u1, v1;
            int   tintIndex;
        };

        // Walk EVERY element of the model that defines this face and
        // resolve each one to an atlas UV + tint. Returns the layers
        // in element order (base first, overlays next) so emission can
        // draw them in that sequence — the overlay's transparent
        // pixels blend over the base.
        void collectFaceLayers(const Game::BlockModel& bm, Game::FaceDir dir,
                               std::vector<FaceLayer>& out) {
            for (const auto& el : bm.elements) {
                auto faceIt = el.faces.find(dir);
                if (faceIt == el.faces.end()) continue;
                const std::string atlasKey = bm.ResolveTexture(faceIt->second.textureRef);
                if (atlasKey.empty() || atlasKey == "missingno") continue;
                AtlasUVRect rect;
                if (!g_atlasBuilder || !g_atlasBuilder->GetUVRect(atlasKey, rect)) continue;
                out.push_back({ rect.uvMin.x, rect.uvMin.y,
                                rect.uvMax.x, rect.uvMax.y,
                                faceIt->second.tintIndex });
            }
        }

        // Resolves a tintindex value to the per-vertex colour to write
        // for that face. -1 (no tint) = white; 0 (biome tint) = the
        // default grass/foliage colour above; higher indices are reserved
        // for other items (banner, dye) and aren't relevant for blocks.
        void tintToColour(int tintIndex, uint8_t& r, uint8_t& g, uint8_t& b) {
            if (tintIndex == 0) {
                r = kDefaultGrassR;
                g = kDefaultGrassG;
                b = kDefaultGrassB;
            } else {
                r = g = b = 255;
            }
        }

        // Fallback: try a couple of typical texture-key patterns for
        // the block's model name. Used when the BlockModelRegistry
        // doesn't have the model loaded (unloaded model, custom block,
        // etc.) — produces a single-texture cube. Returns true if any
        // pattern hits.
        bool fallbackBlockUV(const std::string& modelName,
                             float& u0, float& v0, float& u1, float& v1) {
            if (!g_atlasBuilder || modelName.empty()) return false;
            const std::string keys[] = {
                "block/" + modelName,
                modelName,
            };
            AtlasUVRect rect;
            for (const auto& k : keys) {
                if (g_atlasBuilder->GetUVRect(k, rect)) {
                    u0 = rect.uvMin.x; v0 = rect.uvMin.y;
                    u1 = rect.uvMax.x; v1 = rect.uvMax.y;
                    return true;
                }
            }
            return false;
        }

        // Build the 6-face cube mesh on the fly. Each face is UV'd
        // independently from the block model's per-face texture so
        // multi-textured blocks (grass, oak log, furnace, …) show the
        // right face on the right side. Caller owns the scratch
        // vertex/index buffers.
        //
        // Winding convention: vertices listed counter-clockwise when
        // viewed from the OUTSIDE of the face — matches our pipeline
        // state (CullMode::Back, FrontFace::CounterClockwise), so the
        // outward-facing side is what survives backface culling.
        //
        // Cube occupies [0, 1]³ in local space; the renderer recentres
        // around the cube's mid-point (0.5, 0.5, 0.5) before applying
        // the display transform so rotations pivot at the block centre.
        void buildBlockCubeMesh(Game::BlockID b,
                                std::vector<CubeVert>& verts,
                                std::vector<uint32_t>& idx) {
            verts.clear();
            idx.clear();

            const auto& blk  = Game::BlockRegistry::Get(b);
            const auto* bmPtr = &Game::BlockModelRegistry::GetModel(blk.modelName);
            const bool hasModel = bmPtr && !bmPtr->elements.empty();

            // 8 corners of the unit cube.
            const glm::vec3 v000{0,0,0}, v100{1,0,0}, v110{1,1,0}, v010{0,1,0};
            const glm::vec3 v001{0,0,1}, v101{1,0,1}, v111{1,1,1}, v011{0,1,1};

            // Per-face emit helper. For multi-layer cube models
            // (grass_block has a base cube + an overlay cube at the
            // same position), we walk EVERY element that defines this
            // face and emit one quad per layer. The block path uses
            // depth-test LessOrEqual so coplanar overlay quads aren't
            // z-rejected after the base writes depth; the alpha-blend
            // pipeline composites them in the order we emit (base
            // first, overlay second). Single-element models hit this
            // path with a layer count of 1 and behave exactly as before.
            auto emit = [&](Game::FaceDir dir,
                            const glm::vec3 q[4], const glm::vec2 uv[4]) {
                std::vector<FaceLayer> layers;
                if (hasModel) collectFaceLayers(*bmPtr, dir, layers);
                if (layers.empty()) {
                    // Fallback when the model has no entry for this
                    // face (or no model at all): try model-name keys,
                    // failing that draw with the full atlas range so
                    // the cube is at least visible.
                    FaceLayer fb{0,0,1,1,-1};
                    fallbackBlockUV(blk.modelName, fb.u0, fb.v0, fb.u1, fb.v1);
                    layers.push_back(fb);
                }
                for (const FaceLayer& L : layers) {
                    uint8_t r, g, b;
                    tintToColour(L.tintIndex, r, g, b);
                    glm::vec2 rmap[4];
                    for (int i = 0; i < 4; ++i) {
                        rmap[i].x = L.u0 + uv[i].x * (L.u1 - L.u0);
                        rmap[i].y = L.v0 + uv[i].y * (L.v1 - L.v0);
                    }
                    appendCubeFace(verts, idx, q, rmap, r, g, b);
                }
            };

            // Template UVs: corners in (s, t) ∈ [0,1] expressing where on
            // the resolved atlas rect each corner samples. The chosen
            // mapping puts the texture's top-left at the face's natural
            // top-left for each side (matches vanilla face UV defaults).

            // +Y top
            { glm::vec3 q[4] = {v010, v011, v111, v110};
              glm::vec2 uv[4] = {{0,0},{0,1},{1,1},{1,0}};
              emit(Game::FaceDir::Up, q, uv); }
            // -Y bottom
            { glm::vec3 q[4] = {v000, v100, v101, v001};
              glm::vec2 uv[4] = {{0,0},{1,0},{1,1},{0,1}};
              emit(Game::FaceDir::Down, q, uv); }
            // +Z front (south in MC convention)
            { glm::vec3 q[4] = {v001, v101, v111, v011};
              glm::vec2 uv[4] = {{0,1},{1,1},{1,0},{0,0}};
              emit(Game::FaceDir::South, q, uv); }
            // -Z back (north)
            { glm::vec3 q[4] = {v000, v010, v110, v100};
              glm::vec2 uv[4] = {{1,1},{1,0},{0,0},{0,1}};
              emit(Game::FaceDir::North, q, uv); }
            // +X right (east)
            { glm::vec3 q[4] = {v100, v110, v111, v101};
              glm::vec2 uv[4] = {{1,1},{1,0},{0,0},{0,1}};
              emit(Game::FaceDir::East, q, uv); }
            // -X left (west)
            { glm::vec3 q[4] = {v000, v001, v011, v010};
              glm::vec2 uv[4] = {{0,1},{1,1},{1,0},{0,0}};
              emit(Game::FaceDir::West, q, uv); }
        }

        // Scratch GPU buffers reused for the block cube path. Allocated
        // once (24 verts capacity is fixed) and re-uploaded each frame
        // a block item is rendered.
        BufferHandle s_cubeVB    = INVALID_BUFFER;
        BufferHandle s_cubeIB    = INVALID_BUFFER;
        MeshHandle   s_cubeMesh  = INVALID_MESH;
        // Sized for the worst-case multi-element cube. grass_block has
        // 6 + 4 = 10 quads; some pumpkin/jack o'lantern variants reach
        // similar counts. Bumped to 32 quads (128 verts / 192 indices)
        // for headroom — these buffers are streaming and only one cube
        // is uploaded per frame, so the extra allocation is negligible.
        constexpr uint32_t kCubeMaxQuads = 32;
        constexpr uint32_t kCubeMaxVerts = kCubeMaxQuads * 4;
        constexpr uint32_t kCubeMaxIdx   = kCubeMaxQuads * 6;
    } // namespace

    // ──────────────────────────────────────────────────────────────
    bool HeldItemRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // Reuse the existing block opaque shader — it samples a single
        // 2D texture with vertex-colour multiply and supports alpha
        // testing, which is exactly the requirements for both the
        // voxelised sprite and the textured cube paths.
        m_shader = g_renderBackend->CreateShaderFromFiles(
            "shaders/block.vert", "shaders/block.frag");
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[HeldItemRenderer] failed to load block shader — "
                         "held items will not render");
            return false;
        }

        // 1×1 white fallback so the descriptor binding is satisfied
        // when an item's texture lookup fails.
        unsigned char white[4] = { 255, 255, 255, 255 };
        m_dummyTexture = g_renderBackend->CreateTexture2D(
            1, 1, TextureFormat::RGBA8, white);

        // Pre-allocate the block-cube scratch buffers.
        s_cubeVB = g_renderBackend->CreateBuffer(
            BufferUsage::Vertex, kCubeMaxVerts * sizeof(CubeVert),
            nullptr, BufferAccess::Streaming);
        s_cubeIB = g_renderBackend->CreateBuffer(
            BufferUsage::Index,  kCubeMaxIdx  * sizeof(uint32_t),
            nullptr, BufferAccess::Streaming);
        s_cubeMesh = g_renderBackend->CreateMesh(
            s_cubeVB, s_cubeIB, GetBlockVertexLayout());

        m_initialized = true;
        Log::Info("[HeldItemRenderer] initialized");
        return true;
    }

    void HeldItemRenderer::Shutdown() {
        if (!g_renderBackend) return;
        HeldItemSpriteMesh::ClearCache();
        if (s_cubeMesh != INVALID_MESH)  { g_renderBackend->DestroyMesh(s_cubeMesh); s_cubeMesh = INVALID_MESH; }
        if (s_cubeVB   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(s_cubeVB); s_cubeVB = INVALID_BUFFER; }
        if (s_cubeIB   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(s_cubeIB); s_cubeIB = INVALID_BUFFER; }
        if (m_dummyTexture != INVALID_TEXTURE) {
            g_renderBackend->DestroyTexture(m_dummyTexture);
            m_dummyTexture = INVALID_TEXTURE;
        }
        if (m_shader != INVALID_SHADER) {
            g_renderBackend->DestroyShader(m_shader);
            m_shader = INVALID_SHADER;
        }
        m_initialized = false;
    }

    void HeldItemRenderer::Tick(Game::ItemID selectedItem,
                                bool attackPressedThisTick) {
        // First tick of the game: skip the equip slide-in. Otherwise
        // the player sees the very first item they spawn holding slide
        // up into the hand position, which looks like a bug.
        static bool s_firstTick = true;
        if (s_firstTick) {
            m_displayedItem = selectedItem;
            m_pendingItem   = selectedItem;
            m_equipProgress = m_equipProgressPrev = 0.0f;
            m_swingProgress = m_swingProgressPrev = 0.0f;
            s_firstTick = false;
            return;
        }

        m_pendingItem = selectedItem;
        m_equipProgressPrev = m_equipProgress;
        m_swingProgressPrev = m_swingProgress;

        // Equip animation state machine. Vanilla pattern:
        //   • equipProgress lerps toward a target by up to kEquipStep
        //     per tick (target = 1 when items differ → item hides;
        //     target = 0 when same → item slides back up).
        //   • When the item is mostly hidden (progress > 0.9 → the
        //     equivalent of vanilla's mainHandHeight < 0.1) snap the
        //     displayed item to the pending one. This is the point
        //     where the swap is visually invisible.
        const float target = (m_displayedItem != m_pendingItem) ? 1.0f : 0.0f;
        const float delta  = std::clamp(target - m_equipProgress,
                                        -kEquipStep, kEquipStep);
        m_equipProgress = std::clamp(m_equipProgress + delta, 0.0f, 1.0f);
        if (m_equipProgress > 0.9f) {
            m_displayedItem = m_pendingItem;
        }

        // Swing animation. Mirrors MC's LivingEntity.swing() retrigger
        // gate: a new attack restarts the animation IF the previous swing
        // is past halfway (swingTime >= getCurrentSwingDuration()/2). MC's
        // Minecraft.continueAttack calls player.swing() every frame during
        // continuous mining and lets this internal gate decide the rate;
        // the result is a swing every ~half-duration (3 ticks at default
        // duration=6). Spam-clicking gets the same treatment — each click
        // past the half-mark snaps to a fresh swing instead of being
        // dropped on the floor.
        const bool canRetrigger = !m_swingActive || m_swingProgress >= 0.5f;
        if (attackPressedThisTick && canRetrigger) {
            m_swingActive = true;
            m_swingProgress = 0.0f;
            m_swingProgressPrev = 0.0f;
        }
        if (m_swingActive) {
            m_swingProgress += kSwingStep;
            if (m_swingProgress >= 1.0f) {
                m_swingActive = false;
                m_swingProgress = 0.0f;
            }
        }
    }

    void HeldItemRenderer::Render(float aspect, float partialTick,
                                  float walkDistance) {
        if (!m_initialized || !g_renderBackend) return;
        if (m_displayedItem == 0) return;          // air → render nothing

        // MC parity: clear the depth buffer before rendering the viewmodel
        // so it never gets occluded by world geometry pressed up against
        // the camera (walls, low ceilings). Mirrors MC's
        // GameRenderer.renderItemInHand path which runs after
        // `RenderSystem.clear(GL_DEPTH_BUFFER_BIT, ...)`. The viewmodel
        // then z-sorts only against itself.
        g_renderBackend->Clear(/*color=*/false, /*depth=*/true, /*stencil=*/false);

        // Look up the item once; bail if it doesn't exist.
        const auto& item = Game::ItemRegistry::Get(m_displayedItem);

        // ── Pick the mesh + texture for this item type ──────────────
        DisplayXf       xf       = kSpriteXf;
        MeshHandle      mesh     = INVALID_MESH;
        uint32_t        indexCount = 0;
        TextureHandle   tex      = m_dummyTexture;
        bool            useAtlas = false;          // block path: bind atlas

        // ── BEWLR shortcut ─────────────────────────────────────────
        // If the held item is a block backed by a BlockEntity that has a
        // dedicated renderer (chest, sign, banner, …), short-circuit the
        // normal cube path: bake the held-item display transform into a
        // model matrix and hand off to the BER's RenderBEWLR. Mirrors MC's
        // BlockEntityWithoutLevelRenderer.renderByItem dispatch.
        const Game::BlockEntityType* beType =
            (item.renderType == Game::ItemRenderType::Block && item.blockId != Game::BlockID::Air)
                ? Game::BlockEntityTypes::ForBlock(item.blockId) : nullptr;
        Render::BlockEntityRenderer* beRenderer =
            (beType && Render::g_blockEntityRenderDispatcher)
                ? Render::g_blockEntityRenderDispatcher->GetRenderer(beType->TypeId())
                : nullptr;
        if (beRenderer) {
            // Build the same hand transform chain as the cube path, then
            // hand to the BE renderer with the MC-pixel-space mesh
            // recentred to the cell midpoint.
            const float equipBE = m_equipProgressPrev +
                (m_equipProgress - m_equipProgressPrev) * partialTick;
            const float swingBE = m_swingActive
                ? m_swingProgressPrev + (m_swingProgress - m_swingProgressPrev) * partialTick
                : 0.0f;
            glm::mat4 modelBE(1.0f);
            modelBE = glm::translate(modelBE, glm::vec3(0.56f, -0.52f, -0.72f));
            applyEquipTransform(modelBE, equipBE);
            applyBobTransform  (modelBE, walkDistance);
            if (m_swingActive || swingBE > 0.0f) applySwingTransform(modelBE, swingBE);
            modelBE *= buildDisplayMatrix(kBlockXf);
            // Chest mesh lives in MC pixel-space [0,16]³. Scale to [0,1]
            // block space, then translate to centre the mesh on origin so
            // the display transform's rotation pivots around the chest's
            // own centre rather than its corner.
            modelBE = glm::scale(modelBE, glm::vec3(1.0f / 16.0f));
            modelBE = glm::translate(modelBE, glm::vec3(-8.0f, -8.0f, -8.0f));
            const float fovYBE = glm::radians(70.0f);
            const glm::mat4 projBE = glm::perspective(fovYBE, aspect, 0.05f, 8.0f);
            beRenderer->RenderBEWLR(item.blockId, projBE * modelBE);
            return;
        }

        if (item.renderType == Game::ItemRenderType::Block) {
            // Build a 1×1 textured-cube mesh, atlas UVs sampled from
            // the block's representative texture.
            std::vector<CubeVert> verts;
            std::vector<uint32_t> idx;
            verts.reserve(kCubeMaxVerts);
            idx.reserve(kCubeMaxIdx);
            buildBlockCubeMesh(item.blockId, verts, idx);
            g_renderBackend->UpdateBuffer(s_cubeVB, 0,
                verts.size() * sizeof(CubeVert), verts.data());
            g_renderBackend->UpdateBuffer(s_cubeIB, 0,
                idx.size() * sizeof(uint32_t), idx.data());
            mesh        = s_cubeMesh;
            indexCount  = (uint32_t)idx.size();
            xf          = kBlockXf;
            // Block path uses the atlas (matches how chunks render).
            if (g_atlasBuilder) {
                tex = g_atlasBuilder->GetBackendTextureHandle();
                useAtlas = true;
            }
        } else {
            // Sprite path: voxelised extrusion from the sprite PNG.
            // Items can have either a layer0 (single-frame) or a
            // selectable sprite (compass/clock); fall back to layer0
            // for now — the dynamic frame selectors are GUI-only.
            std::string name = item.spriteName;
            if (name.empty() && !item.spriteFrames.empty()) name = item.spriteFrames[0];
            if (name.empty() && !item.spriteLayers.empty()) name = item.spriteLayers[0];
            if (name.empty()) return;

            const auto* entry = HeldItemSpriteMesh::GetOrBuild(name);
            if (!entry) return;
            mesh       = entry->mesh;
            indexCount = entry->indexCount;
            tex        = entry->texture != INVALID_TEXTURE ? entry->texture
                                                           : m_dummyTexture;
            xf = kSpriteXf;
            // Sprite mesh lives in 0..16 local space; the display
            // transform's translation is in 0..1 block units, so we
            // need to pre-scale the mesh to fit in the same unit cube
            // as the block path. Done by composing an extra 1/16 scale
            // BEFORE the display transform below.
        }
        if (mesh == INVALID_MESH) return;

        // ── Build the model matrix ─────────────────────────────────
        // Step order (extrinsic — read bottom-up):
        //   1. Base hand position (lower-right of the view)
        //   2. Equip slide (off-screen by equip progress)
        //   3. View bob (walk-driven drift)
        //   4. Swing animation
        //   5. Display transform (per-item)
        //   6. Sprite/Block local-space normalisation
        //
        // partialTick blends the previous and current tick samples so
        // animation is smooth at any framerate.
        const float equip = m_equipProgressPrev +
            (m_equipProgress - m_equipProgressPrev) * partialTick;
        const float swingP = m_swingActive
            ? m_swingProgressPrev + (m_swingProgress - m_swingProgressPrev) * partialTick
            : 0.0f;

        glm::mat4 model(1.0f);
        // 1) Base hand position. Lower-right of the view, slightly
        //    forward of the camera. Y is negative (below eye), X
        //    positive (to right), Z negative (in front).
        model = glm::translate(model, glm::vec3(0.56f, -0.52f, -0.72f));
        // 2) Equip slide
        applyEquipTransform(model, equip);
        // 3) Walk bob
        applyBobTransform(model, walkDistance);
        // 4) Swing arc
        if (m_swingActive || swingP > 0.0f) {
            applySwingTransform(model, swingP);
        }
        // 5) Display transform
        model *= buildDisplayMatrix(xf);
        // 6) Convert mesh-local space into the same coordinate frame
        //    vanilla's display transform expects, and apply vanilla's
        //    pre-display recentre.
        //
        //    Vanilla's ItemTransform.apply ends with
        //    `translate(-0.5, -0.5, -0.5)`. Pose stacks post-multiply,
        //    so that translate appears LAST in the composed matrix —
        //    which means it's the FIRST thing applied to the vertex.
        //    Net effect: vanilla treats the mesh as living in a unit
        //    cube [0,1]³ centred around the (0.5, 0.5, 0.5) point,
        //    and recentres it around origin BEFORE the rotate/scale.
        //
        //    Block path: our cube already lives in [0,1]³ → translate
        //    by (-0.5, -0.5, -0.5) matches vanilla exactly.
        //
        //    Sprite path: our voxelised mesh lives in
        //    [0,16]² × [-0.5, +0.5] (X/Y in pixel units; Z is already
        //    centred on 0 since we built a single slab around z=0
        //    rather than vanilla's z=[7.5, 8.5] convention). So:
        //      • scale(1/16) converts pixel-units → block-units
        //      • translate(-8, -8, 0) in PRE-scale pixel units shifts
        //        X/Y centre to origin (equivalent to (-0.5, -0.5, 0)
        //        in post-scale block units)
        //      • Z component of recentre is 0 because the mesh's Z is
        //        already centred on origin — applying -0.5 there would
        //        push the slab half a block forward (wrong direction).
        if (item.renderType != Game::ItemRenderType::Block) {
            model = glm::scale(model, glm::vec3(1.0f / 16.0f));
            model = glm::translate(model, glm::vec3(-8.0f, -8.0f, 0.0f));
        } else {
            model = glm::translate(model, glm::vec3(-0.5f, -0.5f, -0.5f));
        }

        // ── Projection: a separate, narrower viewmodel projection so
        //    the held item stays the same on-screen size regardless of
        //    world FOV. 70° is the vanilla viewmodel FOV.
        const float fovY = glm::radians(70.0f);
        const glm::mat4 proj = glm::perspective(fovY, aspect, 0.05f, 8.0f);
        const glm::mat4 mvp  = proj * model;

        // ── Pipeline + draw ────────────────────────────────────────
        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = true;
        state.colorWriteEnabled = true;
        // LessOrEqual on the cube path so coplanar overlay quads
        // (grass_block_side_overlay, melon_stem overlays, etc.) pass
        // the depth test against the base quad they sit on top of.
        // Sprite path is single-layer so Less is fine either way; we
        // unify to LessOrEqual for simplicity.
        state.depthCompareOp = CompareOp::LessEqual;
        // Alpha blending always on: the cube path needs it for
        // translucent blocks (glass, ice, stained glass) and it's a
        // no-op for opaque blocks since every texel has α=255. The
        // sprite path uses the alpha-discard threshold below to drop
        // fully-transparent corners of items so blending doesn't
        // smear background through the would-be-empty pixels.
        state.blendEnabled    = true;
        state.srcBlendFactor  = BlendFactor::SrcAlpha;
        state.dstBlendFactor  = BlendFactor::OneMinusSrcAlpha;
        // Cull-mode depends on item type:
        //   • Block path: standard back-face culling (the cube is
        //     closed, no need to draw inside faces).
        //   • Sprite path: NO culling. The voxelised mesh emits front,
        //     back, and side faces with outward normals — but after the
        //     display Y-rotation -90° the sprite plane swings around
        //     and which side is "outward" depends on the camera angle.
        //     Disabling cull means both faces always draw; the small
        //     overdraw cost is negligible for a single small mesh.
        state.cullMode  = (item.renderType == Game::ItemRenderType::Block)
                              ? CullMode::Back : CullMode::None;
        state.frontFace = FrontFace::CounterClockwise;
        state.primitiveType = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);

        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(tex, 0);
        g_renderBackend->SetUniformMat4 (m_shader, "uMVP", mvp);
        // Cube path: tiny threshold (0.01) drops fully-transparent
        // texels so glass etc. doesn't get a faint outline; the rest
        // of the alpha range goes through the blend equation above.
        // Sprite path: 0.5 (matches vanilla cutout) — discards the
        // transparent halo around items like swords without softening
        // their silhouette via blending.
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest",
            useAtlas ? 0.01f : 0.5f);
        // Clear any portal-plane clipping inherited from the see-through
        // pass — held items render after world, never inside a portal.
        g_renderBackend->SetUniformVec4 (m_shader, "uPortalClipPlane",
            glm::vec4(0.0f));

        g_renderBackend->DrawIndexed(mesh, indexCount);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
