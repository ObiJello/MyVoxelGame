#version 330 core
// File: shaders/beam_glare.frag (OpenGL volumetric light beam — lens glare)
// See src/client/renderer/effects/VolumetricBeam.hpp for the model.
in vec3 vLocal;
in vec3 vRenderPos;
out vec4 FragColor;

// Per-draw parameters: seven vec4 "slots" (VolumetricBeam.cpp has the table).
// The names are the ones VKBackend routes into the Common UBO, so one C++ path
// feeds both backends.
uniform vec3  uPortalColor;  uniform float uPulse;
uniform vec3  uColorDark;    uniform float uOpenAmount;
uniform vec3  uColorHot;     uniform float uOpenAmountVS;
uniform vec3  uKeyDir;       uniform float uKeyIntensity;
uniform float uTime, uTimeVS, uStaticAmount, uColorScale;
uniform float uPortalActive, uForceFarDepth, uOutlineMode, uFlashIntensity;
uniform vec4  uTint;
#define P0 vec4(uPortalColor, uPulse)
#define P1 vec4(uColorDark, uOpenAmount)
#define P2 vec4(uColorHot, uOpenAmountVS)
#define P3 vec4(uKeyDir, uKeyIntensity)
#define P4 vec4(uTime, uTimeVS, uStaticAmount, uColorScale)
#define P5 vec4(uPortalActive, uForceFarDepth, uOutlineMode, uFlashIntensity)
#define P6 uTint
// The scene-depth snapshot (VolumetricBeam::EnsureSceneDepth) — OpenGL only.
uniform sampler2D uSceneDepth;
uniform float     uHasSceneDepth;
uniform mat4      uSceneViewProj;   // render space → clip, the view's own
#define HAS_SCENE_DEPTH 1
uniform vec4 uGlareProbe;   // (source uv, source depth, enabled)

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
