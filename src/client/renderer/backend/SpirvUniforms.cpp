// File: src/client/renderer/backend/SpirvUniforms.cpp
#include "SpirvUniforms.hpp"

#include <cstdlib>
#include <cstring>

namespace Render {

    // Uniform-name matching for the setters below, which run per draw across
    // every subsystem. std::string == const char* costs a strlen plus a
    // memcmp per candidate, and SetFloat walks up to twenty of them.
    // With the literal's length known at compile time, a candidate is
    // rejected on size, then on its LAST character (every name starts with
    // 'u', so the first is useless), before any memcmp runs.
    template <size_t N>
    static inline bool NameIs(const std::string& name, const char (&lit)[N]) {
        constexpr size_t len = N - 1;
        return name.size() == len && name[len - 1] == lit[len - 1] &&
               std::memcmp(name.data(), lit, len) == 0;
    }

    void SpirvUniformState::SetMat4(const std::string& name, const glm::mat4& value) {
        if (NameIs(name, "uMVP") || NameIs(name, "uViewProj")) {
            // "uViewProj" is the instanced block shader's name for the same
            // push-constant slot — its model matrix arrives per instance.
            const glm::mat4 mvp = kZeroToOneDepthCorrect * value;
            push.uMVP = mvp;
            // The Common copy is dirty only for shaders that read it — see
            // commonMatricesDirty.
            if (common.uMVP != mvp) {
                common.uMVP = mvp;
                commonMatricesDirty = true;
            }
        } else if (NameIs(name, "uPostParams")) {
            // A post pass's config uniforms (Render::PostChain), packed four
            // vec4s deep into the uMVP slot — never a transform, so no depth
            // correction.
            push.uMVP = value;
        } else if (NameIs(name, "uModel")) {
            if (common.uModel != value) {
                common.uModel = value;
                commonMatricesDirty = true;
            }
        } else if (NameIs(name, "uLocalToRender")) {
            // The block-entity shader's model -> render-space matrix (its
            // fog needs the fragment's render-space position). Affine, so
            // its three ROWS fit the three push-constant vec4s that shader
            // has no other use for (uColor / uUVRange / uScalars) — one
            // model matrix per block entity would otherwise burn a Common
            // UBO slot per draw. blockentity_vk.vert rebuilds it.
            push.uColor   = glm::vec4(value[0][0], value[1][0], value[2][0], value[3][0]);
            push.uUVRange = glm::vec4(value[0][1], value[1][1], value[2][1], value[3][1]);
            push.uScalars = glm::vec4(value[0][2], value[1][2], value[2][2], value[3][2]);
        } else if (name.rfind("uBones[", 0) == 0) {
            // "uBones[N]" — parse N, write into the bone palette.
            const size_t lbracket = 7;            // length of "uBones["
            const size_t rbracket = name.find(']', lbracket);
            if (rbracket != std::string::npos) {
                const int idx = std::atoi(name.c_str() + lbracket);
                if (idx >= 0 && idx < kMaxBones) {
                    bones.bones[idx] = value;
                    bonesDirty = true;
                }
            }
        }
    }

