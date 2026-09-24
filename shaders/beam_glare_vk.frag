// File: shaders/beam_glare_vk.frag (Vulkan volumetric light beam — lens glare)
// See src/client/renderer/effects/VolumetricBeam.hpp for the model.
// Vulkan twin of beam_glare.frag: same maths (generated from one
// body), parameters from the Common UBO, created with CreateShaderFromFilesPortal.
#version 450

layout(location = 0) in vec3 vLocal;
layout(location = 1) in vec3 vRenderPos;
layout(location = 0) out vec4 FragColor;

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

// A lens glare: a hot core, a soft halo and an anamorphic streak. The quad
// spans [-1,1]^2 locally, x stretched by the aspect (P2.w) so the streak has
// room; in round units the halo's radius is 1.
//   P2 = (colour, aspect)   P3.w = strength (fog and flare folded in on the CPU)
//   P4.y = streak strength
void main() {
    float aspect = P2.w;
    vec2 c = vec2(vLocal.x * aspect, vLocal.y);
    float r2 = dot(c, c);
    float core = exp(-40.0 * r2);
    float halo = exp(-4.5 * r2) * clamp(1.0 - r2, 0.0, 1.0);
    float ends = 1.0 - abs(vLocal.x);
    float streak = exp(-c.y * c.y * 900.0) * ends * ends * ends;
    float vis = 1.0;
#ifdef HAS_SCENE_DEPTH
    // Occlusion: five taps round the source's pixel in the depth snapshot.
    if (uGlareProbe.w > 0.5) {
        vec2 px = 2.0 / vec2(textureSize(uSceneDepth, 0));
        vec2 o[5] = vec2[5](vec2(0.0), vec2(px.x, 0.0), vec2(-px.x, 0.0), vec2(0.0, px.y), vec2(0.0, -px.y));
        vis = 0.0;
        for (int i = 0; i < 5; ++i) {
            vec2 uv = uGlareProbe.xy + o[i];
            bool inside = all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
            float d = inside ? texture(uSceneDepth, uv).r : 1.0;
            vis += d >= uGlareProbe.z - 2e-5 ? 0.2 : 0.0;
        }
    }
#endif
    vec3 col = P2.rgb * (1.6 * core + 0.55 * halo + 0.6 * P4.y * streak) * P3.w * vis;
    FragColor = vec4(1.0 - exp(-col), 0.0);
}
