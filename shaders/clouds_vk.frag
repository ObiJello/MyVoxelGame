// File: shaders/clouds_vk.frag
#version 450
#ifdef OIT
#extension GL_GOOGLE_include_directive : require
#endif

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vDist;
#ifndef OIT_ALPHA_ONLY
layout(location = 0) out vec4 FragColor;
#endif

// Improved Transparency (MC 26.3 OIT, Render::ImprovedTransparency): the
// OIT variants are this file compiled with -DOIT and a stage define
// (<name>_oit_{db,tr,ac}_vk.frag.spv, CMake). The engine's own compile never
// sees any of it.
#ifdef OIT
#include "oit_lib.glsl"
#endif

layout(set = 0, binding = 0) uniform sampler2D uTexture; // unused (dummy white)

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4 uColor;
} pc;

layout(std140, set = 1, binding = 0) uniform Common {
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
    vec4  uFogColor_;
    vec4  uFogEnv_;
    vec4  uCamPosBright_;
} U;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    // MC rendertype_clouds.fsh: fog fades ALPHA (not a color mix).
    vec4 color = vColor * pc.uColor;
#ifndef OIT_DEPTH_BOUNDS
    // (MC clouds.fsh: the depth bounds take the cloud unfaded.)
    color.a *= 1.0 - linearFog(vDist, U.uFogEnv_.x, U.uFogEnv_.y);
#endif
    if (color.a <= 0.0) discard;
#ifdef OIT_ALPHA_ONLY
    executeAlphaOnlyPhase(gl_FragCoord.z, color.a);
#else
    FragColor = color;
#ifdef OIT_ACCUMULATE
    // MC calculateFinalColor: premultiplied, weighted by the transmittance
    // in front of it (the fog is already in the colour).
    FragColor = sampleColorForAccumulation(FragColor);
#endif
#endif
}