    void SpirvUniformState::SetVec4(const std::string& name, const glm::vec4& value) {
        if (NameIs(name, "uTint") || NameIs(name, "uColor") || NameIs(name, "uClipPlane") ||
            NameIs(name, "uPortalClipPlane")) {
            // uClipPlane (PlayerRenderer's portal-ghost half-space cull)
            // AND uPortalClipPlane (block shaders' world-space portal
            // plane → gl_ClipDistance[0]) are aliased onto the same
            // push-constant slot as uColor — no shader currently needs
            // both a tint and a clip plane simultaneously, and packing
            // them here keeps the chunk + player shaders from needing
            // a UBO descriptor for one tiny vec4.
            push.uColor = value;
            if (common.uTint != value) {
                common.uTint = value;
                commonDirty = true;
            }
        } else if (NameIs(name, "uEntityClipPlane")) {
            // The entity shaders' portal clip plane. Their uColor slot is the
            // hurt overlay, so the plane rides in the uUVRange slot, which
            // they do not otherwise use (entity_vk.vert).
            push.uUVRange = value;
        } else if (NameIs(name, "uFogColor")) {
            // Environment fog block (sky/clouds/chunk shaders) — dedicated
            // Common fields, NOT aliased onto uTint: block shaders need
            // uPortalClipPlane (which lives in the uColor/uTint slot) and
            // fog uniforms simultaneously.
            //
            // The fog fields are set by every world renderer, often per
            // draw (a block entity re-sends the frame's fog for each one);
            // only a real change costs a fresh UBO slot.
            if (common.uFogColor != value) {
                common.uFogColor = value;
                commonDirty = true;
            }
        } else if (NameIs(name, "uFogEnv")) {
            if (common.uFogEnv != value) {
                common.uFogEnv = value;
                commonDirty = true;
            }
        } else if (NameIs(name, "uTerrainLight")) {
            if (common.uTerrainLight != value) { common.uTerrainLight = value; commonDirty = true; }
        } else if (NameIs(name, "uOverlayColor")) {
            // MC's entity overlay — the primed-TNT white flash. Its own
            // field rather than an alias onto uTint, because the block shaders
            // need uPortalClipPlane (which lives in the uColor/uTint slot) at
            // the same time.
            if (common.uOverlayColor != value) {
                common.uOverlayColor = value;
                commonDirty = true;
            }
        }
    }

    void SpirvUniformState::SetVec3(const std::string& name, const glm::vec3& value) {
        if (NameIs(name, "uPortalColor")) {
            push.uColor         = glm::vec4(value, push.uColor.a);
            common.uPortalColor = glm::vec4(value, common.uPortalColor.a);
            commonDirty = true;
        } else if (NameIs(name, "uColorDark")) {
            common.uColorDark = glm::vec4(value, common.uColorDark.a);
            commonDirty = true;
        } else if (NameIs(name, "uColorHot")) {
            common.uColorHot = glm::vec4(value, common.uColorHot.a);
            commonDirty = true;
        } else if (NameIs(name, "uKeyDir")) {
            common.uKeyDir = glm::vec4(value, common.uKeyDir.a);
            commonDirty = true;
        } else if (NameIs(name, "uTint") || NameIs(name, "uColor")) {
            push.uColor  = glm::vec4(value, push.uColor.a);
            common.uTint = glm::vec4(value, common.uTint.a);
            commonDirty = true;
        } else if (NameIs(name, "uCameraPos")) {
            const glm::vec4 v(value, common.uCamPosBright.w);
            if (common.uCamPosBright != v) {
                common.uCamPosBright = v;
                commonDirty = true;
            }
        } else if (NameIs(name, "uFogColor")) {
            const glm::vec4 v(value, common.uFogColor.a);
            if (common.uFogColor != v) {
                common.uFogColor = v;
                commonDirty = true;
            }
        } else if (NameIs(name, "uEntityLight") || NameIs(name, "uDrawLight")) {
            // The per-draw lightmap colour of the entity- and block-family
            // shaders (EntityEnvironment.hpp): push constants uScalars.xyz,
            // so a light change between draws never burns a UBO slot.
            push.uScalars = glm::vec4(value, push.uScalars.w);
        } else if (NameIs(name, "uBlockEntityLight")) {
            // blockentity_vk: its uScalars hold uLocalToRender's rows, so the
            // colour rides uScreenSize.xy (r, g) and uLineWidth (b) — no line
            // shader shares a draw with it.
            push.uScreenSize = glm::vec2(value.r, value.g);
            push.uLineWidth  = value.b;
        }
    }

