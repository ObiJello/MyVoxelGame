// File: shaders/blockentity_vk.frag (Vulkan block-entity shader)
// Vulkan twin of blockentity.frag: texture × vertex colour, the draw's light
// (uBlockEntityLight, a push-constant colour in the uScreenSize/uLineWidth slots — a light change
// between draws must not cost a UBO slot), the terrain's fog. The alpha-discard threshold differs per renderer (chest /
// shulker 0.05, campfire food 0.5, skull 0.1) and rides uAlphaTest.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vRenderPos;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uLightRG;      // 64 — uBlockEntityLight.rg (VKBackend routes it here)
    float uLightB;      // 72 — uBlockEntityLight.b
    float uAlphaTest;
} pc;

// Common UBO (portal pipeline layout, set = 1) — see entity_vk.frag.
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
    vec4  uFogColor_;      // rgb = fog colour, a = strength
    vec4  uFogEnv_;        // (envStart, envEnd, rdStart, rdEnd)
    vec4  uCamPosBright_;  // xyz = camera (render space)
} U;

layout(location = 0) out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 t = texture(uTex, vUV);
    if (t.a < pc.uAlphaTest) discard;
    vec4 c = t * vColor;
    c.rgb *= vec3(pc.uLightRG, pc.uLightB);
    vec3 fogDelta = vRenderPos - U.uCamPosBright_.xyz;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, U.uFogEnv_.x, U.uFogEnv_.y),
                         linearFog(cyl, U.uFogEnv_.z, U.uFogEnv_.w));
    c.rgb = mix(c.rgb, U.uFogColor_.rgb, fogValue * U.uFogColor_.a);
    FragColor = c;
}
