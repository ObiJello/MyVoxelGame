#version 330 core
// File: shaders/beam_volume.frag (OpenGL volumetric light beam — in-scattering)
// See src/client/renderer/effects/VolumetricBeam.hpp for the model.
in vec3 vRenderPos;
out vec4 FragColor;

uniform sampler2D uNoiseTex;
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

const int   kMaxSteps = 6;
const float kClipSoft = 1.75;   // the soft end at a terrain clip (P6.w)

// Interleaved gradient noise: the per-pixel sample offset (no banding).
float ign(vec2 c) { return fract(52.9829189 * fract(dot(c, vec2(0.06711056, 0.00583715)))); }

// 3D value noise off the 2D noise texture: G holds slice z, R slice z+1
// (VolumetricBeam.cpp NoisePixels), one bilinear fetch.
float valueNoise3(vec3 x) {
    vec3 p = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    vec2 uv = p.xy + vec2(37.0, 17.0) * p.z + f.xy;
    vec2 rg = textureLod(uNoiseTex, (uv + 0.5) / 64.0, 0.0).yx;
    return mix(rg.x, rg.y, f.z);
}

// Drifting mist and dust: billows (1/8 block) and wisps (1/2 block), each on
// its own slow wind, around a mean of about 1. Anchored to the apex (q), so
// it is fixed in the world while the lamp is; P4.y decorrelates beams.
float mistAt(vec3 q) {
    vec3 w = q + P4.y * vec3(17.13, 5.71, 11.37);
    float t = P4.x;
    float n1 = valueNoise3((w + vec3(0.5, 0.0625, 0.25) * t) * 0.125);
    float n2 = valueNoise3((w + vec3(-0.75, 0.125, 0.5) * t) * 0.5);
    float n = 0.62 * n1 + 0.38 * n2;
    return mix(1.0, 0.15 + 1.9 * smoothstep(0.22, 0.78, n), P4.z);
}

// Henyey-Greenstein, normalised to 1 seen side-on (mu = 0), over a floor so
// the back-scatter is never black. mu = cos(light direction, direction to
// the eye): looking back down the beam at the lamp is mu -> 1.
float phase(float mu, float g) {
    float g2 = g * g;
    float den = max(1.0 + g2 - 2.0 * g * mu, 1e-3);
    return 0.3 + 0.7 * pow((1.0 + g2) / den, 1.5);
}

// The ray's segment inside the cone |x-A|^2 - s^2 <= (r0 + k s)^2, 0 <= s <= sEnd.
// CA = origin - apex. f(t) = a t^2 + 2 b t + c; the slab cuts off the other
// nappe, so the answer is always one interval.
bool coneInterval(vec3 CA, vec3 R, vec3 D, float r0, float k, float sEnd, out vec2 seg) {
    seg = vec2(0.0);
    float sc = dot(CA, D);
    float rd = dot(R, D);
    float rc = r0 + k * sc;
    float a = 1.0 - (1.0 + k * k) * rd * rd;
    float b = dot(CA, R) - rd * (sc + k * rc);
    float c = dot(CA, CA) - sc * sc - rc * rc;

    float s0, s1;
    if (abs(rd) > 1e-6) {
        float ta = -sc / rd;
        float tb = (sEnd - sc) / rd;
        s0 = min(ta, tb);
        s1 = max(ta, tb);
    } else {
        if (sc < 0.0 || sc > sEnd) return false;
        s0 = -1e9;
        s1 = 1e9;
    }

    float t0 = -1e9;
    float t1 = 1e9;
    float disc = b * b - a * c;
    if (abs(a) > 1e-6) {
        if (disc >= 0.0) {
            float sq = sqrt(disc);
            float ra = (-b - sq) / a;
            float rb = (-b + sq) / a;
            float lo = min(ra, rb);
            float hi = max(ra, rb);
            if (a > 0.0) {
                t0 = lo;
                t1 = hi;
            } else {
                // Steeper than the cone: inside on two half-lines, one per
                // nappe. The slab keeps the forward one.
                vec2 i1 = vec2(s0, min(s1, lo));
                vec2 i2 = vec2(max(s0, hi), s1);
                seg = (i1.y - i1.x) >= (i2.y - i2.x) ? i1 : i2;
                return seg.y > seg.x;
            }
        } else if (a > 0.0) {
            return false;
        }
    } else if (abs(b) > 1e-6) {
        // Parallel to a generator: f is linear.
        float tr = -c / (2.0 * b);
        if (b > 0.0) t1 = tr; else t0 = tr;
    } else if (c > 0.0) {
        return false;
    }
    seg = vec2(max(t0, s0), min(t1, s1));
    return seg.y > seg.x;
}

void main() {
    vec3 eye = CAMERA;
    vec3 R = normalize(vRenderPos - eye);
    vec3 A = P0.xyz;
    vec3 D = P1.xyz;
    float L = P0.w;
    float clipEnd = min(P6.w, L);
    float k = (P2.w - P1.w) / L;

    // Intersect from the point of the ray nearest the cone's middle: far
    // from the lamp the quadratic's terms are large and nearly cancel.
    float tShift = max(dot(A + D * (0.5 * clipEnd) - eye, R), 0.0);
    vec2 seg;
    if (!coneInterval(eye + R * tShift - A, R, D, P1.w, k, clipEnd, seg)) discard;
    float tNear = max(seg.x + tShift, 0.0);
    float tFar  = seg.y + tShift;
#ifdef HAS_SCENE_DEPTH
    if (uHasSceneDepth > 0.5) tFar = min(tFar, sceneDistance(eye, R));
#endif
    if (tFar <= tNear + 1e-3) discard;
    // Past the fog's end nothing reaches the eye.
    if (1.0 - fogAt(R * tNear) <= 0.002) discard;

    float len = tFar - tNear;
    int steps = int(clamp(ceil(len / 3.0), 3.0, float(kMaxSteps)));
    float dt = len / float(steps);
    float jitter = ign(gl_FragCoord.xy);
    float g = P4.w;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < kMaxSteps; ++i) {
        if (i >= steps) break;
        float t = tNear + (float(i) + jitter) * dt;
        vec3 q = eye + R * t - A;
        float s;
        vec3 light = beamLight(q, s);
        float clipFade = 1.0 - smoothstep(clipEnd - kClipSoft, clipEnd, s);
        float mu = -dot(q, R) * inversesqrt(max(dot(q, q), 1e-4));
        float T = 1.0 - fogAt(q + A - eye);
        acc += light * (clipFade * phase(mu, g) * mistAt(q) * T);
    }
    // In-scattered light: Σ · Δt · haze density · intensity, then a soft
    // shoulder instead of a clipped white. Added to the frame (ONE, ONE).
    acc *= dt * P5.x * P3.w;
    FragColor = vec4(1.0 - exp(-acc), 0.0);
}
