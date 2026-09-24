// File: shaders/beam_pool_vk.frag (Vulkan volumetric light beam — light pool)
// See src/client/renderer/effects/VolumetricBeam.hpp for the model.
// Vulkan twin of beam_pool.frag: same maths (generated from one
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

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

// The terrain's fog at offset d from the eye (spherical environmental fog,
// cylindrical render-distance fog, the larger of the two) — what the hill
// behind the beam is fogged by, times the cone's fog factor (P6.x).
float fogAt(vec3 d) {
    float sph = length(d);
    float cyl = max(length(d.xz), abs(d.y));
    return max(linearFog(sph, FOG_ENV.x, FOG_ENV.y), linearFog(cyl, FOG_ENV.z, FOG_ENV.w))
         * FOG_COLOR.a * P6.x;
}

// The beam's emission at q = x - apex, before phase, mist and fog: colour ×
// radial profile × axial falloff. `s` returns the distance along the axis.
//   radial: a tight core (e^-14ρ², core colour) in a wide falloff (e^-3ρ²,
//           edge colour), windowed by (1-ρ²)² so the rim is exactly dark
//   axial:  fade-in from the lens, 1/(1+(s/d½)²) spread, e^-σs extinction,
//           fade over the last part of the length
vec3 beamLight(vec3 q, out float s) {
    s = dot(q, P1.xyz);
    float L = P0.w;
    float k = (P2.w - P1.w) / L;
    float rs = max(P1.w + k * max(s, 0.0), 1e-3);
    float rho2 = max(dot(q, q) - s * s, 0.0) / (rs * rs);
    if (rho2 >= 1.0 || s <= 0.0) return vec3(0.0);
    float window = 1.0 - rho2;
    window *= window;
    vec3 radial = (P2.rgb * exp(-14.0 * rho2) + P3.rgb * exp(-3.0 * rho2)) * window;
    float fadeIn  = smoothstep(0.0, max(P5.z, 1e-3), s);
    float spread  = 1.0 / (1.0 + (s * s) / (P5.w * P5.w));
    float endFade = 1.0 - smoothstep(P6.y * L, L, s);
    float ext     = exp(-P5.y * s);
    return radial * (fadeIn * spread * endFade * ext);
}

// Where the beam lands: the beam's own light (beamLight) at a surface point,
// Lambert against the surface normal, fogged, times the gain (P4.w). Blend is
// DST_COLOR, ONE: the terrain comes out dst · (1 + light), so its texture
// shows through the splash.
//   OpenGL with depth: a box decal — the scene point at this pixel, if it is
//   inside the box (uPoolRenderToBox), normal from its screen derivatives.
//   Otherwise: the quad lies in the face that was hit (normal P4.xyz).
void main() {
    vec3 eye = CAMERA;
    vec3 q = vRenderPos;
    vec3 n = vec3(P4.x, P4.y, P4.z);
    bool outside = false;
#ifdef HAS_SCENE_DEPTH
    if (uPoolDeferred > 0.5) {
        // Derivatives before any discard (they need the whole quad).
        vec3 R = normalize(vRenderPos - eye);
        float t = min(sceneDistance(eye, R), 1e4);
        q = eye + R * t;
        vec3 dn = cross(dFdx(q), dFdy(q));
        n = dot(dn, dn) > 1e-12 ? normalize(dn) : n;
        if (dot(n, eye - q) < 0.0) n = -n;
        vec3 local = (uPoolRenderToBox * vec4(q, 1.0)).xyz;
        outside = t >= 1e4 || any(greaterThan(abs(local), vec3(1.0)));
    }
#endif
    if (outside) discard;
    vec3 rel = q - P0.xyz;
    float s;
    vec3 light = beamLight(rel, s);
    float lambert = max(dot(n, -normalize(rel)), 0.0);
    float T = 1.0 - fogAt(q - eye);
    vec3 relight = light * (lambert * T * P4.w * P3.w);
    FragColor = vec4(min(relight, vec3(1.0)), 0.0);
}
