// File: shaders/block_vk.frag (Vulkan version of block.frag)
// Cutout shader — uniform-based alpha discard via push constant.
#version 450
#ifdef OIT
#extension GL_GOOGLE_include_directive : require
#endif

// Input from vertex shader
layout (location = 0) in vec2 fragTexCoord;
layout (location = 1) in vec3 fragWorldPos;
layout (location = 2) in vec4 fragColor;

// Texture atlas sampler (descriptor set 0, binding 0)
layout (set = 0, binding = 0) uniform sampler2D uTextureAtlas;

// Push constants (must match C++ PushConstantBlock layout exactly)
layout (push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
    vec4  uPortalClipPlane_;  // 80 (vertex stage)
    vec4  uUVRange_;    // 96
    vec4  uDrawLight;   // 112 — xyz: the draw's lightmap colour (uScalars; "uDrawLight")
} pc;

// Common UBO (portal pipeline layout, set=1) — the chunk shaders are created
// via CreateShaderFromFilesPortal so the environment/fog fields appended at
// offset 304 are available. Prefix fields must be declared for std140 offsets.
layout (std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;
    mat4  uModel_;
    vec4  uPortalColor_;
    vec4  uColorDark_;
    vec4  uColorHot_;
    vec4  uKeyDir_;
    vec4  uTint_;
    vec4  uUVRange_;
    vec4  uScalarsA_;
    vec4  uScalarsB_;
    vec4  uScalarsC_;
    vec4  uScalarsD_;
    vec2  uScreenSize_;
    vec2  _pad_;
    vec4  uFogColor_;      // rgb = fog color, a = fog strength
    vec4  uFogEnv_;        // (envStart, envEnd, rdStart, rdEnd)
    vec4  uCamPosBright_;  // xyz = camera pos (w unused here)
    // MC's entity OVERLAY — the TNT white flash. rgb = overlay colour,
    // a = STRENGTH. See the long note in shaders/block.frag for why the alpha
    // is inverted relative to vanilla's OverlayTexture texel.
    //
    // APPENDED at offset 352, following the same rule uFogColor_ did: a shader
    // may declare a SMALLER UBO block than the bound buffer, so the _vk shaders
    // that still stop at 304 or 352 bytes stay valid.
    vec4  uOverlayColor_;  // 352
} U;

// Output
#ifndef OIT_ALPHA_ONLY
layout (location = 0) out vec4 FragColor;
#endif

// Improved Transparency (MC 26.3 OIT, Render::ImprovedTransparency): the
// OIT variants are this file compiled with -DOIT and a stage define
// (<name>_oit_{db,tr,ac}_vk.frag.spv, CMake). The engine's own compile never
// sees any of it.
#ifdef OIT
#include "oit_lib.glsl"
#endif

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 textureColor = texture(uTextureAtlas, fragTexCoord);

    // Discard transparent pixels (threshold varies per pass)
    if (textureColor.a < pc.uAlphaTest) {
        discard;
    }
#ifdef OIT_ALPHA_ONLY
    // MC item.fsh: the depth-bounds / transmittance stages read alpha only.
    executeAlphaOnlyPhase(gl_FragCoord.z, textureColor.a * fragColor.a);
#else

    // Vertex color contains: biome tint * AO * directional face shade (gamma space)
    vec3 finalColor = textureColor.rgb * fragColor.rgb;

    // MC entity.fsh order: overlay AFTER the vertex-colour multiply and BEFORE
    // the lightmap, so a flashing block still dims and fogs.
    finalColor = mix(finalColor, U.uOverlayColor_.rgb, U.uOverlayColor_.a);

    // The draw's lightmap colour (see block.frag) + MC-style distance fog
    finalColor *= pc.uDrawLight.xyz;
    vec3 fogDelta = fragWorldPos - U.uCamPosBright_.xyz;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, U.uFogEnv_.x, U.uFogEnv_.y),
                         linearFog(cyl, U.uFogEnv_.z, U.uFogEnv_.w));
    finalColor = mix(finalColor, U.uFogColor_.rgb, fogValue * U.uFogColor_.a);

    FragColor = vec4(finalColor, textureColor.a * fragColor.a);
#ifdef OIT_ACCUMULATE
    // MC calculateFinalColor: premultiplied, weighted by the transmittance
    // in front of it (the fog is already in the colour).
    FragColor = sampleColorForAccumulation(FragColor);
#endif
#endif
}
