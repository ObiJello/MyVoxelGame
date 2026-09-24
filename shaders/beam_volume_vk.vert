// File: shaders/beam_volume_vk.vert (Vulkan volumetric light beam — proxy)
// See src/client/renderer/effects/VolumetricBeam.hpp for the model.
// Vulkan twin of beam_volume.vert: same maths (generated from one
// body), parameters from the Common UBO, created with CreateShaderFromFilesPortal.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;     // unused (block vertex layout)
layout(location = 2) in vec4 aColor;  // unused
// Field order must match VKBackend::PushConstantBlock (see entity_vk.vert).
layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 0-63
    vec2 uScreenSize;   // 64-71
    float uLineWidth;   // 72-75
    float uAlphaTest;   // 76-79
} pc;
// Common UBO (portal pipeline layout, set = 1) — see entity_vk.frag. The beam
// parameters ride the portal renderer's fields (VolumetricBeam.cpp's table).
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
#define P0 U.uPortalColor_
#define P1 U.uColorDark_
#define P2 U.uColorHot_
#define P3 U.uKeyDir_
#define P4 U.uScalarsA_
#define P5 U.uScalarsB_
#define P6 U.uTint_
#define CAMERA    U.uCamPosBright_.xyz
#define FOG_COLOR U.uFogColor_
#define FOG_ENV   U.uFogEnv_
#define MVP pc.uMVP
layout(location = 0) out vec3 vRenderPos;

// The proxy: a unit capped frustum (aPos = axial fraction, cos, sin) placed
// over the cone — its radius × 1.03 + 0.08 (circumscribing the 16-gon, room
// for the soft rim), from just behind the apex to the clip end (P6.w).
// Must match kProxyScale / kProxyPad in VolumetricBeam.cpp.
void main() {
    vec3 A = P0.xyz;
    vec3 D = P1.xyz;
    float L = P0.w;
    float sEnd = min(P6.w, L);
    vec3 helper = abs(D.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 U1 = normalize(cross(D, helper));
    vec3 W1 = cross(D, U1);
    float s = mix(-0.08, sEnd + 0.08, aPos.x);
    float r = (P1.w + (P2.w - P1.w) * clamp(s, 0.0, L) / L) * 1.03 + 0.08;
    vec3 pos = A + D * s + (U1 * aPos.y + W1 * aPos.z) * r;
    vRenderPos = pos;
    gl_Position = MVP * vec4(pos, 1.0);
}
