// File: shaders/portal_vk.frag
// Vulkan version of the portal renderer fragment shader. Same algorithm
// as the OpenGL portal.frag — both procedural PortalFlame and the
// texture-driven PortalRefract path (Valve's Stage 2 rim fire effect)
// are present, so portals look *identical* on both backends.
//
// Texture bindings (mapped via VKBackend's portal pipeline layout):
//   set=0 binding=0 — uPortalNoiseTex  (slot 0)
//   set=2 binding=0 — uPortalColorTex  (slot 1)
// UBO bindings:
//   set=1 binding=0 — CommonUBO        (all the scalar uniforms)
#version 450

layout(set = 0, binding = 0) uniform sampler2D uPortalNoiseTex;
layout(set = 2, binding = 0) uniform sampler2D uPortalColorTex;

layout(std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;
    mat4  uModel_;
    vec4  uPortalColor_;    // rgb=color, w=uPulse
    vec4  uColorDark_;      // rgb=dark,  w=uOpenAmount
    vec4  uColorHot_;       // rgb=hot,   w=uOpenAmountVS
    vec4  uKeyDir_;
    vec4  uTint_;
    vec4  uUVRange_;
    vec4  uScalarsA_;       // (uTime, uTimeVS, uStaticAmount, uColorScale)
    vec4  uScalarsB_;       // (uPortalActive, uForceFarDepth, uOutlineMode, uFlashIntensity)
    vec4  uScalarsC_;
    vec4  uScalarsD_;       // (uHasSprite, uUseSkin, uUseTextures, _pad)
    vec2  uScreenSize_;
    vec2  _pad_;
} U;

#define uPortalColor    U.uPortalColor_.rgb
#define uPulse          U.uPortalColor_.w
#define uColorDark      U.uColorDark_.rgb
#define uOpenAmount     U.uColorDark_.w
#define uColorHot       U.uColorHot_.rgb
#define uOpenAmountVS   U.uColorHot_.w
#define uTime           U.uScalarsA_.x
#define uTimeVS         U.uScalarsA_.y
#define uStaticAmount   U.uScalarsA_.z
#define uColorScale     U.uScalarsA_.w
#define uPortalActive   U.uScalarsB_.x
#define uForceFarDepth  U.uScalarsB_.y
#define uOutlineMode    U.uScalarsB_.z
#define uFlashIntensity U.uScalarsB_.w
#define uUseTextures    U.uScalarsD_.z

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vNoiseUV;
layout(location = 0) out vec4 FragColor;

// ------- Helpers (verbatim from portal.frag) -------
float linearstep(float a, float b, float v) { return clamp((v - a) / (b - a), 0.0, 1.0); }

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i + vec2(0.0, 0.0));
    float b = hash21(i + vec2(1.0, 0.0));
    float c2 = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c2, d, f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += a * valueNoise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

// ------- PortalFlame (procedural fallback) -------
vec4 PortalFlame(bool transparentCenter) {
    vec2  cv      = vUV - vec2(0.5);
    float radial = length(cv) / 0.5;
    if (radial > 1.0) discard;

    float angle = atan(cv.y, cv.x);
    float t     = uTime;

    vec2 ringCW  = vec2(cos(angle + t * 0.13), sin(angle + t * 0.13));
    vec2 ringCCW = vec2(cos(angle - t * 0.08), sin(angle - t * 0.08));

    vec2 warp = vec2(
        fbm(ringCW * 1.7 + vec2(t * 0.18,  0.0)),
        fbm(ringCW * 1.7 + vec2(0.0,       t * 0.18 + 31.4))
    ) * 0.30;

    float n1 = fbm((ringCW  + warp) * 2.4  + vec2(t * 0.45, 0.0));
    float n2 = fbm( ringCCW         * 5.0  + vec2(t * 0.80, 7.3));
    float n3 = fbm( ringCW          * 11.0 + vec2(t * 1.20, 13.1));

    float flame = n1 * 0.55 + n2 * 0.30 + n3 * 0.15;
    flame = smoothstep(0.30, 0.75, flame);

    const float kFixedReach = 0.30;
    float baseIntensity = smoothstep(1.0 - kFixedReach, 1.0, radial);
    baseIntensity = pow(baseIntensity, 1.45);

    float brightnessMod = mix(0.78, 1.00, flame);
    float intensity = baseIntensity * brightnessMod;

    {
        vec2  lineRing = vec2(cos(angle), sin(angle)) * 4.5;
        float lineNoise = valueNoise(lineRing + vec2(t * 0.55, 0.0));
        float lineMask = smoothstep(0.72, 0.92, lineNoise);
        float lineRadialFade = smoothstep(0.45, 0.82, radial)
                             * (1.0 - smoothstep(0.92, 1.00, radial));
        intensity += lineMask * lineRadialFade * 0.18;
    }

    const float kCrestCenter = 0.93;
    const float kCrestSigma  = 0.025;
    float crestArg = (radial - kCrestCenter) / kCrestSigma;
    float crest = exp(-crestArg * crestArg);

    vec3 col;
    if (intensity < 0.35) {
        col = mix(uColorDark, uPortalColor, intensity / 0.35);
    } else if (intensity < 0.75) {
        col = mix(uPortalColor, uColorHot, (intensity - 0.35) / 0.40);
    } else {
        col = uColorHot;
    }
    col = mix(col, uColorHot * 1.18, crest * 0.65);

    float brightnessPulse = 0.95 + 0.05 * sin(t * 1.20);
    col *= brightnessPulse;

    if (uFlashIntensity > 0.001) {
        col = mix(col, uColorHot * 1.6, uFlashIntensity * 0.85);
    }

    if (transparentCenter) {
        float a = intensity * uOpenAmount;
        if (uStaticAmount > 0.001) {
            float spiral1 = sin(angle * 3.0 + radial * 8.0 + t * 0.55);
            float spiral2 = sin(angle * 5.0 - radial * 6.0 - t * 0.40);
            float vortex  = (spiral1 + spiral2) * 0.25 + 0.5;
            float radialFade = smoothstep(0.0, 0.35, radial);
            vortex = mix(0.40, 1.00, vortex * radialFade);
            vec3  staticCol = uColorDark * mix(0.12, 0.42, vortex);
            return vec4(mix(col, staticCol, 1.0 - a) * (a + uStaticAmount),
                        max(a, uStaticAmount));
        }
        return vec4(col, a);
    } else {
        float spiral1 = sin(angle * 3.0 + radial * 8.0 + t * 0.55);
        float spiral2 = sin(angle * 5.0 - radial * 6.0 - t * 0.40);
        float vortex  = (spiral1 + spiral2) * 0.25 + 0.5;
        float radialFade = smoothstep(0.0, 0.35, radial);
        vortex = mix(0.40, 1.00, vortex * radialFade);
        vec3 centerCol = uColorDark * mix(0.12, 0.42, vortex);
        vec3 finalCol  = mix(centerCol, col, intensity * uOpenAmount);
        return vec4(finalCol, 1.0);
    }
}

