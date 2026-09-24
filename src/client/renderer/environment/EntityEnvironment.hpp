// File: src/client/renderer/environment/EntityEnvironment.hpp
//
// The environment every world-space draw that is NOT terrain takes from the
// frame — mobs, players, armor stands, morphs, block entities (sign text
// included), particles — so they dim at night and fog exactly as the
// terrain they stand on does. MC applies the same two things to entities,
// block entities and particles that it applies to chunks: the lightmap and
// FogRenderer's fog (entity.fsh / particle.fsh / block.fsh all end in
// `color *= lightMapColor; apply_fog(...)`).
//
// ── Light ──────────────────────────────────────────────────────────────
// MC gives every entity, block entity, dropped item and particle ONE packed
// light (LightCoordsUtil) per submit — EntityRenderer.getPackedLightCoords:
// block and sky light at BlockPos.containing(getLightProbePosition), which
// is the EYE position — and the shader multiplies by the lightmap texel for
// it. Here the renderers read the packed light from the bound client
// level's chunk light (PackedLightAt) and hand the texel's RGB to the draw
// (LightColor, from Render::Lightmap's CPU copy of the frame's lightmap):
//
//   LitAt(probe)        an ordinary entity: the texel for the light at its
//                       probe cell.
//   FullBlockAt(probe)  an entity MC lights at block light 15
//                       (EntityRenderer.getBlockLightLevel: burning
//                       entities, and the blaze, magma cube, wither, allay,
//                       vex, the fireballs, the shulker bullet …) — block 15,
//                       the probe cell's sky light.
//   kEmissive           render types with NO lightmap at all (MC's EMISSIVE
//                       define: eyes, the warden's glow layers, breeze eyes,
//                       the explosion particle): 1.0.
//
// Lit() / FullBlockLight() are the scalar, position-free forms (the sky
// texel at block light 0 / 15) for the few draws with no position — and,
// under OBEY_LIGHT=0, the whole story: the old frame-wide sky dim.
//
// ── Fog ────────────────────────────────────────────────────────────────
// The terrain's exact packing — uFogColor (rgb, a = strength),
// uFogEnv (envStart, envEnd, rdStart, rdEnd), uCameraPos in RENDER space —
// and the terrain's formula (spherical + cylindrical linear fog, max of the
// two). Emissive draws skip the light, never the fog: MC fogs eyes too, so
// a blind player sees an enderman's eyes vanish into the black with it.
//
// On Vulkan these land in the Common UBO (VKBackend routes the names), so
// every shader reading them is created with CreateShaderFromFilesPortal.
// The per-draw light rides the push constants (uEntityLight →
// PushConstantBlock::uScalars.x) so a batch change never burns a UBO slot.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace Render::EntityEnvironment {

    // A shader that reads the frame's fog. On Vulkan it is created on the
    // portal pipeline layout (the Common UBO carries the fog) and marked as
    // taking its matrices from the push constants only
    // (VKBackend::SetShaderIgnoresCommonMatrices), so a per-draw uMVP never
    // costs a UBO slot. OpenGL: CreateShaderFromFiles.
    ShaderHandle CreateShader(const std::string& vertexPath, const std::string& fragmentPath);

    // No lightmap (MC's EMISSIVE render types).
    inline constexpr float kEmissive = 1.0f;

    // Position-free scalars (see above).
    float Lit();
    float FullBlockLight();

    // MC LightCoordsUtil.pack(block, sky) at the cell holding `world`, from
    // the bound client level (a portal view binds the far level).
    int PackedLightAt(const glm::dvec3& world);
    int PackedLightAt(int x, int y, int z);
    // MC LevelRenderer.getLightCoords(level, pos) — a BLOCK's light, what
    // block entities are drawn with: FULL_BRIGHT for an emissiveRendering
    // state, else the cell's light with block light raised to the state's
    // own emission (a lit campfire's food at 15).
    int LevelLightCoordsAt(const glm::ivec3& pos);
    // The lightmap colour for a packed light, on the lightmap of the frame
    // being drawn. OBEY_LIGHT=0: Lit() / FullBlockLight() as grey.
    glm::vec3 LightColor(int packedLight);
    glm::vec3 LitAt(const glm::dvec3& probe);
    glm::vec3 FullBlockAt(const glm::dvec3& probe);
    // A light colour as an RGBA8 word (r in the low byte, alpha 255) — for
    // per-instance / per-vertex light attributes.
    uint32_t ToRGBA8(const glm::vec3& light);

    // The per-draw light of the entity-family shaders (entity, stick figure,
    // mob particle): uEntityLight, a vec3 — on Vulkan the push constants'
    // uScalars.xyz, so a change between draws never burns a UBO slot.
    void SetEntityLight(ShaderHandle shader, const glm::vec3& light);
    // The same for the block-family shaders (block.frag: dropped items, XP
    // orbs, falling blocks / TNT, the held item, the fill preview):
    // uDrawLight, also in the push constants' uScalars.xyz on Vulkan.
    void SetDrawLight(ShaderHandle shader, const glm::vec3& light);

    // uFogColor / uFogEnv / uCameraPos / uSkyBrightness from the frame being
    // drawn (a portal view's own frame while one is active).
    // `cameraWorld` is the WORLD position of the view's eye; it is handed to
    // the shader in render space (the space the vertices are in).
    void ApplyWorld(ShaderHandle shader, const glm::dvec3& cameraWorld);

    // No fog, a fixed brightness — the viewmodel (MC lights the hand from
    // the lightmap but it is far too close to fog) and GUI item icons.
    void ApplyUnfogged(ShaderHandle shader, float brightness);

} // namespace Render::EntityEnvironment