    void SpirvUniformState::SetVec2(const std::string& name, const glm::vec2& value) {
        if (NameIs(name, "uScreenSize")) {
            push.uScreenSize   = value;
            common.uScreenSize = value;
            commonDirty = true;
        } else if (NameIs(name, "uUVMin")) {
            push.uUVRange.x = value.x;   push.uUVRange.y = value.y;
            common.uUVRange.x = value.x; common.uUVRange.y = value.y;
            commonDirty = true;
        } else if (NameIs(name, "uUVMax")) {
            push.uUVRange.z = value.x;   push.uUVRange.w = value.y;
            common.uUVRange.z = value.x; common.uUVRange.w = value.y;
            commonDirty = true;
        } else if (NameIs(name, "uInSize")) {
            // The entity-outline post passes' input size (MC SamplerInfo
            // InSize) — push constants only, like uScreenSize.
            push.uScreenSize = value;
        } else if (NameIs(name, "uBlurDir")) {
            // entity_outline_box_blur's BlurDir.
            push.uUVRange.x = value.x;
            push.uUVRange.y = value.y;
        } else if (NameIs(name, "uOutSize")) {
            // A post pass's SamplerInfo.OutSize (Render::PostChain).
            push.uUVRange.z = value.x;
            push.uUVRange.w = value.y;
        } else if (NameIs(name, "uAuxSize")) {
            // A post pass's second sampler's size.
            push.uColor.x = value.x;
            push.uColor.y = value.y;
        }
    }

    void SpirvUniformState::SetFloat(const std::string& name, float value) {
        // Push-constant block-style aliases:
        if (NameIs(name, "uLineWidth"))           { push.uLineWidth = value; }
        else if (NameIs(name, "uAlphaTest"))      { push.uAlphaTest = value; }
        // Post passes (Render::PostChain): whether each input samples
        // bilinear (else nearest, in the shader), and globals.MenuBlurRadius.
        else if (NameIs(name, "uInBilinear"))     { push.uLineWidth = value; }
        else if (NameIs(name, "uAuxBilinear"))    { push.uAlphaTest = value; }
        else if (NameIs(name, "uMenuBlurRadius")) { push.uScalars.x = value; }
        // Portal renderer + crosshair scalar packing:
        else if (NameIs(name, "uPulse"))          { push.uScalars.x = value; common.uPortalColor.a = value; commonDirty = true; }
        else if (NameIs(name, "uOpenAmount"))     { push.uScalars.z = value; common.uColorDark.a    = value; commonDirty = true; }
        else if (NameIs(name, "uOpenAmountVS"))   { common.uColorHot.a  = value; commonDirty = true; }
        else if (NameIs(name, "uKeyIntensity"))   { common.uKeyDir.a    = value; commonDirty = true; }
        else if (NameIs(name, "uTime"))           { push.uScalars.y = value; common.uScalarsA.x = value; commonDirty = true; }
        else if (NameIs(name, "uTimeVS"))         { common.uScalarsA.y = value; commonDirty = true; }
        else if (NameIs(name, "uStaticAmount"))   { common.uScalarsA.z = value; commonDirty = true; }
        else if (NameIs(name, "uColorScale"))     { common.uScalarsA.w = value; commonDirty = true; }
        else if (NameIs(name, "uPortalActive"))   { common.uScalarsB.x = value; commonDirty = true; }
        else if (NameIs(name, "uForceFarDepth"))  { common.uScalarsB.y = value; commonDirty = true; }
        else if (NameIs(name, "uOutlineMode"))    { common.uScalarsB.z = value; commonDirty = true; }
        else if (NameIs(name, "uFlashIntensity")) { push.uScalars.w = value; common.uScalarsB.w = value; commonDirty = true; }
        else if (NameIs(name, "uAmbient"))        { common.uScalarsC.x = value; commonDirty = true; }
        else if (NameIs(name, "uAlphaCutoff"))    { common.uScalarsC.y = value; commonDirty = true; }
        else if (NameIs(name, "uHasSprite"))      { common.uScalarsD.x = value; commonDirty = true; }
        else if (NameIs(name, "uUseSkin"))        { common.uScalarsD.y = value; commonDirty = true; }
        else if (NameIs(name, "uUseTextures"))    { common.uScalarsD.z = value; commonDirty = true; }
        else if (NameIs(name, "uSkyBrightness"))  {
            if (common.uCamPosBright.w != value) { common.uCamPosBright.w = value; commonDirty = true; }
        }
        // The per-draw lightmap stand-in of the entity / particle / stick-
        // figure shaders (EntityEnvironment.hpp) — a push constant, so a batch
        // switching between lit and emissive never burns a UBO slot.
        else if (NameIs(name, "uEntityLight") || NameIs(name, "uDrawLight")) {
            push.uScalars = glm::vec4(value, value, value, push.uScalars.w);
        }
        // The block-entity shader's light: its uScalars are taken by
        // uLocalToRender's rows, so it rides the uLineWidth slot (no line
        // shader shares a draw with it).
        else if (NameIs(name, "uBlockEntityLight")) {
            push.uScreenSize = glm::vec2(value, value);
            push.uLineWidth  = value;
        }
    }

