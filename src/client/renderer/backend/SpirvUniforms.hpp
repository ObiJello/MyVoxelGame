// File: src/client/renderer/backend/SpirvUniforms.hpp
#pragma once

// The uniform model of the backends that draw with the _vk shaders
// (Vulkan, and Metal through the MSL generated from the same SPIR-V —
// tools/gen_metal_shaders.py). Those shaders read three blocks:
//
//   push constants   PushConstantBlock, 128 bytes, every shader
//   Common           CommonUBO, the portal-layout shaders (set 1 binding 0)
//   Bones            BonesUBO, the skinned viewmodel (set 1 binding 1)
//
// and RenderBackend::SetUniform* routes each GL uniform name the renderers
// use onto a field of one (or both) of them. The routing lives here, once,
// so both backends feed the shaders byte-identical blocks.

#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace Render {

    // Push constant data — must match every shader's
    // layout(push_constant) block exactly. Vulkan guarantees 128 bytes
    // of push constants minimum; we use ALL of it so portal-feature
    // shaders (crosshair, particle, simple HUD) can fit their tiny
    // uniform sets here without needing a UBO. Larger shaders
    // (portal renderer, viewmodel skinning) use the Common / Bones blocks.
    // CRITICAL: do NOT reorder the first four fields — block_vk /
    // crosshair_vk / highlight_vk / gui_*_vk / player_billboard_vk
    // shaders ALL declare a push_constant block ending at uAlphaTest
    // (offset 76, total 80 bytes). They read pc.uAlphaTest by offset,
    // so moving uAlphaTest off offset 76 breaks alpha discard for
    // every existing shader and transparent texels render as their
    // discarded-pixel default (black). New portal-feature uniforms
    // append AFTER the existing tail.
    struct PushConstantBlock {
        glm::mat4 uMVP        = glm::mat4(1.0f);   // 0-63   (64) — existing
        glm::vec2 uScreenSize = {0, 0};            // 64-71  (8)  — existing
        float     uLineWidth  = 0.0f;              // 72-75  (4)  — existing
        float     uAlphaTest  = 0.0f;              // 76-79  (4)  — existing
        // ---- new fields below; safe to add because GLSL shaders that
        // only declare the first 80 bytes simply ignore the trailing
        // bytes of the push range. ----
        glm::vec4 uColor      = {0, 0, 0, 0};      // 80-95  (16) — tint / portal color
        glm::vec4 uUVRange    = {0, 0, 1, 1};      // 96-111 (16) — (uvMin.xy, uvMax.xy)
        glm::vec4 uScalars    = {0, 0, 0, 0};      // 112-127(16) — per-shader scalar pack
    };                                              // 128 bytes — Vulkan minimum guarantee
    static_assert(sizeof(PushConstantBlock) == 128, "push constant block is 128 bytes");

    // Common uniforms used across portal/viewmodel/HDR/bloom shaders.
    // Packs every uniform we route from C++ SetUniform* into named
    // fields. Layout matches `layout(std140, set=1, binding=0) uniform
    // Common { ... }` in the _vk shaders. std140 means vec3 takes
    // 16 bytes (rounded up to vec4 alignment) so we use vec4 for
    // vec3-flavored uniforms with the 4th component carrying a
    // related scalar.
    struct CommonUBO {
        glm::mat4 uMVP         = glm::mat4(1.0f);   //   0
        glm::mat4 uModel       = glm::mat4(1.0f);   //  64
        glm::vec4 uPortalColor = {0, 0, 0, 0};      // 128 — rgb=color, w=uPulse
        glm::vec4 uColorDark   = {0, 0, 0, 0};      // 144 — rgb=dark, w=uOpenAmount
        glm::vec4 uColorHot    = {0, 0, 0, 0};      // 160 — rgb=hot,  w=uOpenAmountVS
        glm::vec4 uKeyDir      = {0, 0, 0, 0};      // 176 — xyz=keyDir, w=uKeyIntensity
        glm::vec4 uTint        = {1, 1, 1, 1};      // 192 — rgba (crosshair / glow tint)
        glm::vec4 uUVRange     = {0, 0, 1, 1};      // 208 — (uvMin.xy, uvMax.xy)
        glm::vec4 uScalarsA    = {0, 0, 0, 0};      // 224 — (uTime, uTimeVS, uStaticAmount, uColorScale)
        glm::vec4 uScalarsB    = {0, 0, 0, 0};      // 240 — (uPortalActive, uForceFarDepth, uOutlineMode, uFlashIntensity)
        glm::vec4 uScalarsC    = {1, 0, 1, 0};      // 256 — (uAmbient, uAlphaCutoff, uExposure, uHasBloom)
        glm::vec4 uScalarsD    = {0, 0, 0, 0};      // 272 — (uHasSprite, uUseSkin, uUseTextures, uWorldLighting)
        glm::vec2 uScreenSize  = {0, 0};            // 288
        // The block atlas's sprite arrays (AtlasBuilder): x = sprite ids
        // below this are array layers (uSpriteLayers), y = layers per array
        // (uSpriteArrayCap) — the terrain fragment shaders' U.uSpriteArray_.
        // Was the pad to vec4 alignment; every other _vk shader still names
        // it _pad_ and never reads it.
        glm::vec2 uSpriteArray = {0, 0};            // 296
        // Environment / fog block (sky, clouds, chunk fog + night dim).
        // APPENDED so older _vk shaders that declare the 304-byte layout
        // stay valid (a shader may declare a smaller UBO block than the
        // bound buffer). New shaders declare the full 352-byte layout.
        glm::vec4 uFogColor    = {1, 1, 1, 1};      // 304 — rgb=fog color, w=1
        glm::vec4 uFogEnv      = {1e9f, 1e9f, 1e9f, 1e9f}; // 320 — (envStart, envEnd, rdStart, rdEnd)
        glm::vec4 uCamPosBright= {0, 0, 0, 1};      // 336 — xyz=uCameraPos, w=uSkyBrightness
        // MC's entity OVERLAY (OverlayTexture) — rgb = overlay colour,
        // w = STRENGTH. Zero is a clean passthrough, which is why the
        // alpha is inverted from vanilla's texel; see shaders/block.frag.
        // Appended for the same reason the fog block was: a _vk shader may
        // declare a smaller layout than the buffer it is bound to.
        glm::vec4 uOverlayColor= {0, 0, 0, 0};      // 352 — rgb=colour, w=strength
        // The view's render origin (RenderOrigin.hpp): the integer block
        // position every float the GPU sees is measured from. The
        // terrain vertex shader subtracts it from the section-origin
        // table in INTEGER arithmetic. Appended, as the fog block was.
        glm::ivec4 uRenderOrigin= {0, 0, 0, 0};     // 368 — xyz = origin, w unused
        // x = the terrain light mode (ChunkRenderer::SetEnvironmentUniforms:
        // 0 four lightmap samples blended, 1 the light coords blended and
        // one sample); yzw spare.
        glm::vec4 uTerrainLight= {0, 0, 0, 0};      // 384
    };                                              // 384 bytes
    static_assert(sizeof(CommonUBO) == 400, "CommonUBO matches the shaders' std140 block");

    // 96-mat4 bone palette UBO for the viewmodel skinning shader.
    // 6144 bytes — well within the typical UBO size limit (16 KB).
    static constexpr int kMaxBones = 96;
    struct BonesUBO {
        glm::mat4 bones[kMaxBones];
        BonesUBO() { for (glm::mat4& b : bones) b = glm::mat4(1.0f); }
    };

    // GL→clip-space depth conversion. glm::perspective produces a GL-style
    // matrix where the near plane maps to NDC z = -1; Vulkan's and Metal's
    // near plane is z = 0 and the rasterizer clips anything with z_ndc < 0.
    // Without this premultiplication, half the depth range falls into the
    // clipped half-space. For normal scenes you barely notice (geometry is
    // mostly distant, sitting in z_ndc > 0), but the portal renderer's
    // oblique projection explicitly anchors geometry to the GL near plane
    // (z_ndc = -1) so that vertices on the destination portal's clip plane
    // land exactly on the near plane in OpenGL; uncorrected they end up at
    // z_ndc = -1 (clipped) instead of 0 (kept). Visible symptom: the
    // see-through view goes blank at steep angles / distance where more
    // dst-world geometry sits near the oblique clip plane. Every uMVP
    // coming through SetUniformMat4 is premultiplied to map z_ndc
    // [-1, +1] → [0, +1]:
    //   z_clip_new = 0.5·z_clip_old + 0.5·w  →  z_ndc_new = 0.5·z_ndc_old + 0.5
    // OpenGL keeps its native GL-style matrix.
    inline const glm::mat4 kZeroToOneDepthCorrect = glm::mat4(
        1.0f, 0.0f, 0.0f, 0.0f,   // col 0
        0.0f, 1.0f, 0.0f, 0.0f,   // col 1
        0.0f, 0.0f, 0.5f, 0.0f,   // col 2 — z_clip *= 0.5
        0.0f, 0.0f, 0.5f, 1.0f);  // col 3 — z_clip += 0.5 * w

    // The CPU-side working copies of the three blocks plus their dirty
    // state. The setters write to BOTH the push constants (for block-style
    // shaders) AND the Common / Bones blocks (for portal-feature shaders)
    // where a name feeds both: the push constants are sent fresh every
    // draw, so the double write costs nothing meaningful and lets a single
    // C++ setter feed either shader path transparently. The backend copies
    // a dirty block into its per-draw ring slot and clears the flag.
    struct SpirvUniformState {
        PushConstantBlock push;
        CommonUBO         common;
        BonesUBO          bones;
        bool commonDirty = true;
        bool bonesDirty  = true;
        // uMVP / uModel changed since the last Common slot. Kept apart from
        // commonDirty so a shader that ignores the block's matrices
        // (SetShaderIgnoresCommonMatrices) can keep rebinding the previous
        // slot; any slot written copies the current matrices, so a later
        // matrix-reading draw still sees the right ones.
        bool commonMatricesDirty = true;

        void SetMat4(const std::string& name, const glm::mat4& value);
        void SetVec4(const std::string& name, const glm::vec4& value);
        void SetVec3(const std::string& name, const glm::vec3& value);
        void SetVec2(const std::string& name, const glm::vec2& value);
        void SetFloat(const std::string& name, float value);
        void SetInt(const std::string& name, int value);
        void SetIVec3(const std::string& name, const glm::ivec3& value);
    };

} // namespace Render
