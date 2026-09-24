// File: shaders/mob_particle_vk.frag
// Vulkan version of MobParticleSystem's fragment shader. MC particle.fsh:
// texture × per-particle colour, the 0.1 alpha cutout, the lightmap (here the
// draw's light — the sky dim, or 1 for the fullbright explosion and portal
// motes), then the terrain's fog.
#version 450

layout(set = 0, binding = 0) uniform sampler2D uSprite;

layout(push_constant) uniform PC {
    mat4  uMVP;
    vec2  uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4  uColor;
    vec4  uUVRange;
    vec4  uScalars;      // xyz: the draw's lightmap colour (EntityEnvironment.hpp)
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

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vRenderPos;
layout(location = 0) out vec4 FragColor;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 s = texture(uSprite, vUV);
    vec4 c = s * vColor;
    if (c.a < 0.1) discard;
    c.rgb *= pc.uScalars.xyz;
    vec3 fogDelta = vRenderPos - U.uCamPosBright_.xyz;
    float sph = length(fogDelta);
    float cyl = max(length(fogDelta.xz), abs(fogDelta.y));
    float fogValue = max(linearFog(sph, U.uFogEnv_.x, U.uFogEnv_.y),
                         linearFog(cyl, U.uFogEnv_.z, U.uFogEnv_.w));
    c.rgb = mix(c.rgb, U.uFogColor_.rgb, fogValue * U.uFogColor_.a);
    FragColor = c;
}