    void SpirvUniformState::SetIVec3(const std::string& name, const glm::ivec3& value) {
        if (NameIs(name, "uRenderOrigin")) {
            // .w carries uFadeNowMs (the chunk fade clock); a view's origin
            // change must not zero it.
            common.uRenderOrigin = glm::ivec4(value, common.uRenderOrigin.w);
            commonDirty = true;
        }
    }

    void SpirvUniformState::SetInt(const std::string& name, int value) {
        // Texture-sampler bindings come through as integers (legacy
        // GL pattern). These backends bind textures by slot, so we ignore
        // those names. Other ints fall through into the float path's
        // uUseTextures slot etc.
        if (NameIs(name, "uUseTextures")) {
            common.uScalarsD.z = static_cast<float>(value);
            commonDirty = true;
        } else if (NameIs(name, "uFadeNowMs")) {
            // Chunk fade-in clock (SectionFade.hpp): terrain_vk.vert reads
            // U.uRenderOrigin_.w; the fade length rides uScalarsC.z.
            common.uRenderOrigin.w = value;
            commonDirty = true;
        } else if (NameIs(name, "uFadeMs")) {
            common.uScalarsC.z = static_cast<float>(value);
            commonDirty = true;
        } else if (NameIs(name, "uSpriteLayers")) {
            // The block atlas's sprite arrays (AtlasBuilder): sprite ids
            // below this are layers of them; the terrain fragment shaders
            // read int(U.uSpriteArray_.x). 0 = no array, every sprite
            // through the atlas.
            if (common.uSpriteArray.x != static_cast<float>(value)) {
                common.uSpriteArray.x = static_cast<float>(value);
                commonDirty = true;
            }
        } else if (NameIs(name, "uWorldLighting")) {
            // 0 = World Lighting off: the terrain shaders skip the lightmap
            // (U.uScalarsD_.w).
            if (common.uScalarsD.w != static_cast<float>(value)) {
                common.uScalarsD.w = static_cast<float>(value);
                commonDirty = true;
            }
        } else if (NameIs(name, "uSpriteArrayCap")) {
            // Layers per array: id s is layer s % cap of array s / cap.
            if (common.uSpriteArray.y != static_cast<float>(value)) {
                common.uSpriteArray.y = static_cast<float>(value);
                commonDirty = true;
            }
        } else if (NameIs(name, "uHasSprite")) {
            // PortalParticleSystem uses uHasSprite to select between the
            // Portal-extracted sprite texture path and the procedural
            // soft-disc fallback. Routed into BOTH the push-constant
            // uScalars.x (where portal_particle_vk reads it) and the
            // Common slot (so any portal-layout shader that wants it also
            // sees it).
            push.uScalars.x    = static_cast<float>(value);
            common.uScalarsD.x = static_cast<float>(value);
            commonDirty = true;
        }
        // "uSprite" and other sampler-name ints are no-ops — textures bind
        // by slot, not via a uniform int.
    }

} // namespace Render