// ------- PortalRefract (Valve's Stage 2 — same as portal.frag) -------
vec4 PortalRefract(bool /*transparentCenter*/) {
    const float kOuterBorder    = 0.075;
    const float kInnerBorder    = kOuterBorder * 4.0;
    const float kBorderSoftness = 0.875;

    float openAmt   = smoothstep(0.0, 1.0, clamp(uOpenAmount, 0.0, 1.0));
    float openAmtSq = openAmt * openAmt;

    vec2  stretchVec      = vUV * 2.0 - 1.0;
    float distFromCenter  = length(stretchVec);
    if (distFromCenter > 1.0 + kOuterBorder) discard;

    float stencilCutout = step(distFromCenter, openAmtSq);
    float outerMask = (1.0 - linearstep(openAmtSq, openAmtSq + kOuterBorder, distFromCenter))
                    * (1.0 - stencilCutout);
    float innerMask = linearstep(openAmtSq - kInnerBorder, openAmtSq, distFromCenter)
                    * stencilCutout;
    float portalActive  = clamp(uPortalActive, 0.0, 1.0);
    float effectFadeIn  = max(clamp(openAmt * 2.5, 0.0, 1.0), 1.0 - portalActive);
    float effectMask = (innerMask + outerMask) * effectFadeIn;

    vec4 noise1 = texture(uPortalNoiseTex, vNoiseUV.xy);
    vec4 noise2 = texture(uPortalNoiseTex, vNoiseUV.wz - noise1.rg * 0.02);
    noise1      = texture(uPortalNoiseTex, vNoiseUV.xy - noise2.rg * 0.02);

    float noiseVal = (noise1.g + noise2.g) * 0.5;
    float portalActiveWithNoise = smoothstep(0.0, noiseVal, portalActive);

    float borderMaskNoise = 1.0 - smoothstep(
        effectMask - kBorderSoftness,
        effectMask + kBorderSoftness,
        noiseVal);
    noiseVal    = borderMaskNoise;
    effectMask *= borderMaskNoise;

    float transparency = clamp(
        effectMask + stencilCutout * (1.0 - portalActiveWithNoise),
        0.0, 1.0) * 1.5;

    float btShift = pow(abs(1.0 - vUV.y), 1.5) * 0.8 + 0.2;
    float rampU = clamp(pow(noiseVal, 0.5) * btShift * transparency, 0.0, 1.0);
    vec3 flameColor = texture(uPortalColorTex, vec2(rampU, 0.5)).rgb;
    flameColor *= uColorScale;

    if (uFlashIntensity > 0.001) {
        vec3 hot = texture(uPortalColorTex, vec2(0.95, 0.5)).rgb * uColorScale;
        flameColor = mix(flameColor, hot * 1.4, uFlashIntensity * 0.75);
    }

    return vec4(flameColor, clamp(transparency, 0.0, 1.0));
}

void main() {
    if (uForceFarDepth > 0.5) {
        gl_FragDepth = 1.0;
    } else {
        gl_FragDepth = gl_FragCoord.z;
    }
    // Match portal.frag exactly so OpenGL and Vulkan are pixel-identical.
    if (uOutlineMode > 1.5) {
        FragColor = (uUseTextures > 0.5) ? PortalRefract(false) : PortalFlame(false);
    } else if (uOutlineMode > 0.5) {
        FragColor = (uUseTextures > 0.5) ? PortalRefract(true)  : PortalFlame(true);
    } else {
        // Solid fill — stencil mark / depth clear / depth refill passes.
        vec2  c           = vUV - vec2(0.5);
        float dist        = length(c) * 2.0;
        float solidOpen   = smoothstep(0.0, 1.0, clamp(uOpenAmount, 0.0, 1.0));
        float solidOpenSq = solidOpen * solidOpen;
        if (dist > solidOpenSq) discard;
        FragColor = vec4(uPortalColor, 1.0);
    }
}
