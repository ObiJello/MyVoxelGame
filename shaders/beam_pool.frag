#version 330 core
// File: shaders/beam_pool.frag (OpenGL volumetric light beam — light pool)
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
uniform vec3  uCameraPos;   // render space
uniform vec4  uFogColor;    // rgb, a = strength
uniform vec4  uFogEnv;      // (envStart, envEnd, rdStart, rdEnd)
#define CAMERA    uCameraPos
#define FOG_COLOR uFogColor
#define FOG_ENV   uFogEnv
// The scene-depth snapshot (VolumetricBeam::EnsureSceneDepth) — OpenGL only.
uniform sampler2D uSceneDepth;
uniform float     uHasSceneDepth;
uniform mat4      uSceneViewProj;   // render space → clip, the view's own
#define HAS_SCENE_DEPTH 1
uniform float uPoolDeferred;      // 1: box decal over the depth snapshot
uniform mat4  uPoolRenderToBox;   // render space → the box's [-1,1]³

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

#ifdef HAS_SCENE_DEPTH
// Distance along the ray eye + t·R to the scene at this pixel (1e9: sky).
// Solved in clip space — z(t)/w(t) = the stored depth, both linear in t — so
// it holds for any projection the view uses (bob, nausea, a portal's).
float sceneDistance(vec3 eye, vec3 R) {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(uSceneDepth, 0));
    float d = texture(uSceneDepth, uv).r;
    if (d >= 0.99999) return 1e9;
    float Z = d * 2.0 - 1.0;
    vec4 c0 = uSceneViewProj * vec4(eye, 1.0);
    vec4 cR = uSceneViewProj * vec4(R, 0.0);
    float den = cR.z - Z * cR.w;
    if (abs(den) < 1e-8) return 1e9;
    return max((Z * c0.w - c0.z) / den, 0.0);
}
#endif

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
