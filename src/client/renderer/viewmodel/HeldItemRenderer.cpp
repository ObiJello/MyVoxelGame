// File: src/client/renderer/viewmodel/HeldItemRenderer.cpp
#include "client/entity/ClientFishing.hpp"
#include "common/entity/FireworkItems.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "HeldItemRenderer.hpp"
#include "HeldItemSpriteMesh.hpp"

#include "../backend/RenderBackend.hpp"
#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif
#include "../environment/EnvironmentState.hpp"
#include "../environment/EntityEnvironment.hpp"
#include "../core/Vertex.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockModel.hpp"
#include "ItemMeshBuilder.hpp"
#include "common/world/block/entity/BlockEntityType.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "../blockentity/BlockEntityRenderer.hpp"
#include "../blockentity/BlockEntityRenderDispatcher.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "client/map/ClientMaps.hpp"
#include "../entity/model/ModelPart.hpp"
#include "../entity/ItemDisplayTransforms.hpp"
#include "common/core/Ease.hpp"
#include "common/entity/SpearItem.hpp"

#include "stb_image.h"

#include <filesystem>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <cmath>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

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
        // item/handheld_rod's firstperson_righthand — the fishing rod and
        // the two on-a-stick items: rotation [0, 90, 25], translation
        // [0, 1.6, 0.8], scale 0.68. It turns the rod the other way round from
        // item/handheld, so the tip leads — where the fishing line leaves it
        // (FishingHookRenderer's first-person origin assumes this pose).
        constexpr DisplayXf kRodXf {
            { 0.0f, 90.0f, 25.0f },
            { 0.0f, 1.6f / 16.0f, 0.8f / 16.0f },
            0.68f
        };
        // item/crossbow's own firstperson_righthand: rotation [-90, 0, -55],
        // translation [1.13, 3.2, 1.13], scale 0.68 (every crossbow model —
        // standby, pulling, loaded — takes it from this parent).
        constexpr DisplayXf kCrossbowXf {
            { -90.0f, 0.0f, -55.0f },
            { 1.13f / 16.0f, 3.2f / 16.0f, 1.13f / 16.0f },
            0.68f
        };
        bool IsHandheldRod(Game::ItemID id) {
            return id == Game::Items::FishingRod || id == Game::Items::CarrotOnAStick ||
                   id == Game::Items::WarpedFungusOnAStick;
        }
        // block/block parent firstperson_righthand:
        //   rotation [0, 45, 0], translation [0, 0, 0], scale [0.4, 0.4, 0.4].
        //
        // The translation was 2.5/16, which is block/block's THIRDPERSON value
        // ([0, 2.5, 0]) — every held block sat two and a half pixels high. It
        // only became noticeable once the hand started drawing real model
        // geometry, because a full cube looks much the same either way but a
        // button does not.
        constexpr DisplayXf kBlockXf {
            { 0.0f, 45.0f, 0.0f },
            { 0.0f, 0.0f, 0.0f },
            0.40f
        };
        // item/template_chest and item/template_shulker_box —
        // firstperson_righthand: rotation [0, 315, 0], translation [0, 0, 0],
        // scale [0.4, 0.4, 0.4]. Both templates are identical, so one constant
        // covers the chest family and the shulker family.
        //
        // These blocks do NOT inherit block/block: they are `special` items
        // whose model is drawn by a renderer, and MC gives them their own
        // display transforms. Reusing kBlockXf turned the chest 90 degrees (45
        // vs 315) so it presented its side to the camera instead of its front,
        // and lifted it 2.5px (kBlockXf carries block/block's THIRDPERSON
        // translation, not its firstperson [0,0,0]).
        constexpr DisplayXf kChestLikeXf {
            { 0.0f, 315.0f, 0.0f },
            { 0.0f, 0.0f, 0.0f },
            0.40f
        };

        // models/item/decorated_pot.json firstperson_righthand: rotation
        // [0, 90, 0], translation [0, 0, 0], scale 0.375.
        constexpr DisplayXf kDecoratedPotXf {
            { 0.0f, 90.0f, 0.0f },
            { 0.0f, 0.0f, 0.0f },
            0.375f
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
        //
        // leftHand mirrors like MC ItemTransform.apply(true): negate
        // translation.x and the Y/Z rotations (FIRST_PERSON_LEFT_HAND).
        glm::mat4 buildDisplayMatrix(const DisplayXf& xf, bool leftHand) {
            const float mirror = leftHand ? -1.0f : 1.0f;
            glm::mat4 m(1.0f);
            m = glm::translate(m, glm::vec3(xf.translation.x * mirror,
                                            xf.translation.y,
                                            xf.translation.z));
            m = glm::rotate(m, glm::radians(xf.rotationDeg.x),          {1,0,0});
            m = glm::rotate(m, glm::radians(xf.rotationDeg.y * mirror), {0,1,0});
            m = glm::rotate(m, glm::radians(xf.rotationDeg.z * mirror), {0,0,1});
            m = glm::scale(m, glm::vec3(xf.scale));
            return m;
        }

        // Base arm position + equip slide — MC applyItemArmTransform
        // (ItemInHandRenderer.java:313):
        //   translate(invert * 0.56, -0.52 + equippedProgress * -0.6, -0.72)
        // `invert` is +1 for the right (main) hand, -1 for the left
        // (offhand) — the offhand is a pure X mirror.
        void applyItemArmTransform(glm::mat4& m, float invert, float equipProgress) {
            m = glm::translate(m, glm::vec3(invert * 0.56f,
                                            -0.52f + equipProgress * -0.6f,
                                            -0.72f));
        }

        // Vanilla first-person swing POSITION offsets — applied BEFORE the
        // arm transform (renderArmWithItem's default branch):
        //   xPos = -0.4 * sin(sqrt(p) * pi)      (× invert)
        //   yPos =  0.2 * sin(sqrt(p) * 2pi)
        //   zPos = -0.2 * sin(p * pi)
        void applySwingOffsets(glm::mat4& m, float invert, float attack) {
            const float pi = 3.14159265358979323846f;
            const float sp = std::sqrt(attack);
            const float dx = -0.4f * std::sin(sp * pi);
            const float dy =  0.2f * std::sin(sp * 2.0f * pi);
            const float dz = -0.2f * std::sin(attack * pi);
            m = glm::translate(m, glm::vec3(invert * dx, dy, dz));
        }

        // The four-rotation arm-attack chain — MC applyItemArmAttackTransform
        // (ItemInHandRenderer.java:303), applied AFTER the arm transform.
        // The Y±45° brackets re-orient the item between the world-up and
        // arm-relative frames so the X/Z rotations operate on the arm's
        // local axes.
        void applySwingRotations(glm::mat4& m, float invert, float attack) {
            const float pi = 3.14159265358979323846f;
            const float ySwingRot  = std::sin(attack * attack * pi);
            const float xzSwingRot = std::sin(std::sqrt(attack) * pi);
            m = glm::rotate(m, glm::radians(invert * (45.0f + ySwingRot * -20.0f)), {0,1,0});
            m = glm::rotate(m, glm::radians(invert * xzSwingRot * -20.0f),          {0,0,1});
            m = glm::rotate(m, glm::radians(xzSwingRot * -80.0f),                   {1,0,0});
            m = glm::rotate(m, glm::radians(invert * -45.0f),                       {0,1,0});
        }

        // Eat/drink pull-to-mouth — MC applyEatTransform
        // (ItemInHandRenderer.java:261-275). CRITICAL ordering note: MC
        // applies this BEFORE applyItemArmTransform, so the ±90° Y swing
        // and the 0.6/-0.5 translate happen in VIEW space, pivoting the
        // whole hand toward the mouth. Composing it after the arm
        // transform (the old code) pivots around the item's own anchor
        // instead — that's what shoved the food off screen.
        void applyEatTransform(glm::mat4& m, float invert,
                               float remaining, float duration, float partialTick) {
            const float pi = 3.14159265358979323846f;
            const float currUsageTime = remaining - partialTick + 1.0f;         // :262
            const float scaledUsage   = duration > 0.0f
                ? currUsageTime / duration : 0.0f;                              // :263
            if (scaledUsage < 0.8f) {                                           // :264
                // Chew bob — |cos(t/4·π)|·0.1 on Y for the last 80% of the use
                const float extraHeight =
                    std::abs(std::cos(currUsageTime / 4.0f * pi) * 0.1f);       // :265
                m = glm::translate(m, glm::vec3(0.0f, extraHeight, 0.0f));      // :266
            }
            const float eatJiggle = 1.0f - std::pow(scaledUsage, 27.0f);        // :269
            m = glm::translate(m, glm::vec3(invert * eatJiggle * 0.6f,
                                            eatJiggle * -0.5f, 0.0f));          // :271
            m = glm::rotate(m, glm::radians(invert * eatJiggle * 90.0f), {0,1,0}); // :272
            m = glm::rotate(m, glm::radians(eatJiggle * 10.0f),          {1,0,0}); // :273
            m = glm::rotate(m, glm::radians(invert * eatJiggle * 30.0f), {0,0,1}); // :274
        }

        // BLOCK guard raise — renderArmWithItem's BLOCK case, applied AFTER
        // the arm transform (MC: applyItemArmTransform then this). MC skips
        // it for ShieldItem because the shield's arm model + "blocking"
        // predicate carry the raise there — we have no arm model, so the
        // pose applies to the shield too.
        void applyBlockPose(glm::mat4& m, float invert) {
            m = glm::translate(m, glm::vec3(invert * -0.14142136f, 0.08f, 0.14142136f));
            m = glm::rotate(m, glm::radians(-102.25f),         {1,0,0});
            m = glm::rotate(m, glm::radians(invert * 13.365f), {0,1,0});
            m = glm::rotate(m, glm::radians(invert * 78.05f),  {0,0,1});
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

        // Look-around sway — MC ItemInHandRenderer.renderHandsWithItems's two
        // opening mulPose calls, X then Y. Applied straight after the walk bob
        // so the composition matches vanilla's, where GameRenderer.bobView runs
        // on the pose stack before renderHandsWithItems touches it.
        //
        // Both angles arrive pre-multiplied by MC's 0.1 factor.
        void applySwayTransform(glm::mat4& m, float swayPitchDeg, float swayYawDeg);
    }

    void HeldItemRenderer::OnViewRewritten(float deltaPitchDeg, float deltaYawDeg) {
        if (m_firstTick) return;   // the first Tick snaps to the view anyway
        m_xBob += deltaPitchDeg;  m_xBobPrev += deltaPitchDeg;
        m_yBob += deltaYawDeg;    m_yBobPrev += deltaYawDeg;
    }

    namespace {
        void applySwayTransform(glm::mat4& m, float swayPitchDeg, float swayYawDeg) {
            m = glm::rotate(m, glm::radians(swayPitchDeg), {1, 0, 0});
            m = glm::rotate(m, glm::radians(swayYawDeg),   {0, 1, 0});
        }

        // Scratch GPU buffers reused for the block cube path. Allocated
        // once (24 verts capacity is fixed) and re-uploaded each frame
        // a block item is rendered.
        // PER HAND: both hands can hold block items in one frame, and the
        // backends map-and-copy UpdateBuffer at record time — a shared
        // buffer meant the offhand overwrote the mainhand's geometry before
        // the GPU read it (visible on Vulkan, masked by orphaning on GL).
        BufferHandle s_cubeVB[2]   = { INVALID_BUFFER, INVALID_BUFFER };
        BufferHandle s_cubeIB[2]   = { INVALID_BUFFER, INVALID_BUFFER };
        MeshHandle   s_cubeMesh[2] = { INVALID_MESH,   INVALID_MESH };

        // The map poses' geometry, per hand for the same reason: the arms,
        // the paper, the map and its decorations, one upload per hand and a
        // draw per texture. Room for both arms (with sleeves) and the
        // map's full 256-marker decoration budget with margin.
        constexpr size_t kMapMaxQuads = 320;
        BufferHandle s_mapVB[2]   = { INVALID_BUFFER, INVALID_BUFFER };
        BufferHandle s_mapIB[2]   = { INVALID_BUFFER, INVALID_BUFFER };
        MeshHandle   s_mapMesh[2] = { INVALID_MESH,   INVALID_MESH };
    } // namespace

    // ──────────────────────────────────────────────────────────────
    bool HeldItemRenderer::Initialize() {
        if (!g_renderBackend) return false;

        // Reuse the existing block opaque shader — it samples a single
        // 2D texture with vertex-colour multiply and supports alpha
        // testing, which is exactly the requirements for both the
        // voxelised sprite and the textured cube paths.
        // On Vulkan the block shaders now declare the Common UBO (set=1,
        // fog + sky-brightness fields), which requires the UBO-aware
        // (portal) pipeline layout — plain CreateShaderFromFiles bakes the
        // texture-only layout and vkCreateGraphicsPipelines fails with
        // VK_ERROR_INITIALIZATION_FAILED. Same cast pattern as
        // ChunkRenderer/SkyRenderer.
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal(
                "shaders/block.vert", "shaders/block.frag");
#endif
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles(
                "shaders/block.vert", "shaders/block.frag");
        }
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
        for (int h = 0; h < 2; ++h) {
            s_cubeVB[h] = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, kItemCubeMaxVerts * sizeof(ItemCubeVert),
                nullptr, BufferAccess::Streaming);
            s_cubeIB[h] = g_renderBackend->CreateBuffer(
                BufferUsage::Index,  kItemCubeMaxIdx  * sizeof(uint32_t),
                nullptr, BufferAccess::Streaming);
            s_cubeMesh[h] = g_renderBackend->CreateMesh(
                s_cubeVB[h], s_cubeIB[h], GetBlockVertexLayout());
            s_mapVB[h] = g_renderBackend->CreateBuffer(
                BufferUsage::Vertex, kMapMaxQuads * 4 * sizeof(ItemCubeVert),
                nullptr, BufferAccess::Streaming);
            s_mapIB[h] = g_renderBackend->CreateBuffer(
                BufferUsage::Index, kMapMaxQuads * 6 * sizeof(uint32_t),
                nullptr, BufferAccess::Streaming);
            s_mapMesh[h] = g_renderBackend->CreateMesh(
                s_mapVB[h], s_mapIB[h], GetBlockVertexLayout());
        }

        m_initialized = true;
        Log::Info("[HeldItemRenderer] initialized");
        return true;
    }

    void HeldItemRenderer::Shutdown() {
        if (!g_renderBackend) return;
        HeldItemSpriteMesh::ClearCache();
        for (int h = 0; h < 2; ++h) {
            if (s_cubeMesh[h] != INVALID_MESH)  { g_renderBackend->DestroyMesh(s_cubeMesh[h]); s_cubeMesh[h] = INVALID_MESH; }
            if (s_cubeVB[h]   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(s_cubeVB[h]); s_cubeVB[h] = INVALID_BUFFER; }
            if (s_cubeIB[h]   != INVALID_BUFFER){ g_renderBackend->DestroyBuffer(s_cubeIB[h]); s_cubeIB[h] = INVALID_BUFFER; }
            if (s_mapMesh[h] != INVALID_MESH)   { g_renderBackend->DestroyMesh(s_mapMesh[h]); s_mapMesh[h] = INVALID_MESH; }
            if (s_mapVB[h]   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(s_mapVB[h]); s_mapVB[h] = INVALID_BUFFER; }
            if (s_mapIB[h]   != INVALID_BUFFER) { g_renderBackend->DestroyBuffer(s_mapIB[h]); s_mapIB[h] = INVALID_BUFFER; }
        }
        if (m_skinTexture != INVALID_TEXTURE) {
            g_renderBackend->DestroyTexture(m_skinTexture);
            m_skinTexture = INVALID_TEXTURE;
        }
        m_skinTried = false;
        if (m_tridentTexture != INVALID_TEXTURE) {
            g_renderBackend->DestroyTexture(m_tridentTexture);
            m_tridentTexture = INVALID_TEXTURE;
        }
        m_tridentTextureTried = false;
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

    void HeldItemRenderer::Tick(Game::ItemID mainItem, Game::ItemID offhandItem,
                                bool attackPressedThisTick, float mainHandSwapScale,
                                float viewPitchDeg, float viewYawDeg) {
        PROFILE_ZONE_N("HeldItem.Tick");
        // MC LocalPlayer.applyInput: the lagging view angles advance half the
        // remaining gap each tick.
        //
        // The differences are WRAPPED, which vanilla does not need to do —
        // MC's getYRot() accumulates without bound, so crossing "south" is
        // just yaw 179 -> 181. This engine derives yaw from atan2, which wraps
        // to [-180, 180]; without the wrap a single step across that seam is a
        // 359-degree gap and the item snaps violently sideways once per
        // full turn.
        auto wrapDeg = [](float d) {
            while (d < -180.0f) d += 360.0f;
            while (d >  180.0f) d -= 360.0f;
            return d;
        };

        if (m_firstTick) {
            // Snap the lag to the current view so the item does not swing in
            // from wherever the angles happened to start.
            m_xBob = m_xBobPrev = viewPitchDeg;
            m_yBob = m_yBobPrev = viewYawDeg;
        } else {
            m_xBobPrev = m_xBob;
            m_yBobPrev = m_yBob;
            m_xBob += wrapDeg(viewPitchDeg - m_xBob) * 0.5f;
            m_yBob += wrapDeg(viewYawDeg   - m_yBob) * 0.5f;
        }

        // First tick of the game: skip the equip slide-in. Otherwise
        // the player sees the very first item they spawn holding slide
        // up into the hand position, which looks like a bug.
        if (m_firstTick) {
            for (int h = 0; h < 2; ++h) {
                const Game::ItemID it = (h == 0) ? mainItem : offhandItem;
                m_hands[h].displayed = it;
                m_hands[h].pending   = it;
                m_hands[h].mapId     = m_hands[h].pendingMapId;
                m_hands[h].stack     = m_hands[h].pendingStack;
                m_hands[h].equipProgress = m_hands[h].equipProgressPrev = 0.0f;
            }
            m_swingProgress = m_swingProgressPrev = 0.0f;
            m_firstTick = false;
            return;
        }

        m_hands[0].pending = mainItem;
        m_hands[1].pending = offhandItem;
        m_swingProgressPrev = m_swingProgress;

        // Per-hand equip animation state machine (MC keeps mainHandHeight and
        // offHandHeight separately, ItemInHandRenderer.tick:560-573):
        //   • equipProgress moves toward its target by at most kEquipStep
        //     (vanilla's 0.4) per tick — the target is 1 (fully hidden) while
        //     the hand's item differs from the one being drawn.
        //   • Once the item is mostly hidden (progress > 0.9, i.e. vanilla's
        //     mainHandHeight < 0.1) the displayed item snaps to the pending
        //     one. That is the point where the swap is visually invisible.
        //
        // Note `equipProgress` here is
        // vanilla's mainHandHeight INVERTED (1 = fully lowered), so vanilla's
        // target height maps to `1 - height`.
        //
        // The main hand's rest height is NOT 1 — it is
        // `getItemSwapScale(1.0F)` CUBED, which is what paces the raise to the
        // held weapon's own attack delay: a bare hand (5 ticks) is back up
        // almost at once, a netherite axe (20 ticks) takes a full second, and
        // the cube keeps it low for most of that and then snaps up at the end.
        // Targeting a flat 1.0, as this did, drew every item at the bare-hand
        // rate and lost the correlation with the cooldown entirely.
        for (int h = 0; h < 2; ++h) {
            HandState& hand = m_hands[h];
            hand.equipProgressPrev = hand.equipProgress;

            const float swap = std::clamp(mainHandSwapScale, 0.0f, 1.0f);
            const float restHeight = (h == 0) ? swap * swap * swap : 1.0f;
            const bool differs = hand.displayed != hand.pending || hand.mapId != hand.pendingMapId;
            const float target = differs ? 1.0f : 1.0f - restHeight;

            const float delta  = std::clamp(target - hand.equipProgress,
                                            -kEquipStep, kEquipStep);
            hand.equipProgress = std::clamp(hand.equipProgress + delta, 0.0f, 1.0f);
            if (hand.equipProgress > 0.9f) {
                hand.displayed = hand.pending;
                hand.mapId     = hand.pendingMapId;
            }
            // The same item: the live stack replaces the visible one at once
            // (its components — a crossbow's load — with no swap).
            if (hand.pendingStack.itemId == hand.displayed) hand.stack = hand.pendingStack;
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
            // ItemStack.getAttackAnimation of the main hand: a spear's STAB
            // for its attack duration, SwingAnimation.DEFAULT's 6-tick
            // WHACK for everything else.
            m_swingStab = Game::Spear::IsSpear(mainItem);
            m_swingDuration = std::max(1, Game::Spear::AttackAnimationDuration(mainItem));
        }
        if (m_swingActive) {
            m_swingProgress += m_swingStab ? 1.0f / static_cast<float>(m_swingDuration) : kSwingStep;
            if (m_swingProgress >= 1.0f) {
                m_swingActive = false;
                m_swingProgress = 0.0f;
            }
        }
    }

    glm::vec3 HeldItemRenderer::HandLight() const {
        if (!m_hasLightProbe) return glm::vec3(EntityEnvironment::Lit());
        return EntityEnvironment::LitAt(m_lightProbe);
    }

    void HeldItemRenderer::Render(float aspect, float partialTick,
                                  float walkDistance,
                                  float viewPitchDeg, float viewYawDeg,
                                  bool renderMainHand) {
        PROFILE_ZONE_N("HeldItemRender");
        if (!m_initialized || !g_renderBackend) return;
        const bool drawMain = renderMainHand && m_hands[0].displayed != 0;
        const bool drawOff  = m_hands[1].displayed != 0;
        if (!drawMain && !drawOff) return;         // both hands empty

        // MC ItemInHandRenderer.renderHandsWithItems, the first four lines:
        //   xBob = lerp(partialTick, xBobO, xBob)
        //   yBob = lerp(partialTick, yBobO, yBob)
        //   mulPose(XP.rotationDegrees((viewXRot - xBob) * 0.1F))
        //   mulPose(YP.rotationDegrees((viewYRot - yBob) * 0.1F))
        //
        // Computed once here rather than per hand because vanilla applies it
        // to the shared pose stack before either hand is drawn — both hands
        // sway together, as one rig.
        //
        // Both the interpolation and the final difference wrap, for the same
        // atan2 seam reason as the follow in Tick.
        {
            auto wrapDeg = [](float d) {
                while (d < -180.0f) d += 360.0f;
                while (d >  180.0f) d -= 360.0f;
                return d;
            };
            const float xBob = m_xBobPrev + wrapDeg(m_xBob - m_xBobPrev) * partialTick;
            const float yBob = m_yBobPrev + wrapDeg(m_yBob - m_yBobPrev) * partialTick;
            m_swayPitchDeg = wrapDeg(viewPitchDeg - xBob) * 0.1f;
            m_swayYawDeg   = wrapDeg(viewYawDeg   - yBob) * 0.1f;
        }

        // MC parity: clear the depth buffer before rendering the viewmodel
        // so it never gets occluded by world geometry pressed up against
        // the camera (walls, low ceilings). Mirrors MC's
        // GameRenderer.renderItemInHand path which runs after
        // `RenderSystem.clear(GL_DEPTH_BUFFER_BIT, ...)`. The viewmodel
        // then z-sorts only against itself.
        g_renderBackend->Clear(/*color=*/false, /*depth=*/true, /*stencil=*/false);
        m_viewPitchDeg = viewPitchDeg;

        // MC renderHandsWithItems: off hand first, then main hand on top.
        if (drawOff)  RenderHand(1, aspect, partialTick, walkDistance);
        if (drawMain) RenderHand(0, aspect, partialTick, walkDistance);
    }

    void HeldItemRenderer::RenderHand(int hand, float aspect, float partialTick,
                                      float walkDistance) {
        PROFILE_ZONE_N("HeldItemRender.Hand");
        const HandState& hs = m_hands[hand];
        // itemStack.has(DataComponents.MAP_ID): the map poses.
        if (hs.mapId) {
            RenderMapHand(hand, aspect, partialTick, walkDistance);
            return;
        }
        const float invert = (hand == 0) ? 1.0f : -1.0f;   // right / left arm
        const bool leftHand = (hand == 1);
        // Whether THIS hand carries the hold-to-use pose (MC keys the
        // useAnimation switch on player.getUsedItemHand()).
        const bool usingThisHand =
            m_useActive && m_useRemaining > 0 &&
            m_useHand == static_cast<uint32_t>(hand);

        // Look up the item once; bail if it doesn't exist.
        const auto& item = Game::ItemRegistry::Get(hs.displayed);

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
        // NOTE the absence of a `renderType == Block` test. Chest and shulker
        // box declare `{"type":"minecraft:special"}` in assets/items/{slug}.json,
        // which ClientItemLoader reports as ClientItemKind::Special and
        // ItemRegistry turns into renderType = Sprite — because MC renders them
        // through a SpecialModelRenderer rather than a baked block model. So
        // gating on Block excluded exactly the items this path exists to serve:
        // a held chest fell through to the sprite path, found no item texture,
        // and drew nothing at all.
        //
        // `blockId != Air` is the real test — it is set for every block item
        // regardless of renderType, and ForBlock decides the rest.
        const Game::BlockEntityType* beType =
            (item.blockId != Game::BlockID::Air)
                ? Game::BlockEntityTypes::ForBlock(item.blockId) : nullptr;
        Render::BlockEntityRenderer* beRenderer =
            (beType && Render::g_blockEntityRenderDispatcher)
                ? Render::g_blockEntityRenderDispatcher->GetRenderer(beType->TypeId())
                : nullptr;
        // SupportsBEWLR, not merely "has a renderer": this branch RETURNS, so
        // handing an item to a renderer that inherits the no-op RenderBEWLR
        // draws nothing at all. The campfire is exactly that case — it has a
        // renderer for its fire and cooking items, but its block geometry is an
        // ordinary model, so it must fall through to the cube path below.
        if (beRenderer && beRenderer->SupportsBEWLR()) {
            // Build the same hand transform chain as the cube path (see the
            // main chain below for the MC ordering notes), then hand to the
            // BE renderer with the MC-pixel-space mesh recentred to the
            // cell midpoint.
            const float equipBE = hs.equipProgressPrev +
                (hs.equipProgress - hs.equipProgressPrev) * partialTick;
            const float swingBE = (hand == 0 && m_swingActive)
                ? m_swingProgressPrev + (m_swingProgress - m_swingProgressPrev) * partialTick
                : 0.0f;
            glm::mat4 modelBE(1.0f);
            applyBobTransform(modelBE, walkDistance);
            applySwayTransform(modelBE, m_swayPitchDeg, m_swayYawDeg);
            if (usingThisHand && (m_useAnim == Game::ItemUseAnimation::EAT ||
                                  m_useAnim == Game::ItemUseAnimation::DRINK)) {
                applyEatTransform(modelBE, invert,
                                  (float)m_useRemaining, (float)m_useDuration,
                                  partialTick);
                applyItemArmTransform(modelBE, invert, equipBE);
            } else if (usingThisHand && m_useAnim == Game::ItemUseAnimation::BLOCK) {
                applyItemArmTransform(modelBE, invert, equipBE);
                applyBlockPose(modelBE, invert);
            } else if (swingBE > 0.0f) {
                applySwingOffsets(modelBE, invert, swingBE);
                applyItemArmTransform(modelBE, invert, equipBE);
                applySwingRotations(modelBE, invert, swingBE);
            } else {
                applyItemArmTransform(modelBE, invert, equipBE);
            }
            modelBE *= buildDisplayMatrix(item.blockId == Game::BlockID::DecoratedPot ? kDecoratedPotXf
                                                                                    : kChestLikeXf,
                                          leftHand);
            // Chest mesh lives in MC pixel-space [0,16]³. Scale to [0,1]
            // block space, then translate to centre the mesh on origin so
            // the display transform's rotation pivots around the chest's
            // own centre rather than its corner. This IS vanilla's
            // `translate(-0.5,-0.5,-0.5)` pre-display centring, expressed in
            // pixels — it centres the CELL, not the geometry's bounding box,
            // which is why a chest (14px tall, sitting on the cell floor)
            // correctly hangs slightly low rather than being re-centred.
            modelBE = glm::scale(modelBE, glm::vec3(1.0f / 16.0f));
            modelBE = glm::translate(modelBE, glm::vec3(-8.0f, -8.0f, -8.0f));
            const float fovYBE = glm::radians(70.0f);
            const glm::mat4 projBE = glm::perspective(fovYBE, aspect, 0.05f, 8.0f);
            // m_viewTilt: MC renders the hand inside the SAME bobbed pose as
            // the level (GameRenderer.bobHurt runs before renderItemInHand), so
            // the damage tilt and the death spin carry the hand with them. The
            // hand's own transform is already in view space, so the tilt slots
            // in exactly where the view matrix would be.
            // MC lights the hand from the lightmap (night, night vision and
            // the Darkness pulse all reach it) but never fogs it — the same
            // environment the block-item path below takes.
            BEWLRLight light;
            light.world = false;
            light.light = HandLight();
            beRenderer->RenderBEWLRStack(item.blockId, hs.stack, projBE * m_viewTilt * modelBE, light);
            return;
        }

        // The trident and the spears draw their in-hand models, placed by
        // those models' own first-person display transforms
        // (items/trident.json → trident_in_hand / trident_throwing, the
        // TridentModel special; items/<x>_spear.json → <x>_spear_in_hand).
        const bool tridentHand = hs.displayed == Game::Items::Trident;
        const Game::Spear::SpearDefinition* spearDef = Game::Spear::Find(hs.displayed);
        std::string displayModel;
        if (tridentHand) {
            displayModel = usingThisHand ? "item/trident_throwing" : "item/trident_in_hand";
        }

        if (tridentHand) {
            // No mesh here: RenderTridentModel draws it once the pose is built.
        } else if (item.renderType == Game::ItemRenderType::Block) {
            // Build a 1×1 textured-cube mesh, atlas UVs sampled from
            // the block's representative texture.
            std::vector<ItemCubeVert> verts;
            std::vector<uint32_t> idx;
            verts.reserve(kItemCubeMaxVerts);
            idx.reserve(kItemCubeMaxIdx);
            // Real model geometry first; the textured cube is the fallback for
            // blocks whose model has no elements (water/lava, and the BEWLR
            // blocks that never reach here anyway).
            if (!BuildBlockModelMesh(item.blockId, item.blockModelOverride, verts, idx)) {
                BuildBlockCubeMesh(item.blockId, verts, idx);
            }
            g_renderBackend->UpdateBuffer(s_cubeVB[hand], 0,
                verts.size() * sizeof(ItemCubeVert), verts.data());
            g_renderBackend->UpdateBuffer(s_cubeIB[hand], 0,
                idx.size() * sizeof(uint32_t), idx.data());
            mesh        = s_cubeMesh[hand];
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
            //
            // Every layer, each with its tint from assets/items/{slug}.json —
            // plant sprites are greyscale and coloured by it, spawn eggs are
            // a tinted base under tinted spots — the same composition the GUI
            // icon draws (GuiGraphics). The hand carries only the item id, so
            // per-stack tints (a potion's contents) read as the JSON default.
            Game::ItemStack shown;
            if (hs.stack.itemId == hs.displayed && !hs.stack.IsEmpty()) {
                shown = hs.stack;   // its components pick the sprite / tint
            } else {
                shown.itemId = hs.displayed;
            }
            shown.count  = 1;
            // MC FishingRodCast (items/fishing_rod.json's condition): the rod
            // in the hand whose line is out draws item/fishing_rod_cast.
            const HeldItemSpriteMesh::Entry* entry = nullptr;
            if (hs.displayed == Game::Items::FishingRod &&
                Client::Fishing::LocalCastHand() == static_cast<int>(hand)) {
                entry = HeldItemSpriteMesh::GetOrBuild("fishing_rod_cast");
            }
            if (!entry && spearDef) {
                // The long in-hand sprite (item/<x>_spear_in_hand).
                const std::string slug(Game::ItemRegistry::Slug(hs.displayed));
                entry = HeldItemSpriteMesh::GetOrBuild(slug + "_in_hand");
                if (entry) displayModel = "item/" + slug + "_in_hand";
            }
            if (!entry) {
                // Drawn as the hand's own slot: a per-stack model property
                // (using_item) answers only for the hand in use.
                const Game::ScopedItemRenderSlot renderSlot(m_handSlots[hand]);
                entry = HeldItemSpriteMesh::GetOrBuildForStack(shown);
            }
            if (!entry) return;
            mesh       = entry->mesh;
            indexCount = entry->indexCount;
            tex        = entry->texture != INVALID_TEXTURE ? entry->texture
                                                           : m_dummyTexture;
            xf = IsHandheldRod(hs.displayed) ? kRodXf
               : hs.displayed == Game::Items::Crossbow ? kCrossbowXf
               : kSpriteXf;
            // Sprite mesh lives in 0..16 local space; the display
            // transform's translation is in 0..1 block units, so we
            // need to pre-scale the mesh to fit in the same unit cube
            // as the block path. Done by composing an extra 1/16 scale
            // BEFORE the display transform below.
        }
        if (mesh == INVALID_MESH && !tridentHand) return;

        // ── Build the model matrix ─────────────────────────────────
        // Chain mirrors MC ItemInHandRenderer.renderArmWithItem exactly
        // (pose-stack order — first listed = outermost):
        //   1. View bob (renderHandsWithItems applies it to the whole
        //      stack before either hand; NOT mirrored)
        //   2. One of (mutually exclusive, like MC's branches):
        //        EAT/DRINK → applyEatTransform THEN applyItemArmTransform
        //                    (eat runs in VIEW space — the ordering fix)
        //        BLOCK     → applyItemArmTransform THEN the guard pose
        //        swinging  → swing offsets → arm transform → attack rotations
        //        idle      → applyItemArmTransform
        //      (applyItemArmTransform carries base position + equip slide)
        //   3. Display transform (per-item, X-mirrored for the left hand)
        //   4. Sprite/Block local-space normalisation
        //
        // partialTick blends the previous and current tick samples so
        // animation is smooth at any framerate.
        const float equip = hs.equipProgressPrev +
            (hs.equipProgress - hs.equipProgressPrev) * partialTick;
        const float swingP = (hand == 0 && m_swingActive)
            ? m_swingProgressPrev + (m_swingProgress - m_swingProgressPrev) * partialTick
            : 0.0f;

        glm::mat4 model(1.0f);
        applyBobTransform(model, walkDistance);
        applySwayTransform(model, m_swayPitchDeg, m_swayYawDeg);
        if (hs.displayed == Game::Items::Crossbow) {
            // MC FirstPersonHandsAndItemsRenderer's crossbow branch: the arm
            // transform first; drawing (not yet loaded) turns it across the
            // body, shaking and pulling back with the charge; otherwise the
            // swing, and a loaded crossbow at rest is held out to the side.
            applyItemArmTransform(model, invert, equip);
            const Game::ItemStack& shownStack = hs.stack.itemId == hs.displayed ? hs.stack
                                                                               : Game::ItemStack{};
            const bool charged = Game::FireworkItems::IsCrossbowCharged(shownStack);
            if (usingThisHand && !charged) {
                model = glm::translate(model, glm::vec3(invert * -0.4785682f, -0.094387f, 0.05731531f));
                model = glm::rotate(model, glm::radians(-11.935f),         {1, 0, 0});
                model = glm::rotate(model, glm::radians(invert * 65.3f),   {0, 1, 0});
                model = glm::rotate(model, glm::radians(invert * -9.785f), {0, 0, 1});
                const float timeHeld = static_cast<float>(m_useDuration) -
                    (static_cast<float>(m_useRemaining) - partialTick + 1.0f);
                const int chargeDuration = std::max(1, Game::FireworkItems::CrossbowChargeDuration(
                    shownStack.IsEmpty() ? Game::ItemStack(Game::Items::Crossbow, 1) : shownStack));
                const float power = std::min(timeHeld / static_cast<float>(chargeDuration), 1.0f);
                if (power > 0.1f) {
                    const float shake = std::sin((timeHeld - 0.1f) * 1.3f) * (power - 0.1f);
                    model = glm::translate(model, glm::vec3(0.0f, shake * 0.004f, 0.0f));
                }
                model = glm::translate(model, glm::vec3(0.0f, 0.0f, power * 0.04f));
                model = glm::scale(model, glm::vec3(1.0f, 1.0f, 1.0f + power * 0.2f));
                model = glm::rotate(model, glm::radians(-invert * 45.0f), {0, 1, 0});   // Axis.YN
            } else {
                // swingArm: the swing offsets and the attack rotations (an
                // identity at rest).
                applySwingOffsets(model, invert, swingP);
                applySwingRotations(model, invert, swingP);
                if (charged && swingP < 0.001f && hand == 0) {
                    model = glm::translate(model, glm::vec3(invert * -0.641864f, 0.0f, 0.0f));
                    model = glm::rotate(model, glm::radians(invert * 10.0f), {0, 1, 0});
                }
            }
        } else if (usingThisHand && (m_useAnim == Game::ItemUseAnimation::EAT ||
                              m_useAnim == Game::ItemUseAnimation::DRINK)) {
            applyEatTransform(model, invert,
                              (float)m_useRemaining, (float)m_useDuration,
                              partialTick);
            applyItemArmTransform(model, invert, equip);
        } else if (usingThisHand && m_useAnim == Game::ItemUseAnimation::BLOCK) {
            applyItemArmTransform(model, invert, equip);
            applyBlockPose(model, invert);
        } else if (usingThisHand && (m_useAnim == Game::ItemUseAnimation::BOW ||
                                     m_useAnim == Game::ItemUseAnimation::TRIDENT)) {
            // renderArmWithItem's BOW / TRIDENT cases: the arm transform,
            // the draw pose, then the pull — its shake past 10%, the lean
            // back and the stretch — and the turn back.
            applyItemArmTransform(model, invert, equip);
            const bool trident = m_useAnim == Game::ItemUseAnimation::TRIDENT;
            if (trident) {
                model = glm::translate(model, glm::vec3(invert * -0.5f, 0.7f, 0.1f));
                model = glm::rotate(model, glm::radians(-55.0f), {1, 0, 0});
            } else {
                model = glm::translate(model, glm::vec3(invert * -0.2785682f, 0.18344387f, 0.15731531f));
                model = glm::rotate(model, glm::radians(-13.935f), {1, 0, 0});
            }
            model = glm::rotate(model, glm::radians(invert * 35.3f), {0, 1, 0});
            model = glm::rotate(model, glm::radians(invert * -9.785f), {0, 0, 1});
            const float timeHeld = static_cast<float>(m_useDuration) -
                                   (static_cast<float>(m_useRemaining) - partialTick + 1.0f);
            float power;
            if (trident) {
                power = std::min(timeHeld / 10.0f, 1.0f);
            } else {
                power = timeHeld / 20.0f;
                power = std::min((power * power + power * 2.0f) / 3.0f, 1.0f);
            }
            if (power > 0.1f) {
                const float shake = std::sin((timeHeld - 0.1f) * 1.3f) * (power - 0.1f);
                model = glm::translate(model, glm::vec3(0.0f, shake * 0.004f, 0.0f));
            }
            model = glm::translate(model, glm::vec3(0.0f, 0.0f, power * (trident ? 0.2f : 0.04f)));
            model = glm::scale(model, glm::vec3(1.0f, 1.0f, 1.0f + power * 0.2f));
            model = glm::rotate(model, glm::radians(-invert * 45.0f), {0, 1, 0});   // Axis.YN
        } else if (usingThisHand && m_useAnim == Game::ItemUseAnimation::BRUSH) {
            // renderArmWithItem's BRUSH case (no custom arm transform): the
            // arm transform, then applyBrushTransform — a swipe of
            // -15 + 75 cos(2π t) degrees about X every 10 ticks of the hold.
            applyItemArmTransform(model, invert, equip);
            const float brushRemaining = std::fmod(static_cast<float>(m_useRemaining), 10.0f);
            const float deltaSinceLastUpdate = brushRemaining - partialTick + 1.0f;
            const float scaledUsageTime = 1.0f - deltaSinceLastUpdate / 10.0f;
            const float swipe = -15.0f + 75.0f * std::cos(scaledUsageTime * 2.0f * 3.1415927f);
            if (invert < 0) {
                model = glm::translate(model, glm::vec3(0.1f, 0.83f, 0.35f));
                model = glm::rotate(model, glm::radians(-80.0f), {1, 0, 0});
                model = glm::rotate(model, glm::radians(-90.0f), {0, 1, 0});
                model = glm::rotate(model, glm::radians(swipe), {1, 0, 0});
                model = glm::translate(model, glm::vec3(-0.3f, 0.22f, 0.35f));
            } else {
                model = glm::translate(model, glm::vec3(-0.25f, 0.22f, 0.35f));
                model = glm::rotate(model, glm::radians(-80.0f), {1, 0, 0});
                model = glm::rotate(model, glm::radians(90.0f), {0, 1, 0});
                model = glm::rotate(model, glm::radians(swipe), {1, 0, 0});
            }
        } else if (usingThisHand && m_useAnim == Game::ItemUseAnimation::SPEAR) {
            // SPEAR (a custom arm transform): the arm's rest spot without
            // the equip slide, then SpearAnimations.firstPersonUse — the
            // raise, the sway, the lowering and the recoil of a hit.
            model = glm::translate(model, glm::vec3(invert * 0.56f, -0.52f, -0.72f));
            const float timeHeld = static_cast<float>(m_useDuration) -
                                   (static_cast<float>(m_useRemaining) - partialTick + 1.0f);
            if (spearDef) {
                const Game::Spear::UseParams u = Game::Spear::UseParams::FromKineticWeapon(spearDef->kinetic, timeHeld);
                model = glm::translate(model, glm::vec3(
                    invert * (u.raiseProgress * 0.15f + u.raiseProgressEnd * -0.05f + u.swayProgress * -0.1f +
                              u.swayScaleSlow * 0.005f),
                    u.raiseProgress * -0.075f + u.raiseProgressMiddle * 0.075f + u.swayScaleFast * 0.01f,
                    u.raiseProgressStart * 0.05f + u.raiseProgressEnd * -0.05f + u.swayScaleSlow * 0.005f));
                // rotateAround(XP(...), 0, 0.1, 0).
                const float xAngle = -65.0f * Game::Ease::InOutBack(u.raiseProgress) - 35.0f * u.lowerProgress +
                                     100.0f * u.raiseBackProgress + -0.5f * u.swayScaleFast;
                model = glm::translate(model, glm::vec3(0.0f, 0.1f, 0.0f));
                model = glm::rotate(model, glm::radians(xAngle), {1, 0, 0});
                model = glm::translate(model, glm::vec3(0.0f, -0.1f, 0.0f));
                // rotateAround(YN(invert * (...)), invert * 0.15, 0, 0).
                const float yAngle = invert * (-90.0f * Game::Ease::Progress(u.raiseProgress, 0.5f, 0.55f) +
                                               90.0f * u.swayProgress + 2.0f * u.swayScaleSlow);
                model = glm::translate(model, glm::vec3(invert * 0.15f, 0.0f, 0.0f));
                model = glm::rotate(model, glm::radians(-yAngle), {0, 1, 0});
                model = glm::translate(model, glm::vec3(invert * -0.15f, 0.0f, 0.0f));
                model = glm::translate(model, glm::vec3(0.0f,
                    -Game::Spear::HitFeedbackAmount(m_ticksSinceKineticHitFeedback), 0.0f));
            }
        } else if (!usingThisHand && m_autoSpinAttack) {
            // A riptide in flight: the item held out ahead, both hands.
            applyItemArmTransform(model, invert, equip);
            model = glm::translate(model, glm::vec3(invert * -0.4f, 0.8f, 0.3f));
            model = glm::rotate(model, glm::radians(invert * 65.0f), {0, 1, 0});
            model = glm::rotate(model, glm::radians(invert * -85.0f), {0, 0, 1});
        } else if (swingP > 0.0f && m_swingStab) {
            // SwingAnimationType.STAB: SpearAnimations.firstPersonAttack —
            // the thrust out, the pull back.
            applyItemArmTransform(model, invert, equip);
            const float starting = Game::Ease::InOutSine(Game::Ease::Progress(swingP, 0.0f, 0.05f));
            const float middle   = Game::Ease::OutBack(Game::Ease::Progress(swingP, 0.05f, 0.2f));
            const float ending   = Game::Ease::InOutExpo(Game::Ease::Progress(swingP, 0.4f, 1.0f));
            model = glm::translate(model, glm::vec3(invert * 0.1f * (starting - middle),
                                                    -0.075f * (starting - ending),
                                                    0.65f * (starting - middle)));
            model = glm::rotate(model, glm::radians(-70.0f * (starting - ending)), {1, 0, 0});
            model = glm::translate(model, glm::vec3(0.0f, 0.0f, -0.25f * (ending - middle)));
        } else if (swingP > 0.0f) {
            applySwingOffsets(model, invert, swingP);
            applyItemArmTransform(model, invert, equip);
            applySwingRotations(model, invert, swingP);
        } else {
            applyItemArmTransform(model, invert, equip);
        }

        // The in-hand models placed by their own display transforms.
        if (!displayModel.empty()) {
            const ItemDisplay::Context ctx = leftHand ? ItemDisplay::Context::FirstPersonLeftHand
                                                      : ItemDisplay::Context::FirstPersonRightHand;
            model = ItemDisplay::Apply(model, ItemDisplay::Get(displayModel, ctx), leftHand);
            if (tridentHand) {
                RenderTridentModel(hand, model, aspect);
                return;
            }
            // The extruded sprite's cell: [0,16]² pixels, depth centred on 0
            // (ItemModelGenerator's layer sits at z 7.5..8.5).
            model = glm::translate(model, glm::vec3(0.0f, 0.0f, 0.5f));
            model = glm::scale(model, glm::vec3(1.0f / 16.0f));
            xf = DisplayXf{ glm::vec3(0.0f), glm::vec3(0.0f), 1.0f };
        }
        // Display transform
        if (displayModel.empty()) model *= buildDisplayMatrix(xf, leftHand);
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
        if (!displayModel.empty()) {
            // Already in the model cell (ItemDisplay::Apply's centring).
        } else if (item.renderType != Game::ItemRenderType::Block) {
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
        // See the block-entity path above for why the tilt goes here.
        const glm::mat4 mvp  = proj * m_viewTilt * model;

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
        // Fog is off for the hand (uFogColor alpha 0 below), so the world
        // position is not read; the identity keeps the uniform defined.
        g_renderBackend->SetUniformMat4 (m_shader, "uModel", glm::mat4(1.0f));
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
        // Environment uniforms the block shader now reads. The viewmodel is
        // drawn in view space, so world-space fog math is meaningless here:
        // uFogColor alpha 0 makes the fog mix a no-op (fogValue is
        // multiplied by it), and the distances are pushed out of reach.
        // The lightmap colour at the player's eye IS applied (MC's
        // lightmap-lit viewmodel: a torch-lit cave lights the hand warm, the
        // night dims it). Without these the GL uniforms default to 0 → the
        // item would render black.
        EntityEnvironment::SetDrawLight(m_shader, HandLight());
        g_renderBackend->SetUniformVec4 (m_shader, "uFogColor", glm::vec4(0.0f));
        g_renderBackend->SetUniformVec4 (m_shader, "uFogEnv",
            glm::vec4(1e9f, 1e9f, 1e9f, 1e9f));
        g_renderBackend->SetUniformVec3 (m_shader, "uCameraPos", glm::vec3(0.0f));

        g_renderBackend->DrawIndexed(mesh, indexCount);
        g_renderBackend->UnbindMesh();
    }

    // ── Maps (MC FirstPersonHandsAndItemsRenderer) ──────────────────────

    namespace {

        constexpr float kPi = 3.14159265358979323846f;

        glm::mat4 RotX(const glm::mat4& m, float deg) { return glm::rotate(m, glm::radians(deg), {1, 0, 0}); }
        glm::mat4 RotY(const glm::mat4& m, float deg) { return glm::rotate(m, glm::radians(deg), {0, 1, 0}); }
        glm::mat4 RotZ(const glm::mat4& m, float deg) { return glm::rotate(m, glm::radians(deg), {0, 0, 1}); }
        glm::mat4 Move(const glm::mat4& m, float x, float y, float z) { return glm::translate(m, {x, y, z}); }

        // PlayerModel (wide): the arm and its sleeve, as renderHand leaves
        // them — resetPose, then zRot ±0.1.
        ModelPart& PlayerArm(bool right) {
            static ModelPart arms[2];
            static bool built = false;
            if (!built) {
                built = true;
                for (int i = 0; i < 2; ++i) {
                    const bool r = i == 0;
                    ModelPart& arm = arms[i];
                    arm.name = r ? "right_arm" : "left_arm";
                    arm.pose = PartPose::Offset(r ? -5.0f : 5.0f, 2.0f, 0.0f);
                    CubeDefinition cube{};
                    cube.originX = r ? -3.0f : -1.0f; cube.originY = -2.0f; cube.originZ = -2.0f;
                    cube.sizeX = 4.0f; cube.sizeY = 12.0f; cube.sizeZ = 4.0f;
                    cube.texOffsX = r ? 40.0f : 32.0f; cube.texOffsY = r ? 16.0f : 48.0f;
                    arm.cubes.push_back(cube);
                    ModelPart* sleeve = arm.AddChild(r ? "right_sleeve" : "left_sleeve", PartPose::Zero());
                    CubeDefinition layer = cube;
                    layer.texOffsX = r ? 40.0f : 48.0f; layer.texOffsY = r ? 32.0f : 48.0f;
                    layer.growX = layer.growY = layer.growZ = 0.25f;
                    sleeve->cubes.push_back(layer);
                    arm.ResetPose();
                    arm.zRot = r ? 0.1f : -0.1f;
                }
            }
            return arms[right ? 0 : 1];
        }

        // MC calculateMapTilt.
        float MapTilt(float xRot) {
            float tilt = 1.0f - xRot / 45.0f + 0.1f;
            tilt = std::clamp(tilt, 0.0f, 1.0f);
            return -std::cos(tilt * kPi) * 0.5f + 0.5f;
        }

        struct MapDraw {
            TextureHandle texture;
            uint32_t first, count;
            float alphaTest;
        };

        struct MapBatch {
            std::vector<ItemCubeVert> verts;
            std::vector<uint32_t> idx;
            std::vector<MapDraw> draws;

            void Quad(const std::array<glm::vec3, 4>& p, const std::array<glm::vec2, 4>& uv) {
                const auto base = static_cast<uint32_t>(verts.size());
                for (int c = 0; c < 4; ++c) {
                    verts.push_back({ p[c].x, p[c].y, p[c].z, uv[c].x, uv[c].y, 255, 255, 255, 255 });
                }
                for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(base + i);
            }
            void Begin(TextureHandle texture, float alphaTest) {
                draws.push_back({ texture, static_cast<uint32_t>(idx.size()), 0, alphaTest });
            }
            void End() { draws.back().count = static_cast<uint32_t>(idx.size()) - draws.back().first; }
        };
    }

    void HeldItemRenderer::RenderMapHand(int hand, float aspect, float partialTick, float walkDistance) {
        PROFILE_ZONE_N("HeldItemRender.Map");
        const HandState& hs = m_hands[hand];
        if (!hs.mapId || s_mapMesh[hand] == INVALID_MESH) return;

        if (!m_skinTried) {
            // The default skin (DefaultPlayerSkin: the wide Steve) — this
            // engine has no skins of its own.
            m_skinTried = true;
            const std::string full = PlatformMain::GetAssetPath("assets/textures/entity/player/wide/steve.png");
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            if (unsigned char* pixels = std::filesystem::exists(full)
                    ? stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha) : nullptr) {
                m_skinTexture = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
                g_renderBackend->SetTextureFilter(m_skinTexture, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(m_skinTexture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
                stbi_image_free(pixels);
            }
        }

        const float inverseArmHeight = hs.equipProgressPrev +
            (hs.equipProgress - hs.equipProgressPrev) * partialTick;
        const float attack = (hand == 0 && m_swingActive)
            ? m_swingProgressPrev + (m_swingProgress - m_swingProgressPrev) * partialTick
            : 0.0f;

        // GameRenderer.bobView, then renderHandsWithItems' sway.
        glm::mat4 root(1.0f);
        applyBobTransform(root, walkDistance);
        applySwayTransform(root, m_swayPitchDeg, m_swayYawDeg);

        MapBatch batch;

        // renderPlayerHand: the arm's ModelPart through `pose` (ModelPart
        // units are pixels; a sixteenth of a block each).
        const auto playerHand = [&](const glm::mat4& pose, bool right) {
            if (m_skinTexture == INVALID_TEXTURE) return;
            std::vector<ModelVertex> mv;
            std::vector<uint32_t> mi;
            PlayerArm(right).Build(glm::scale(pose, glm::vec3(1.0f / 16.0f)), 64.0f, 64.0f, mv, mi);
            const auto base = static_cast<uint32_t>(batch.verts.size());
            batch.Begin(m_skinTexture, 0.1f);
            for (const ModelVertex& v : mv) batch.verts.push_back({ v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b, v.a });
            for (uint32_t i : mi) batch.idx.push_back(base + i);
            batch.End();
        };

        // renderMapHand.
        const auto mapHand = [&](glm::mat4 pose, bool right) {
            const float invert = right ? 1.0f : -1.0f;
            pose = RotY(pose, 92.0f);
            pose = RotX(pose, 45.0f);
            pose = RotZ(pose, invert * -41.0f);
            pose = Move(pose, invert * 0.3f, -1.1f, 0.45f);
            playerHand(pose, right);
        };

        // renderPlayerArm.
        const auto playerArm = [&](glm::mat4 pose, bool right) {
            const float invert = right ? 1.0f : -1.0f;
            const float sqrtAttack = std::sqrt(attack);
            const float xSwingPosition = -0.3f * std::sin(sqrtAttack * kPi);
            const float ySwingPosition = 0.4f * std::sin(sqrtAttack * 2.0f * kPi);
            const float zSwingPosition = -0.4f * std::sin(attack * kPi);
            pose = Move(pose, invert * (xSwingPosition + 0.64000005f),
                        ySwingPosition - 0.6f + inverseArmHeight * -0.6f, zSwingPosition - 0.71999997f);
            pose = RotY(pose, invert * 45.0f);
            const float zSwingRotation = std::sin(attack * attack * kPi);
            const float ySwingRotation = std::sin(sqrtAttack * kPi);
            pose = RotY(pose, invert * ySwingRotation * 70.0f);
            pose = RotZ(pose, invert * zSwingRotation * -20.0f);
            pose = Move(pose, invert * -1.0f, 3.6f, 3.5f);
            pose = RotZ(pose, invert * 120.0f);
            pose = RotX(pose, 200.0f);
            pose = RotY(pose, invert * -135.0f);
            pose = Move(pose, invert * 5.6f, 0.0f, 0.0f);
            playerHand(pose, right);
        };

        // renderMap: the paper (the checkerboard when there is data), then
        // MapRenderer.render.
        const auto renderMap = [&](glm::mat4 pose, int32_t mapId) {
            pose = RotY(pose, 180.0f);
            pose = RotZ(pose, 180.0f);
            pose = glm::scale(pose, glm::vec3(0.38f));
            pose = Move(pose, -0.5f, -0.5f, 0.0f);
            pose = glm::scale(pose, glm::vec3(0.0078125f));
            Client::Maps::MapGeometry geometry;
            const bool hasMapData = Client::Maps::BuildMapGeometry(mapId, pose, /*showOnlyFrame=*/false, geometry);
            const TextureHandle paper = Client::Maps::MapBackgroundTexture(hasMapData);
            if (paper != INVALID_TEXTURE) {
                const auto at = [&pose](float x, float y) { return glm::vec3(pose * glm::vec4(x, y, 0.0f, 1.0f)); };
                batch.Begin(paper, 0.01f);
                batch.Quad({ at(-7.0f, 135.0f), at(135.0f, 135.0f), at(135.0f, -7.0f), at(-7.0f, -7.0f) },
                           { glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 0.0f) });
                batch.End();
            }
            if (!hasMapData) return;
            if (geometry.mapTexture != INVALID_TEXTURE) {
                batch.Begin(geometry.mapTexture, 0.01f);
                batch.Quad(geometry.mapQuad.pos, geometry.mapQuad.uv);
                batch.End();
            }
            if (geometry.decorationTexture != INVALID_TEXTURE && !geometry.decorations.empty()) {
                batch.Begin(geometry.decorationTexture, 0.1f);
                for (const auto& q : geometry.decorations) {
                    if (batch.verts.size() + 4 > kMapMaxQuads * 4) break;
                    batch.Quad(q.pos, q.uv);
                }
                batch.End();
            }
        };

        const bool right = hand == 0;   // the main arm is the right arm
        if (hand == 0 && m_hands[1].displayed == 0) {
            // renderTwoHandedMap.
            const float sqrtAttack = std::sqrt(attack);
            const float ySwingPosition = -0.2f * std::sin(attack * kPi);
            const float zSwingPosition = -0.4f * std::sin(sqrtAttack * kPi);
            glm::mat4 pose = Move(root, 0.0f, -ySwingPosition / 2.0f, zSwingPosition);
            const float mapTilt = MapTilt(m_viewPitchDeg);
            pose = Move(pose, 0.0f, 0.04f + inverseArmHeight * -1.2f + mapTilt * -0.5f, -0.72f);
            pose = RotX(pose, mapTilt * -85.0f);
            {
                const glm::mat4 hands = RotY(pose, 90.0f);
                mapHand(hands, true);
                mapHand(hands, false);
            }
            const float xzSwingRotation = std::sin(sqrtAttack * kPi);
            pose = RotX(pose, xzSwingRotation * 20.0f);
            pose = glm::scale(pose, glm::vec3(2.0f));
            renderMap(pose, *hs.mapId);
        } else {
            // renderOneHandedMap.
            const float invert = right ? 1.0f : -1.0f;
            const glm::mat4 base = Move(root, invert * 0.125f, -0.125f, 0.0f);
            playerArm(RotZ(base, invert * 10.0f), right);
            glm::mat4 pose = Move(base, invert * 0.51f, -0.08f + inverseArmHeight * -1.2f, -0.75f);
            const float sqrtAttack = std::sqrt(attack);
            const float xSwing = std::sin(sqrtAttack * kPi);
            const float xSwingPosition = -0.5f * xSwing;
            const float ySwingPosition = 0.4f * std::sin(sqrtAttack * 2.0f * kPi);
            const float zSwingPosition = -0.3f * std::sin(attack * kPi);
            pose = Move(pose, invert * xSwingPosition, ySwingPosition - 0.3f * xSwing, zSwingPosition);
            pose = RotX(pose, xSwing * -45.0f);
            pose = RotY(pose, invert * xSwing * -30.0f);
            renderMap(pose, *hs.mapId);
        }

        if (batch.draws.empty() || batch.verts.empty()) return;
        if (batch.verts.size() > kMapMaxQuads * 4 || batch.idx.size() > kMapMaxQuads * 6) return;
        g_renderBackend->UpdateBuffer(s_mapVB[hand], 0, batch.verts.size() * sizeof(ItemCubeVert), batch.verts.data());
        g_renderBackend->UpdateBuffer(s_mapIB[hand], 0, batch.idx.size() * sizeof(uint32_t), batch.idx.data());

        const glm::mat4 proj = glm::perspective(glm::radians(70.0f), aspect, 0.05f, 8.0f);
        const glm::mat4 mvp = proj * m_viewTilt;

        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = true;
        state.colorWriteEnabled = true;
        state.depthCompareOp    = CompareOp::LessEqual;   // the paper, the map and its markers stack
        state.blendEnabled      = true;
        state.srcBlendFactor    = BlendFactor::SrcAlpha;
        state.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
        state.cullMode          = CullMode::None;         // the map is seen from whichever side faces
        state.frontFace         = FrontFace::CounterClockwise;
        state.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformMat4(m_shader, "uModel", glm::mat4(1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uPortalClipPlane", glm::vec4(0.0f));
        // The map's RenderTypes.text and the arm's entityTranslucent are
        // both lightmap-lit at the player's light (renderHandsWithItems).
        EntityEnvironment::SetDrawLight(m_shader, HandLight());
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(0.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv", glm::vec4(1e9f, 1e9f, 1e9f, 1e9f));
        g_renderBackend->SetUniformVec3(m_shader, "uCameraPos", glm::vec3(0.0f));
        for (const MapDraw& draw : batch.draws) {
            if (draw.count == 0 || draw.texture == INVALID_TEXTURE) continue;
            g_renderBackend->BindTexture(draw.texture, 0);
            g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", draw.alphaTest);
            g_renderBackend->DrawIndexed(s_mapMesh[hand], draw.count, draw.first);
        }
        g_renderBackend->UnbindMesh();
    }

    // ── The trident in the hand (TridentSpecialRenderer) ─────────────────

    namespace {
        // MC TridentModel.createLayer: the pole and its four children, all at
        // PartPose.ZERO (the flying trident's wrappers belong to its entity
        // renderer, not here). 32x32 sheet.
        const ModelPart& TridentGeometry() {
            static ModelPart pole;
            static bool built = false;
            if (!built) {
                built = true;
                pole.name = "pole";
                pole.pose = PartPose::Zero();
                const auto cube = [](float tx, float ty, float x, float y, float z,
                                     float sx, float sy, float sz, bool mirror) {
                    CubeDefinition c{};
                    c.originX = x; c.originY = y; c.originZ = z;
                    c.sizeX = sx; c.sizeY = sy; c.sizeZ = sz;
                    c.texOffsX = tx; c.texOffsY = ty;
                    c.mirror = mirror;
                    return c;
                };
                pole.cubes.push_back(cube(0.0f, 6.0f, -0.5f, 2.0f, -0.5f, 1.0f, 25.0f, 1.0f, false));
                pole.cubes.push_back(cube(4.0f, 0.0f, -1.5f, 0.0f, -0.5f, 3.0f, 2.0f, 1.0f, false));   // base
                pole.cubes.push_back(cube(4.0f, 3.0f, -2.5f, -3.0f, -0.5f, 1.0f, 4.0f, 1.0f, false));  // left_spike
                pole.cubes.push_back(cube(0.0f, 0.0f, -0.5f, -4.0f, -0.5f, 1.0f, 4.0f, 1.0f, false));  // middle_spike
                pole.cubes.push_back(cube(4.0f, 3.0f, 1.5f, -3.0f, -0.5f, 1.0f, 4.0f, 1.0f, true));    // right_spike
                pole.ResetPose();
            }
            return pole;
        }
    }

    void HeldItemRenderer::RenderTridentModel(int hand, const glm::mat4& pose, float aspect) {
        if (hand < 0 || hand > 1 || s_mapMesh[hand] == INVALID_MESH) return;
        if (!m_tridentTextureTried) {
            m_tridentTextureTried = true;
            const std::string full = PlatformMain::GetAssetPath("assets/textures/entity/trident.png");
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            if (unsigned char* pixels = std::filesystem::exists(full)
                    ? stbi_load(full.c_str(), &w, &h, &ch, STBI_rgb_alpha) : nullptr) {
                m_tridentTexture = g_renderBackend->CreateTexture2D(w, h, TextureFormat::RGBA8, pixels);
                g_renderBackend->SetTextureFilter(m_tridentTexture, TextureFilter::Nearest, TextureFilter::Nearest);
                g_renderBackend->SetTextureWrap(m_tridentTexture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
                stbi_image_free(pixels);
            }
        }
        if (m_tridentTexture == INVALID_TEXTURE) return;

        // TridentSpecialRenderer.DEFAULT_TRANSFORMATION: scale(1, -1, -1),
        // then the model in pixels.
        glm::mat4 m = glm::scale(pose, glm::vec3(1.0f, -1.0f, -1.0f));
        m = glm::scale(m, glm::vec3(1.0f / 16.0f));
        std::vector<ModelVertex> mv;
        std::vector<uint32_t> mi;
        TridentGeometry().Build(m, 32.0f, 32.0f, mv, mi);
        if (mv.empty() || mv.size() > kMapMaxQuads * 4 || mi.size() > kMapMaxQuads * 6) return;
        std::vector<ItemCubeVert> verts;
        verts.reserve(mv.size());
        for (const ModelVertex& v : mv) verts.push_back({ v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b, v.a });
        g_renderBackend->UpdateBuffer(s_mapVB[hand], 0, verts.size() * sizeof(ItemCubeVert), verts.data());
        g_renderBackend->UpdateBuffer(s_mapIB[hand], 0, mi.size() * sizeof(uint32_t), mi.data());

        const glm::mat4 proj = glm::perspective(glm::radians(70.0f), aspect, 0.05f, 8.0f);
        PipelineState state;
        state.depthTestEnabled  = true;
        state.depthWriteEnabled = true;
        state.colorWriteEnabled = true;
        state.depthCompareOp    = CompareOp::LessEqual;
        state.blendEnabled      = false;   // RenderTypes.entitySolid
        state.cullMode          = CullMode::None;
        state.frontFace         = FrontFace::CounterClockwise;
        state.primitiveType     = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", proj * m_viewTilt);
        g_renderBackend->SetUniformMat4(m_shader, "uModel", glm::mat4(1.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uPortalClipPlane", glm::vec4(0.0f));
        EntityEnvironment::SetDrawLight(m_shader, HandLight());
        g_renderBackend->SetUniformVec4(m_shader, "uFogColor", glm::vec4(0.0f));
        g_renderBackend->SetUniformVec4(m_shader, "uFogEnv", glm::vec4(1e9f, 1e9f, 1e9f, 1e9f));
        g_renderBackend->SetUniformVec3(m_shader, "uCameraPos", glm::vec3(0.0f));
        g_renderBackend->BindTexture(m_tridentTexture, 0);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.1f);
        g_renderBackend->DrawIndexed(s_mapMesh[hand], static_cast<uint32_t>(mi.size()), 0);
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
