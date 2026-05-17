// File: src/client/renderer/portal/PortalRenderer.cpp
// See PortalRenderer.hpp for high-level scope.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "PortalRenderer.hpp"
#include "PortalCameraTransform.hpp"
#include "../backend/RenderBackend.hpp"
#ifdef HAS_VULKAN
#include "../backend/vulkan/VKBackend.hpp"
#endif
#include "../mesh/ChunkRenderer.hpp"     // RenderChunksAll for the see-through scene re-render
#include "client/portal/ClientPortalManager.hpp"
#include "common/core/Log.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include "stb_image.h"
#include <vector>
#include <cmath>

namespace Render {

    PortalRenderer g_portalRenderer;

    // 96-segment ellipse triangle fan — visual oval inscribed in the 1×2
    // collision rectangle. The collision/teleport volume in PortalRegistry
    // is the full 1×2 rectangle (so stepping anywhere in the block face
    // counts as entering); the visual is just the oval cutout for the
    // Portal-game-style aesthetic. Higher segment count keeps the rim
    // smooth at close viewing distance — the flame outline is the most
    // visible feature so any polygonal aliasing on the silhouette is
    // immediately obvious. 96 segs × 3 verts/tri × 1 tri/seg = 288 verts,
    // negligible memory.
    static constexpr int   kSegments       = 96;
    static constexpr float kHalfWidth      = 0.5f;     // ±X extent in portal-local space (1 wide)
    // Portal-exact aspect ratio: real Portal's disc is 64×108 inches
    // → ratio 1 : 1.6875. With kHalfWidth = 0.5, kHalfHeight should be
    // 0.5 / (64/108) = 0.84375 to match the visual proportion (was 1.0
    // = pure 1×2 block ratio). Collision/teleport bounds still use the
    // full 1×2 block opening — only the visual oval is narrower.
    static constexpr float kHalfHeight     = 0.84375f; // ±Y extent (Portal-exact 1:1.6875)
    // Pulse oscillates BELOW 1.0 — the portal breathes between
    // (1 - kPulseAmplitude)×scale and 1.0×scale, never larger than its
    // nominal 1×2 block size (so it stays within the wall face).
    static constexpr float kPulseAmplitude = 0.04f;   // breathes down by 4% at trough
    static constexpr float kPulseFreqHz    = 0.35f;   // ~3 second period — slow breathing

    // Vertex layout reuse: pos3 + uv2 + color4 ubyte. The shader ignores UV
    // and per-vertex color (we tint via a uniform), but reusing the layout
    // means the existing GetBlockVertexLayout() machinery applies as-is.
    const char* PortalRenderer::vertexShaderSource = R"(
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4  uMVP;
uniform float uPulse;          // breathing scale ∈ [1-amp, 1.0]
uniform float uTimeVS;         // wall-clock seconds (drives noise scroll)
uniform float uOpenAmountVS;   // 0→1 (must match PS uOpenAmount)

out vec2 vUV;
out vec4 vNoiseUV;             // dual UVs for flow distortion (xy + wz)

void main() {
    // Pulse the oval ever so slightly along its in-plane axes only — keep
    // z (the wall-normal direction) flat so the surface offset doesn't
    // change. (The portal lives at z=0 in its local frame.)
    vec3 pos = vec3(aPos.x * uPulse, aPos.y * uPulse, aPos.z);
    gl_Position = uMVP * vec4(pos, 1.0);

    // Base UV with outer-border adjustment — matches Valve's portal_refract_vs20.fxc
    // (kFlPortalOuterBorder = 0.075). Shrinks the effective portal rect
    // slightly so the rim flame has room to breathe outside the silhouette.
    const float kOuterBorder = 0.075;
    vUV = aUV * (1.0 + kOuterBorder) - vec2(kOuterBorder * 0.5);

    // Noise UV — center-normalize by openAmount so the noise scales with
    // the opening, then scale by kNoiseScale and scroll by time. Two UV
    // sets, scrolling in opposite directions, used for flow distortion in
    // the fragment shader. Algorithm from Valve's portal_refract_vs20.fxc.
    float openAmtVS = clamp(uOpenAmountVS + 0.001, 0.0, 1.0); // avoid div by 0
    const float kNoiseScale  = 0.3;
    const float kScrollRate  = 0.0275;
    float scroll = uTimeVS * kScrollRate;
    vec2 noiseBase = (aUV - 0.5) / openAmtVS + 0.5;
    vNoiseUV.xy = noiseBase * kNoiseScale + vec2( scroll, 0.0);
    vNoiseUV.zw = noiseBase * kNoiseScale - vec2( scroll, 0.0); // PS reads .wz to swap axes
}
)";

    const char* PortalRenderer::fragmentShaderSource = R"(
#version 330 core

// Direct port of Valve's portal_refract_ps2x.fxc (Source SDK 2013,
// Stage 2 — the rim fire effect). Bindings:
//   uPortalNoiseTex : 256×256 noise texture (Portal's noise-blur-256x256.vtf).
//                     Sampled twice with flow-distortion offsets.
//   uPortalColorTex : 256×1 1D color ramp (portal-{blue,orange}-color.vtf).
//                     Sampled at U = pow(noise, 0.5) × bottomToTop × transparency.
//   uColorScale     : VMT $PortalColorScale (4.0 by default). Brightens
//                     ramp output for emissive look + HDR bloom.
//   uPortalActive   : 1.0 - staticAmount. 1.0 = fully active (rim only),
//                     0.0 = inactive (vortex fills the centre).
// Other uniforms inherited from the rest of the portal pipeline below.
//
// The legacy uPortalColor/uColorDark/uColorHot remain only as fallback
// inputs to the procedural PortalFlame() path below (used when textures
// fail to load).
uniform sampler2D uPortalNoiseTex;
uniform sampler2D uPortalColorTex;
uniform float     uColorScale;
uniform float     uPortalActive;
uniform int       uUseTextures;   // 1 = sample noise+ramp (PortalRefract), 0 = procedural fallback
uniform vec3  uPortalColor;
uniform vec3  uColorDark;
uniform vec3  uColorHot;
// 0 = output the fragment's interpolated depth (default).
// 1 = force gl_FragDepth = 1.0 (far). Used by the see-through pass's
//     "depth clear inside silhouette" sub-pass.
uniform float uForceFarDepth;
// 0 = solid fill (uPortalColor, alpha=1) — used by stencil-mark, depth-
//     clear, depth-refill (color writes are off in those passes anyway).
// 1 = ACTIVE flame outline. Transparent centre lets the see-through
//     world show through; rim is the colored Portal-game flame.
// 2 = INACTIVE flame outline. Opaque BLACK centre (no destination paired
//     yet); rim is the colored flame.
uniform float uOutlineMode;
// Wall-clock seconds, used to animate the flame noise. Same value passed
// each frame via SetUniformFloat — drives the flicker / breathing of the
// flame tongues so the portal feels alive instead of static.
uniform float uTime;
// Teleport flash intensity in [0, 1]. 0 = idle. 1 = peak (right after
// teleport). Decays to 0 over kFlashDurationSec seconds wall-clock. The
// renderer computes (flashEndTime - now) / duration each frame and
// passes it as this uniform.
uniform float uFlashIntensity;
// Portal-game-authoritative animation state (c_prop_portal.cpp).
//   uOpenAmount   : 0 → 1 over 0.5s when portal is first placed
//   uStaticAmount : 1 → 0 over 1.0s when paired portal gets re-fired
// Both default to (open=1, static=0) = "fully open, no static" so old
// callers that don't set them still render correctly.
uniform float uOpenAmount;
uniform float uStaticAmount;

in  vec2 vUV;
in  vec4 vNoiseUV;            // dual scrolling UVs from VS
out vec4 FragColor;

// linearstep — Valve's clamp((v-a)/(b-a), 0, 1). GLSL's smoothstep adds
// a cubic ease which we don't want here; linearstep stays linear.
float linearstep(float a, float b, float v) {
    return clamp((v - a) / (b - a), 0.0, 1.0);
}

// ────────── GPU-friendly value-noise + fBM ──────────
// Hash → smoothed value noise → fractional Brownian motion. Standard
// shadertoy pattern. We need NO external textures and NO derivatives,
// just pseudo-random per-frame animation.

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);   // smoothstep
    float a = hash21(i + vec2(0.0, 0.0));
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
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

// ────────── Portal flame (Portal-game-faithful) ──────────
//
// Source-engine-inspired portal effect using purely procedural noise.
// Tuned to match Portal's actual look as closely as we can without HDR /
// bloom / refraction / particles:
//   1. CLEAN GEOMETRIC RING — noise modulates BRIGHTNESS within a
//      fixed-thickness ring instead of carving the ring's inner edge into
//      flame tongues. Portal's ring reads as a ring, not as fire.
//   2. THIN RING — kFixedReach = 0.30 (was 0.58). The see-through area
//      dominates the disc, like Portal.
//   3. NO INNER HALO BLEED — alpha at the centre is exactly 0; the
//      see-through is clean (no tinted haze inside the ring).
//   4. HOT CREST INWARD — peak brightness sits at radial 0.93 (slightly
//      inside the geometric rim), matching Portal where the bright
//      crescent is JUST INWARD from the silhouette edge.
//   5. SATURATED DARK TIPS — no luma-desat; deep portal color stays
//      vivid all the way to the inner edge of the ring.
//   6. Counter-rotating noise + domain warp + 3 octaves still in place,
//      but contrast softened (smoothstep 0.30..0.75 instead of 0.18..0.82)
//      so the wisps are FEATHERED edges of the ring, not distinct tongues.
vec4 PortalFlame(bool transparentCenter) {
    vec2  c      = vUV - vec2(0.5);
    float radial = length(c) / 0.5;          // 0 at centre, 1 at rim
    if (radial > 1.0) discard;

    float angle = atan(c.y, c.x);             // -π to π, continuous
    float t     = uTime;

    // Counter-rotating sample positions — shimmer.
    vec2 ringCW  = vec2(cos(angle + t * 0.13), sin(angle + t * 0.13));
    vec2 ringCCW = vec2(cos(angle - t * 0.08), sin(angle - t * 0.08));

    // Domain warping — slight curl to the noise field so the ring
    // brightness modulation looks organic, not mechanical.
    vec2 warp = vec2(
        fbm(ringCW * 1.7 + vec2(t * 0.18,  0.0)),
        fbm(ringCW * 1.7 + vec2(0.0,       t * 0.18 + 31.4))
    ) * 0.30;

    // Three octaves of noise.
    float n1 = fbm((ringCW  + warp) * 2.4  + vec2(t * 0.45, 0.0));
    float n2 = fbm( ringCCW         * 5.0  + vec2(t * 0.80, 7.3));
    float n3 = fbm( ringCW          * 11.0 + vec2(t * 1.20, 13.1));

    // Combined noise field. Softer contrast → feathered edges, not
    // distinct tongues. Keep it gentle.
    float flame = n1 * 0.55 + n2 * 0.30 + n3 * 0.15;
    flame = smoothstep(0.30, 0.75, flame);

    // CLEAN RING — geometric thickness FIXED. Noise modulates BRIGHTNESS
    // within the ring, not the ring's inner edge position.
    const float kFixedReach = 0.30;
    float baseIntensity = smoothstep(1.0 - kFixedReach, 1.0, radial);
    baseIntensity = pow(baseIntensity, 1.45);

    // Brightness modulation — noise varies brightness 78%..100% around
    // the ring. Tongues read as "lighter" / "darker" patches within the
    // ring shape, not as protrusions.
    float brightnessMod = mix(0.78, 1.00, flame);
    float intensity = baseIntensity * brightnessMod;

    // ── Radial speed-lines (item #7) ──
    // Thin lighter angular streaks suggesting energy flowing outward
    // along the ring. High-freq noise sampled on a unit circle (so it
    // wraps seamlessly at -π/+π) identifies "lit" angular channels;
    // streaks appear as bright lines crossing the outer half of the ring.
    {
        vec2  lineRing = vec2(cos(angle), sin(angle)) * 4.5;
        float lineNoise = valueNoise(lineRing + vec2(t * 0.55, 0.0));
        // Narrow threshold → thin streaks. ~5-10% of the angular range
        // is "lit" at any moment.
        float lineMask = smoothstep(0.72, 0.92, lineNoise);
        // Radial profile: emerge from inside the ring, fade out before
        // the outer rim so they don't fight the hot crest.
        float lineRadialFade = smoothstep(0.45, 0.82, radial)
                             * (1.0 - smoothstep(0.92, 1.00, radial));
        intensity += lineMask * lineRadialFade * 0.18;
    }

    // Hot crest — Gaussian peak at radial 0.93 (slightly INWARD from the
    // geometric rim, matching Portal's bright crescent). Sigma ≈ 0.025
    // gives a thin band ~5% of the disc wide.
    const float kCrestCenter = 0.93;
    const float kCrestSigma  = 0.025;
    float crestArg = (radial - kCrestCenter) / kCrestSigma;
    float crest = exp(-crestArg * crestArg);

    // ── Color ramp ── three saturated stops, no desaturation.
    vec3 col;
    if (intensity < 0.35) {
        col = mix(uColorDark, uPortalColor, intensity / 0.35);
    } else if (intensity < 0.75) {
        col = mix(uPortalColor, uColorHot, (intensity - 0.35) / 0.40);
    } else {
        col = uColorHot;
    }
    // Hot crest band — bright tinted lift, stays in hot color's hue.
    col = mix(col, uColorHot * 1.18, crest * 0.65);

    // Subtle brightness pulse — slow ~1.2 Hz, ±5% amplitude.
    float brightnessPulse = 0.95 + 0.05 * sin(t * 1.20);
    col *= brightnessPulse;

    // Teleport flash boost — adds a strong tinted-hot brightness lift
    // over the entire flame. Flash starts at 1.0 and decays to 0 over
    // ~0.35s, so the curve here is just a linear scale from the uniform.
    // Quadratic mix toward hot so the flash reads as a vivid pop rather
    // than a uniform brightening (matches Portal's "energy spike").
    if (uFlashIntensity > 0.001) {
        col = mix(col, uColorHot * 1.6, uFlashIntensity * 0.85);
    }

    if (transparentCenter) {
        // ACTIVE: alpha gradient. NO inner halo bleed — centre is exactly
        // alpha=0 so the see-through view is perfectly clean.
        //
        // Portal's "static ping" (uStaticAmount > 0): when this portal's
        // pair-mate was just re-fired, fade the inactive vortex pattern
        // back IN over the see-through. Mirrors materialproxy_portalstatic.
        // cpp's ComputeStaticAmount, which the real game uses to lerp
        // between portalstaticoverlay and the dynamic see-through mesh.
        // Plus an open-anim ramp: while the portal is opening (uOpenAmount
        // < 1), the rim fades up from 0 over 0.5s.
        float a = intensity * uOpenAmount;
        if (uStaticAmount > 0.001) {
            float spiral1 = sin(angle * 3.0 + radial * 8.0 + t * 0.55);
            float spiral2 = sin(angle * 5.0 - radial * 6.0 - t * 0.40);
            float vortex  = (spiral1 + spiral2) * 0.25 + 0.5;
            float radialFade = smoothstep(0.0, 0.35, radial);
            vortex = mix(0.40, 1.00, vortex * radialFade);
            vec3  staticCol = uColorDark * mix(0.12, 0.42, vortex);
            // Static pulls the centre alpha up from 0 → 1 as it ramps,
            // veiling the see-through with the dark vortex pattern.
            return vec4(mix(col, staticCol, 1.0 - a) * (a + uStaticAmount),
                        max(a, uStaticAmount));
        }
        return vec4(col, a);
    } else {
        // INACTIVE (item #10): structured polar swirl vortex — Portal's
        // inactive portals have an organized spinning pattern, NOT
        // random noise. We compose two counter-spinning sine waves at
        // different angular and radial frequencies; the result is a set
        // of concentric "energy lines" that slowly rotate.
        //
        //   spiral1: 3 angular periods, expanding outward at radial-rate 8,
        //            spinning CW
        //   spiral2: 5 angular periods, expanding inward at radial-rate 6,
        //            spinning CCW
        // Their sum gives a moiré-like vortex pattern.
        float spiral1 = sin(angle * 3.0 + radial * 8.0 + t * 0.55);
        float spiral2 = sin(angle * 5.0 - radial * 6.0 - t * 0.40);
        float vortex  = (spiral1 + spiral2) * 0.25 + 0.5;        // 0..1

        // Fade the vortex toward the very centre so the brightest lines
        // are in the mid-ring area (matches Portal's "deep well" look).
        float radialFade = smoothstep(0.0, 0.35, radial);
        vortex = mix(0.40, 1.00, vortex * radialFade);

        vec3 centerCol = uColorDark * mix(0.12, 0.42, vortex);
        // Rim fades in via uOpenAmount during the 0.5s open animation
        // (matches Portal's `m_fOpenAmount += dt * 2.0`). Centre disc
        // stays opaque the whole time.
        vec3 finalCol  = mix(centerCol, col, intensity * uOpenAmount);
        return vec4(finalCol, 1.0);
    }
}

// ─── PortalRefract Stage 2 — direct port of Valve's portal_refract_ps2x.fxc ──
//
// Algorithm and constants from Source SDK 2013
// (src/materialsystem/stdshaders/portal_refract_ps2x.fxc, Stage 2 branch).
// Pixel-equivalent output to the real Portal portal-rim shader.
//
// Silhouette: derived from radial distance, NOT a mask texture. The
// "PortalMask" in Portal's VMT is actually the NOISE texture
// (noise-blur-256x256.vtf) sampled for flame distortion.
//
// `transparentCenter` is kept as the API hook but is now derived from
// uPortalActive (1.0 = active/centered transparent, 0.0 = inactive/filled).
vec4 PortalRefract(bool /*transparentCenter*/) {
    const float kOuterBorder    = 0.075;
    const float kInnerBorder    = kOuterBorder * 4.0;   // 0.30
    const float kBorderSoftness = 0.875;

    // Open amount — Valve smoothsteps then squares for the stencil cutout
    float openAmt   = smoothstep(0.0, 1.0, clamp(uOpenAmount, 0.0, 1.0));
    float openAmtSq = openAmt * openAmt;

    // Stretch vector + radial distance from centre. Valve's silhouette
    // is computed from the UV directly, not from a mask texture.
    vec2  stretchVec      = vUV * 2.0 - 1.0;
    float distFromCenter  = length(stretchVec);

    // Hard cutoff at the geometric mesh edge — NOT at the hole edge.
    // The VS pre-scales UV by (1 + kOuterBorder) = 1.075, so the rim
    // vertices of the mesh sit at distFromCenter == 1.075, not 1.0.
    // Valve's outerMask deliberately renders in the band [1.0, 1.075]
    // (the "outer border" past the hole edge — where the rim flame
    // extends OUTSIDE the see-through silhouette). Discarding at 1.0
    // throws away that entire band and leaves a transparent ring
    // between the rim and the visible disc edge.
    if (distFromCenter > 1.0 + kOuterBorder) discard;

    // Stencil cutout (1.0 inside the hole, 0.0 outside)
    float stencilCutout = step(distFromCenter, openAmtSq);

    // Outer effect mask — band just outside the hole, fading out over kOuterBorder
    float outerMask = (1.0 - linearstep(openAmtSq, openAmtSq + kOuterBorder, distFromCenter))
                    * (1.0 - stencilCutout);

    // Inner effect mask — band just inside the hole, ramping in over kInnerBorder
    float innerMask = linearstep(openAmtSq - kInnerBorder, openAmtSq, distFromCenter)
                    * stencilCutout;

    // Fade in as portal opens (or fade out as it deactivates / pings).
    // 1 - portalActive ensures the rim stays visible while inactive.
    float portalActive  = clamp(uPortalActive, 0.0, 1.0);
    float effectFadeIn  = max(clamp(openAmt * 2.5, 0.0, 1.0), 1.0 - portalActive);

    // Combined effect mask
    float effectMask = (innerMask + outerMask) * effectFadeIn;

    // ── Flow-distortion noise (Valve's signature trick) ──
    // Sample noise twice using offset/scrolling UVs from the VS, with
    // each sample's RG channel perturbing the OTHER sample's UV.
    vec4 noise1 = texture(uPortalNoiseTex, vNoiseUV.xy);
    vec4 noise2 = texture(uPortalNoiseTex, vNoiseUV.wz - noise1.rg * 0.02);
    noise1      = texture(uPortalNoiseTex, vNoiseUV.xy - noise2.rg * 0.02);

    // Average G channels — "more solid flames and calmer" per Valve comment.
    // Raw noise (no floor) — Portal allows the noise to dip to 0 for
    // transparent gaps in the flame tongues. smoothstep(0,0,x) is
    // well-defined per GLSL spec (returns step(0,x), not NaN); the
    // hot-pink artifact I worried about earlier was actually a texture-
    // binding bug fixed elsewhere.
    float noiseVal = (noise1.g + noise2.g) * 0.5;
    float portalActiveWithNoise = smoothstep(0.0, noiseVal, portalActive);

    // Border softened by noise
    float borderMaskNoise = 1.0 - smoothstep(
        effectMask - kBorderSoftness,
        effectMask + kBorderSoftness,
        noiseVal);
    noiseVal    = borderMaskNoise;
    effectMask *= borderMaskNoise;

    // Alpha — outer rim + inner cutout filled when inactive. The ×1.5
    // magic number from Valve's HLSL ("makes the flames thicker"). We
    // keep it for the COLOR-LOOKUP U coordinate (Valve's effect needs
    // transparency to be able to exceed 1 to push U into the bright end
    // of the ramp), but the OUTPUT alpha must clamp to [0,1] — many
    // GPUs leave src.a unclamped through srcAlpha blending and the
    // resulting `-0.5 * dst` term subtracts the wall colour from our
    // output, producing wild hue shifts (e.g. hot pink for blue rim
    // over gray walls).
    float transparency = clamp(
        effectMask + stencilCutout * (1.0 - portalActiveWithNoise),
        0.0, 1.0) * 1.5;

    // Bottom-to-top brightness shift — Valve's formula
    // (portal_refract_ps2x.fxc:197) `pow(abs(uv.y), 1.5) * 0.8 + 0.2`
    // wants BRIGHT at the BOTTOM of the disc. In our mesh, V=1 is
    // assigned to the +Y top vertex (sin(π/2) = 1). So we mirror
    // `vUV.y` before computing brightness to get bright-at-bottom.
    // (Earlier audit claimed this was a double-flip; testing confirmed
    // it's required — the audit was wrong about Valve's mesh UV
    // convention.)
    float btShift = pow(abs(1.0 - vUV.y), 1.5) * 0.8 + 0.2;

    // 1D color ramp lookup keyed on noise × brightness × transparency.
    // Clamp U into [0,1] so we don't depend on the sampler's wrap mode.
    float rampU = clamp(pow(noiseVal, 0.5) * btShift * transparency, 0.0, 1.0);
    vec3 flameColor = texture(uPortalColorTex, vec2(rampU, 0.5)).rgb;

    // $PortalColorScale (4.0 default) — pushes pixels into HDR so bloom kicks in
    flameColor *= uColorScale;

    // Teleport flash — bonus tint lift on the brightest portion of the ramp
    if (uFlashIntensity > 0.001) {
        vec3 hot = texture(uPortalColorTex, vec2(0.95, 0.5)).rgb * uColorScale;
        flameColor = mix(flameColor, hot * 1.4, uFlashIntensity * 0.75);
    }

    // Clamp output alpha to [0,1] for safe blending.
    return vec4(flameColor, clamp(transparency, 0.0, 1.0));
}

void main() {
    if (uForceFarDepth > 0.5) {
        gl_FragDepth = 1.0;
    } else {
        gl_FragDepth = gl_FragCoord.z;
    }

    // Texture-driven path (Portal-extracted mask + ramp) preferred.
    // Falls back to procedural PortalFlame() only if textures failed
    // to load (uUseTextures == 0).
    if (uOutlineMode > 1.5) {
        FragColor = (uUseTextures == 1) ? PortalRefract(false) : PortalFlame(false);
    } else if (uOutlineMode > 0.5) {
        FragColor = (uUseTextures == 1) ? PortalRefract(true)  : PortalFlame(true);
    } else {
        // Solid fill — stencil mark / depth clear / depth refill passes.
        // Limit to the HOLE region (dist < openAmtSq), matching Valve's
        // `flStencilCutout = step(distFromCenter, openAmountSquared)`
        // from portal_refract_ps2x.fxc:71. This makes the see-through
        // hole GROW from a tiny dot at the centre to the full disc as
        // the portal opens (0 → 0.5 s). Without the openAmtSq gate the
        // see-through view appears full-size from frame 1 even though
        // the rim is still ramping outward, which looks wrong.
        vec2  c          = vUV - vec2(0.5);
        float dist       = length(c) * 2.0;
        float solidOpen  = smoothstep(0.0, 1.0, clamp(uOpenAmount, 0.0, 1.0));
        float solidOpenSq = solidOpen * solidOpen;
        if (dist > solidOpenSq) discard;
        FragColor = vec4(uPortalColor, 1.0);
    }
}
)";

    // Refraction sub-pass fragment shader (item #9). Samples the captured
    // pre-refraction framebuffer (uSceneSnapshot) at rim-distorted UVs to
    // produce a subtle "energy field" wobble inside the silhouette,
    // matching Portal's refractive look at the rim.
    //
    // Distortion strength rises with radial distance — zero at the center,
    // peaking just inside the rim — so the see-through view stays clean
    // in the middle and only wobbles where the energy ring lives.
    // Distortion direction comes from a low-frequency noise sampled on
    // the unit circle (so it wraps), animated over time.
    // Direct port of PortalRefract Stage 0 from Valve's
    // portal_refract_ps2x.fxc:73-141. Two effects combined:
    //   1. Warp-around-hole: a tangent-space refraction that bends the
    //      see-through image radially outward as the portal opens
    //      (most-visible during the 0.5 s open animation, subtle after).
    //      Magnitude: `vTangentRefract = -stretchVec_unit × openAmt² ×
    //                  (1 - pow(distFromCenter, 64))`,
    //      applied via screen-space projected tangent + binormal vectors.
    //      `kPortalRadius = (32, 32)` per Valve's comment ("Should be
    //      32, 54 but this reduces the artifacts").
    //   2. Darkening ring: makes the portal stand out on plain walls.
    //      Persistent (not just during open). Mid-rim is ~0.85×
    //      luminance, fading back to 1.0 inward + outward of the
    //      midpoint.
    //
    // The portal's world-space tangent (right) + binormal (upDir) are
    // constant across the mesh, so we precompute their screen-space UV
    // directions on the C++ side (simplification of Valve's per-pixel
    // projection — accepts mild foreshortening error at extreme view
    // angles in exchange for not needing per-vertex tangent/binormal
    // attributes).
    const char* PortalRenderer::refractionFragmentSource = R"(
#version 330 core

uniform sampler2D uSceneSnapshot;       // _rt_Portal1 equivalent
uniform vec2      uScreenSize;          // framebuffer pixels
uniform vec2      uScreenTangent;       // screen-UV direction of portal +right
uniform vec2      uScreenBinormal;      // screen-UV direction of portal +up
uniform float     uOpenAmount;          // 0→1 over 0.5 s after firing

in  vec2 vUV;
out vec4 FragColor;

float linearstep(float a, float b, float v) {
    return clamp((v - a) / max(b - a, 1e-6), 0.0, 1.0);
}

void main() {
    // ── Stage 0: warp pixels around hole ──
    vec2  stretchVec     = vUV * 2.0 - 1.0;
    float distFromCenter = length(stretchVec);
    if (distFromCenter > 1.0) discard;

    vec2 stretchVecUnit = distFromCenter > 1e-6
        ? stretchVec / distFromCenter
        : vec2(0.0);

    float openAmt   = smoothstep(0.0, 1.0, clamp(uOpenAmount, 0.0, 1.0));
    float openAmtSq = openAmt * openAmt;

    // Tangent-space refraction offset (Valve exact)
    vec2 vTangentRefract = -stretchVecUnit * openAmtSq
                         * (1.0 - pow(clamp(distFromCenter, 0.0, 1.0), 64.0));
    vTangentRefract *= smoothstep(openAmt * 1.5, openAmt, distFromCenter);
    // Valve's kPortalRadius = (32, 32) is in HU but our uScreenTangent
    // already scales by world-position-per-screen-UV (it's the screen
    // displacement of a UNIT WORLD VECTOR — portal.right is 1m long).
    // So the multiplier here is just a magnitude tuning constant; not
    // a unit conversion. Portal's effective offset = `vTangentRefract`
    // × vProjTangent, and vProjTangent is small (~0.001–0.01 UV per HU
    // depending on viewing distance). To match Portal's subtle warp,
    // we use a small kPortalRadius so the final offset is ~1–2% UV at
    // most — without this scaling the see-through view sucks toward
    // the centre ("pinched into the middle").
    const vec2 kPortalRadius = vec2(0.04, 0.04);
    vTangentRefract *= kPortalRadius;

    // Convert tangent-space offset into screen-UV offset via the
    // pre-computed screen-space tangent/binormal directions
    vec2 screenUV_noWarp = gl_FragCoord.xy / uScreenSize;
    vec2 refractOffset = vTangentRefract.x * uScreenTangent
                       - vTangentRefract.y * uScreenBinormal;
    vec2 sampleUV = clamp(screenUV_noWarp + refractOffset, 0.0, 1.0);

    vec3 cRefract = texture(uSceneSnapshot, sampleUV).rgb;

    // Darkening ring (Valve exact, portal_refract_ps2x.fxc:124-128)
    float flHoleEdge = openAmtSq;
    float flDimEdge  = clamp(openAmt * 2.0, 0.0, 1.0);
    float flDarkeningRing = linearstep(flHoleEdge - 0.01, flDimEdge, distFromCenter);
    flDarkeningRing = abs(flDarkeningRing * 2.0 - 1.0) * 0.15 + 0.85;

    vec3 result = cRefract * flDarkeningRing;

    // Stage 0 alpha-tests against distFromCenter <= 1
    FragColor = vec4(result, 1.0);
}
)";

    PortalRenderer::PortalRenderer() = default;

    PortalRenderer::~PortalRenderer() {
        Shutdown();
    }

    void PortalRenderer::Shutdown() {
        if (!g_renderBackend) return;
        if (m_mesh != INVALID_MESH)              { g_renderBackend->DestroyMesh(m_mesh);             m_mesh = INVALID_MESH; }
        if (m_vb != INVALID_BUFFER)              { g_renderBackend->DestroyBuffer(m_vb);             m_vb = INVALID_BUFFER; }
        if (m_ib != INVALID_BUFFER)              { g_renderBackend->DestroyBuffer(m_ib);             m_ib = INVALID_BUFFER; }
        if (m_dummyTexture != INVALID_TEXTURE)   { g_renderBackend->DestroyTexture(m_dummyTexture);  m_dummyTexture = INVALID_TEXTURE; }
        if (m_sceneSnapshot != INVALID_TEXTURE)  { g_renderBackend->DestroyTexture(m_sceneSnapshot); m_sceneSnapshot = INVALID_TEXTURE; }
        if (m_noiseTexture != INVALID_TEXTURE)   { g_renderBackend->DestroyTexture(m_noiseTexture);   m_noiseTexture = INVALID_TEXTURE; }
        if (m_maskTexture != INVALID_TEXTURE)    { g_renderBackend->DestroyTexture(m_maskTexture);    m_maskTexture = INVALID_TEXTURE; }
        if (m_blueColorRamp != INVALID_TEXTURE)  { g_renderBackend->DestroyTexture(m_blueColorRamp);  m_blueColorRamp = INVALID_TEXTURE; }
        if (m_orangeColorRamp != INVALID_TEXTURE){ g_renderBackend->DestroyTexture(m_orangeColorRamp); m_orangeColorRamp = INVALID_TEXTURE; }
        if (m_shader != INVALID_SHADER)          { g_renderBackend->DestroyShader(m_shader);         m_shader = INVALID_SHADER; }
        if (m_refractionShader != INVALID_SHADER){ g_renderBackend->DestroyShader(m_refractionShader); m_refractionShader = INVALID_SHADER; }
        m_snapshotWidth = m_snapshotHeight = 0;
        m_initialized = false;
    }

    namespace {
        // Helper: load a PNG via stb_image, upload as RGBA8 or sRGB,
        // set Linear filter + caller-chosen wrap. Use srgb=true for
        // colour data (ramps, sprite albedo); false for intensity /
        // alpha-mask / normal-map data so the GPU doesn't apply a
        // gamma curve to it. Returns INVALID_TEXTURE on miss (caller
        // logs a warning + falls back to procedural rendering).
        Render::TextureHandle LoadPortalPNG(const char* path,
                                            Render::TextureWrap wrap = Render::TextureWrap::ClampToEdge,
                                            bool srgb = false) {
            int w = 0, h = 0, ch = 0;
            stbi_set_flip_vertically_on_load(0);
            unsigned char* pixels = stbi_load(path, &w, &h, &ch, STBI_rgb_alpha);
            if (!pixels) {
                Log::Warning("[PortalRenderer] stbi_load failed for %s: %s",
                             path, stbi_failure_reason());
                return INVALID_TEXTURE;
            }
            const Render::TextureFormat fmt = srgb
                ? Render::TextureFormat::SRGB8_A8
                : Render::TextureFormat::RGBA8;
            Render::TextureHandle tex = Render::g_renderBackend->CreateTexture2D(
                w, h, fmt, pixels);
            stbi_image_free(pixels);
            if (tex != INVALID_TEXTURE) {
                Render::g_renderBackend->SetTextureFilter(tex,
                    Render::TextureFilter::Linear, Render::TextureFilter::Linear);
                Render::g_renderBackend->SetTextureWrap(tex, wrap, wrap);
            }
            return tex;
        }
    } // namespace

    bool PortalRenderer::Initialize() {
        if (m_initialized) return true;
        if (!g_renderBackend) {
            Log::Error("[PortalRenderer] No render backend available");
            return false;
        }

        // Try SPIR-V first (Vulkan), fall back to GLSL source (OpenGL). No
        // .spv files exist yet — the SPIR-V path lands with Phase 5 along
        // with the stencil pipeline. On Vulkan today this Initialize() will
        // currently fall through to the source compile, which the Vulkan
        // backend rejects → m_shader stays INVALID and Render() no-ops
        // gracefully. Acceptable: the user is on GL by default.
        // On Vulkan, this needs the *portal* pipeline layout (UBO-aware),
        // which only CreateShaderFromFilesPortal sets up. We do a backend
        // type check rather than always calling the Portal variant because
        // the OpenGL backend's CreateShaderFromFilesPortal symbol isn't
        // defined — the static dispatch in the abstract RenderBackend
        // only knows CreateShaderFromFiles. So we cast and call directly
        // on Vulkan.
        if (g_renderBackend->GetType() == BackendType::Vulkan) {
#ifdef HAS_VULKAN
            auto* vk = static_cast<VKBackend*>(g_renderBackend.get());
            m_shader = vk->CreateShaderFromFilesPortal(
                "shaders/portal.vert", "shaders/portal.frag");
#endif
        } else {
            m_shader = g_renderBackend->CreateShaderFromFiles(
                "shaders/portal.vert", "shaders/portal.frag");
            if (m_shader == INVALID_SHADER) {
                m_shader = g_renderBackend->CreateShader(
                    vertexShaderSource, fragmentShaderSource);
            }
        }
        if (m_shader == INVALID_SHADER) {
            Log::Warning("[PortalRenderer] Failed to create shader — portal "
                         "rendering disabled");
            return false;
        }

        // 1×1 white dummy so the Vulkan descriptor layout is satisfied even
        // though the fragment shader doesn't sample it. Same trick as
        // BlockHighlight (see BlockHighlight.cpp:120).
        unsigned char white[] = {255, 255, 255, 255};
        m_dummyTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);

        // Refraction sub-pass shader — reuses the SAME vertex shader as
        // the main portal effect (it only needs vUV) plus a dedicated
        // fragment shader that samples the captured framebuffer.
        // Non-fatal if it fails (refraction skipped, rest still works).
        m_refractionShader = g_renderBackend->CreateShader(
            vertexShaderSource, refractionFragmentSource);
        if (m_refractionShader == INVALID_SHADER) {
            Log::Warning("[PortalRenderer] Refraction shader compile failed — refraction disabled");
        }

        // Portal textures — extracted from Portal 1's portal_pak_dir.vpk
        // (see assets/textures/portal/). Drive the direct port of Valve's
        // portal_refract_ps2x.fxc Stage 2 algorithm. Falls back to
        // procedural PortalFlame() if any fail to load.
        //   noise   : Repeat wrap, linear (intensity data — no gamma)
        //   ramps   : ClampToEdge, sRGB (colour data — GPU does sRGB→linear
        //             on sample, matching Valve's EnableSRGBRead(s2, true).
        //             Without this our colours come out ~2× over-bright,
        //             blow out into white, and over-drive the bloom chain.)
        //   mask    : ClampToEdge, linear (alpha-shape data — not colour)
        m_noiseTexture    = LoadPortalPNG("assets/textures/portal/portal_noise.png",
                                          TextureWrap::Repeat, /*srgb=*/false);
        m_maskTexture     = LoadPortalPNG("assets/textures/portal/portal_mask.png",
                                          TextureWrap::ClampToEdge, /*srgb=*/false);
        m_blueColorRamp   = LoadPortalPNG("assets/textures/portal/portal_blue_ramp.png",
                                          TextureWrap::ClampToEdge, /*srgb=*/true);
        m_orangeColorRamp = LoadPortalPNG("assets/textures/portal/portal_orange_ramp.png",
                                          TextureWrap::ClampToEdge, /*srgb=*/true);
        if (m_noiseTexture == INVALID_TEXTURE ||
            m_blueColorRamp == INVALID_TEXTURE ||
            m_orangeColorRamp == INVALID_TEXTURE) {
            Log::Warning("[PortalRenderer] One or more portal textures failed to load. "
                         "Rim shader will fall back to procedural rendering.");
        } else {
            Log::Info("[PortalRenderer] Loaded Portal-extracted textures: "
                      "noise 256×256, mask 512×512, blue/orange ramps 256×1.");
        }

        // Build the unit-portal mesh: triangle fan (center + N rim verts).
        // Vertex 0 = center; verts 1..N = rim; index buffer issues N triangles
        // (center, i, i+1) closing back to vertex 1.
        struct Vertex {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(Vertex) == 24, "Vertex must match GetBlockVertexLayout stride");

        std::vector<Vertex>   verts;
        std::vector<uint32_t> indices;
        verts.reserve(1 + kSegments);
        indices.reserve(kSegments * 3);

        // Center
        verts.push_back({0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 255, 255, 255, 255});

        // Rim — parameterize by angle. Local space: x ∈ [-halfW, halfW],
        // y ∈ [-halfH, halfH], z = 0 (the portal sits on its own plane;
        // basis matrix takes care of orienting + translating to world).
        for (int i = 0; i < kSegments; ++i) {
            const float t   = (float)i / (float)kSegments * 6.2831853f;
            const float x   = std::cos(t) * kHalfWidth;
            const float y   = std::sin(t) * kHalfHeight;
            // UV used purely for layout-compat — set to angle-derived 0..1.
            const float u   = std::cos(t) * 0.5f + 0.5f;
            const float v   = std::sin(t) * 0.5f + 0.5f;
            verts.push_back({x, y, 0.0f, u, v, 255, 255, 255, 255});
        }

        // Triangle fan as triangles (center, i, i+1), with wraparound.
        for (int i = 0; i < kSegments; ++i) {
            const uint32_t a = 1 + i;
            const uint32_t b = 1 + ((i + 1) % kSegments);
            indices.push_back(0);
            indices.push_back(a);
            indices.push_back(b);
        }

        m_indexCount = static_cast<uint32_t>(indices.size());
        m_vb   = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
                                               verts.size() * sizeof(Vertex), verts.data());
        m_ib   = g_renderBackend->CreateBuffer(BufferUsage::Index,
                                               indices.size() * sizeof(uint32_t), indices.data());
        m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());

        m_initialized = true;
        Log::Info("[PortalRenderer] Initialized (verts=%zu, indices=%u)",
                  verts.size(), m_indexCount);
        return true;
    }

    namespace {

        // Build the portal's local→world matrix from the orthonormal basis +
        // origin. Columns: [right, up, normal, origin]. Same convention as
        // PortalRegistry.cpp::PortalToWorld so the visual exactly matches the
        // server's collision plane.
        glm::mat4 PortalToWorldMat(const Client::ClientPortal& p) {
            glm::mat4 m(1.0f);
            m[0] = glm::vec4(p.right,  0.0f);
            m[1] = glm::vec4(p.upDir,  0.0f);
            m[2] = glm::vec4(p.normal, 0.0f);
            m[3] = glm::vec4(glm::vec3(p.origin), 1.0f);
            return m;
        }

        // Matches the user's Portal-game palette intuition: blue is a clean
        // cyan-blue, orange is a warm safety-cone orange. ARGB 8888 picked
        // by eye; tweak freely.
        // Portal-game-faithful 3-stop color palettes. dark = deep
        // saturated portal color used for inner flame tips; mid = the
        // saturated body color; hot = the bright tinted crest at the rim
        // (NOT pure white — Portal portals always retain hue even at the
        // brightest pixel; the blue's hot is light cyan, the orange's hot
        // is warm gold).
        struct PortalPalette {
            glm::vec3 dark;
            glm::vec3 mid;
            glm::vec3 hot;
        };
        // mid stops are EXACT Portal values from portal_util_shared.cpp:146
        // (UTIL_Portal_Color: blue = RGB 64/160/255, orange = RGB 255/160/32).
        // dark = mid * 0.30 (inner edge bleeding toward the disc), hot = mid
        // brightened + slightly washed toward warm/cool white for the rim
        // crest, retaining hue as Portal does.
        constexpr PortalPalette kBluePalette = {
            { 19.0f/255.0f,  48.0f/255.0f,  76.0f/255.0f},
            { 64.0f/255.0f, 160.0f/255.0f, 255.0f/255.0f},
            {140.0f/255.0f, 210.0f/255.0f, 255.0f/255.0f},
        };
        constexpr PortalPalette kOrangePalette = {
            { 76.0f/255.0f,  48.0f/255.0f,  10.0f/255.0f},
            {255.0f/255.0f, 160.0f/255.0f,  32.0f/255.0f},
            {255.0f/255.0f, 210.0f/255.0f, 130.0f/255.0f},
        };

        void DrawOnePortal(const Client::ClientPortal& p, glm::vec3 color,
                           const glm::mat4& proj, const glm::mat4& view,
                           ShaderHandle shader, MeshHandle mesh, uint32_t indexCount,
                           float pulse) {
            const glm::mat4 model = PortalToWorldMat(p);
            const glm::mat4 mvp   = proj * view * model;
            g_renderBackend->SetUniformMat4(shader, "uMVP", mvp);
            g_renderBackend->SetUniformVec3(shader, "uPortalColor", color);
            g_renderBackend->SetUniformFloat(shader, "uPulse", pulse);
            g_renderBackend->DrawIndexed(mesh, indexCount);
        }

    } // namespace

    namespace {

        // Pipeline preset: write the portal silhouette into the stencil
        // buffer. depthTest=true so geometry occluding the portal doesn't
        // get marked (portals correctly hide behind walls / players / etc.).
        // depthWrite + colorWrite OFF — we mutate stencil only.
        //
        // Polygon offset: pushes the portal's depth toward the camera so
        // it reliably wins z-fighting against the wall block it's stuck to
        // at grazing angles. Negative slope + units = pull toward camera.
        //
        // (Phase 7 recursion was tried and removed — too much flickering
        // when the recursion path interacted with the oblique projection's
        // near plane. Single level is the stable behaviour.)
        PipelineState StencilMarkState(uint32_t referenceLevel) {
            PipelineState s;
            s.depthTestEnabled  = true;
            s.depthWriteEnabled = false;
            s.colorWriteEnabled = false;
            s.cullMode          = CullMode::None;          // both portal faces mark
            s.primitiveType     = PrimitiveType::Triangles;
            s.depthBiasEnabled   = true;
            s.depthBiasSlope     = -2.0f;
            s.depthBiasConstant  = -2.0f;
            s.stencilTestEnabled = true;
            s.stencilCompareOp  = CompareOp::Always;
            s.stencilFailOp     = StencilOp::Keep;
            s.stencilDepthFailOp = StencilOp::Keep;
            s.stencilPassOp     = StencilOp::Replace;
            s.stencilReference  = referenceLevel;
            s.stencilWriteMask  = 0xFFu;
            s.stencilReadMask   = 0xFFu;
            return s;
        }

        // Pipeline preset: clear DEPTH and COLOR inside the portal silhouette.
        //
        // Why depth: the virtual-camera world re-render below would otherwise
        // depth-fail against whatever the main scene pass wrote into those
        // pixels (typically the wall block's depth, which is closer to the
        // player than most of the destination room) → silhouette keeps
        // showing the wall.
        //
        // Why color (added later): for parts of the destination world that
        // have NO chunk geometry to render (e.g. open sky above the dst
        // portal viewed at a steep angle), the world re-render produces no
        // fragments — and without a color clear the silhouette retains the
        // main-pass src-wall color. User-visible symptom: "the upper part
        // of the portal shows the wall behind it at low viewing angles."
        // Filling with sky-blue gives the correct look (open sky through
        // the portal where the dst world is empty).
        //
        // Pipeline: depth-test ENABLED with compareOp=Always (necessary —
        // GL won't write depth at all when GL_DEPTH_TEST is disabled, per
        // spec; so the "no test" intent has to be expressed via the always-
        // pass compare op), depth-write ON, color-write ON (clear to sky),
        // stencil-test Equal=N (gate to silhouette), no stencil-write. The
        // far-depth value comes from the shader (uForceFarDepth=1 → gl_FragDepth=1).
        PipelineState DepthClearState(uint32_t referenceLevel) {
            PipelineState s;
            s.depthTestEnabled  = true;
            s.depthCompareOp    = CompareOp::Always;
            s.depthWriteEnabled = true;
            s.colorWriteEnabled = true;
            s.cullMode          = CullMode::None;
            s.primitiveType     = PrimitiveType::Triangles;
            s.stencilTestEnabled = true;
            s.stencilCompareOp  = CompareOp::Equal;
            s.stencilReference  = referenceLevel;
            s.stencilReadMask   = 0xFFu;
            s.stencilWriteMask  = 0;          // don't disturb the stencil mark
            s.stencilFailOp     = StencilOp::Keep;
            s.stencilDepthFailOp = StencilOp::Keep;
            s.stencilPassOp     = StencilOp::Keep;
            return s;
        }

        // Sky color, must match PlatformMain.cpp's glClearColor for the
        // main framebuffer (currently RGB 120,167,255). Keep in sync — if
        // the sky color ever moves to a uniform/global, hook this off it.
        constexpr glm::vec3 kSkyColor(120.0f / 255.0f,
                                      167.0f / 255.0f,
                                      255.0f / 255.0f);

        // Pipeline preset: depth-refill the silhouette so the portal acts
        // as a real surface for any subsequent translucent draws. Color
        // writes off so we don't disturb the see-through render that just
        // landed; depth writes back on; stencil-test gates to the same
        // silhouette (compare equal to recursion level).
        PipelineState DepthRefillState(uint32_t referenceLevel) {
            PipelineState s;
            s.depthTestEnabled  = true;
            s.depthWriteEnabled = true;
            s.colorWriteEnabled = false;
            s.cullMode          = CullMode::None;
            s.primitiveType     = PrimitiveType::Triangles;
            // Force the depth value to win against whatever the see-through
            // pass wrote into this region — we want the portal SURFACE depth
            // (not the depth of stuff visible THROUGH the portal).
            s.depthCompareOp    = CompareOp::Always;
            s.stencilTestEnabled = true;
            s.stencilCompareOp  = CompareOp::Equal;
            s.stencilReference  = referenceLevel;
            s.stencilReadMask   = 0xFFu;
            s.stencilWriteMask  = 0;                       // don't clobber stencil
            s.stencilFailOp     = StencilOp::Keep;
            s.stencilDepthFailOp = StencilOp::Keep;
            s.stencilPassOp     = StencilOp::Keep;
            return s;
        }

        // Pipeline preset for the Portal-game-style "energy ring" outline
        // pass. Runs AFTER the see-through render + depth refill, drawing
        // the colored rim on top of the destination view. Inner pixels of
        // the mesh write nothing visible (alpha=0) so the see-through
        // continues to show through the centre.
        //
        // Depth setup: test=LessEqual so the outline ties with the
        // refilled portal-surface depth (both share the portal-plane Z,
        // so Equal passes) but is still occluded by geometry that sits
        // BETWEEN the camera and the portal — without this the rim
        // glow bleeds through walls/blocks in front of the portal.
        // depth-write=false so subsequent translucent draws still
        // composite against the portal-surface depth.
        // Stencil gated by Equal=ref so the outline only lands inside the
        // silhouette (no spillover at the rectangular bounding box).
        PipelineState OutlineState(uint32_t /*referenceLevel*/) {
            PipelineState s;
            s.depthTestEnabled  = true;
            s.depthCompareOp    = CompareOp::LessEqual;
            s.depthWriteEnabled = false;
            s.colorWriteEnabled = true;
            s.blendEnabled      = true;
            s.srcBlendFactor    = BlendFactor::SrcAlpha;
            s.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
            s.cullMode          = CullMode::None;
            s.primitiveType     = PrimitiveType::Triangles;
            // Stencil INTENTIONALLY DISABLED for the outline pass.
            // The rim needs to render in the band [1.0, 1.075]
            // (Valve's "outer border") which sits OUTSIDE the
            // see-through hole — and therefore outside the stencil=1
            // region marked by the SeeThroughPass's stencil-mark step.
            // The fragment shader's `discard` at distFromCenter > 1.075
            // handles geometric bounds, so stencil gating isn't
            // needed.
            s.stencilTestEnabled = false;
            return s;
        }

        // Pipeline preset for the INACTIVE portal render (only one of the
        // pair placed so far — no destination to see through, so we draw
        // the Portal-game flame outline with an opaque BLACK centre). Alpha
        // blending is needed for the soft anti-aliased silhouette edge
        // (radial≈1 fades to alpha 0 in the shader). depth-write ON because
        // the portal is a solid surface that should occlude later draws.
        PipelineState InactivePortalState() {
            PipelineState s;
            s.depthTestEnabled  = true;
            s.depthWriteEnabled = true;
            s.colorWriteEnabled = true;
            s.blendEnabled      = true;
            s.srcBlendFactor    = BlendFactor::SrcAlpha;
            s.dstBlendFactor    = BlendFactor::OneMinusSrcAlpha;
            s.cullMode          = CullMode::None;
            s.primitiveType     = PrimitiveType::Triangles;
            return s;
        }

    } // namespace

    void PortalRenderer::Render(const glm::mat4& projectionMatrix,
                                const glm::mat4& viewMatrix,
                                const Camera& camera,
                                const Frustum& /*frustum*/,
                                float aspect,
                                float farPlane,
                                const SceneRenderFn& renderScene) {
        if (!m_initialized || !g_renderBackend) return;
        auto& mgr = Client::GetClientPortalManager();
        if (mgr.PairCount() == 0) return;

        // Pulse used by both flat-oval fallback and the depth-refill /
        // stencil-mark sub-passes — keeps the silhouette breathing slightly
        // even when the portal renders see-through (matches Phase 4 feel).
        // Pulse oscillates between (1 - kPulseAmplitude) and 1.0 — never
        // exceeds 1.0, so the portal mesh never grows past its nominal
        // 1×2-block size. Maps the [-1, +1] sin range to [0, 1] and uses
        // it as the SHRINK fraction.
        // Time mod 1000 — matches Valve's portal_refract_helper.cpp:133-134
        // (flTime -= floor(flTime/1000.0)*1000.0). Without this wrap the
        // float time grows unboundedly during long sessions and FP precision
        // erodes the noise-scroll smoothness after a few hours of play.
        const double rawNow = glfwGetTime();
        const float  t      = static_cast<float>(std::fmod(rawNow, 1000.0));
        const float wobble = 0.5f + 0.5f * std::sin(t * kPulseFreqHz * 6.2831853f);
        const float pulse  = 1.0f - kPulseAmplitude * wobble;

        // Helper: portal world model matrix (basis = right/up/normal,
        // translation = origin). Same convention used everywhere portal
        // geometry gets drawn so the visual matches the collision plane.
        auto PortalModel = [](const Client::ClientPortal& p) {
            return glm::mat4{
                glm::vec4(p.right,  0.0f),
                glm::vec4(p.upDir,  0.0f),
                glm::vec4(p.normal, 0.0f),
                glm::vec4(glm::vec3(p.origin), 1.0f),
            };
        };

        // ─── Single-level see-through (Phase 6) ─────────────────────────
        //
        // Per portal direction (blue→orange and orange→blue):
        //   1. Stencil-mark the source portal silhouette (ref=1).
        //   2. Clear depth + color inside the silhouette.
        //   3. Invoke the scene callback with the virtual camera + oblique
        //      projection — it draws chunks, remote players, local player.
        //   4. Depth-refill the silhouette so transparent draws composite.
        //
        // No recursion. Recursion (Phase 7) was removed because its
        // interaction with the oblique near plane caused flickering and
        // didn't actually produce a visible portal-in-portal effect.
        constexpr uint32_t kStencilRef = 1;

        auto SeeThroughPass = [&](const Client::ClientPortal& src,
                                  const Client::ClientPortal& dst,
                                  const PortalPalette& palette,
                                  bool  isOrange,
                                  float flashIntensity,
                                  float openAmount,
                                  float staticAmount) {
            const glm::mat4 model = PortalModel(src);
            const glm::mat4 mvp   = projectionMatrix * viewMatrix * model;

            // 0. Reset stencil so this pass starts clean. Without this, the
            //    previous SeeThroughPass's stencil=1 region (e.g. blue's
            //    silhouette) is still marked when this pass runs (orange's),
            //    so steps 2/3 — which gate on stencil=Equal=1 — fire on the
            //    OTHER portal's silhouette too. Visible symptom: looking at
            //    one portal shows half of its area overwritten with the
            //    OTHER portal's destination view (rendered from the wrong
            //    virtual camera).
            g_renderBackend->Clear(/*color=*/false, /*depth=*/false,
                                   /*stencil=*/true);

            // 1. Stencil mark — write stencil=1 inside the visible part of
            //    the portal silhouette (depth-test against existing geometry).
            //    The shader's solid-fill path discards at dist > openAmtSq,
            //    so as openAmount ramps 0→1 the marked region grows from
            //    centre dot to full disc.
            g_renderBackend->SetPipelineState(StencilMarkState(kStencilRef));
            g_renderBackend->BindShader(m_shader);
            g_renderBackend->BindTexture(m_dummyTexture, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->SetUniformVec3(m_shader, "uPortalColor", glm::vec3(0.0f));
            g_renderBackend->SetUniformFloat(m_shader, "uPulse", pulse);
            g_renderBackend->SetUniformFloat(m_shader, "uForceFarDepth", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uOutlineMode", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uTime", t);
            g_renderBackend->SetUniformFloat(m_shader, "uFlashIntensity", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmount",   openAmount);  // gates hole size
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmountVS", openAmount);  // VS noise scale
            g_renderBackend->DrawIndexed(m_mesh, m_indexCount);
            g_renderBackend->UnbindMesh();

            // 2. Clear depth (→ far) + color (→ sky) inside the silhouette.
            //    Same openAmount gating so the depth-cleared region matches
            //    the marked stencil region (both grow with the animation).
            g_renderBackend->SetPipelineState(DepthClearState(kStencilRef));
            g_renderBackend->BindShader(m_shader);
            g_renderBackend->BindTexture(m_dummyTexture, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->SetUniformVec3(m_shader, "uPortalColor", kSkyColor);
            g_renderBackend->SetUniformFloat(m_shader, "uPulse", pulse);
            g_renderBackend->SetUniformFloat(m_shader, "uForceFarDepth", 1.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uOutlineMode", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uTime", t);
            g_renderBackend->SetUniformFloat(m_shader, "uFlashIntensity", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmount",   openAmount);
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmountVS", openAmount);
            g_renderBackend->DrawIndexed(m_mesh, m_indexCount);
            g_renderBackend->UnbindMesh();

            // 3. Build the virtual camera + oblique projection + frustum
            //    (same as Phase 6) and invoke the caller's scene render.
            //    SetStencilOverride splices stencil-test Equal=1 into
            //    every SetPipelineState the lambda makes, so chunks and
            //    players render only inside the silhouette.
            const Camera virt = PortalTransform::ComputeVirtualCamera(camera, src, dst);
            const glm::mat4 virtView = virt.GetViewMatrix();
            const glm::mat4 baseProj = glm::perspective(
                glm::radians(virt.fov), aspect, 0.05f, farPlane);
            const glm::mat4 virtProj = PortalTransform::ObliqueProjection(
                baseProj, virtView, dst);
            const Frustum   virtFrust = Frustum::FromMatrix(baseProj * virtView);

            g_renderBackend->SetStencilOverride(
                /*enabled=*/   true,
                /*compareOp=*/ CompareOp::Equal,
                /*passOp=*/    StencilOp::Keep,
                /*reference=*/ kStencilRef,
                /*readMask=*/  0xFFu,
                /*writeMask=*/ 0u);

            // World-space user clip plane — mirrors Portal's
            // PushCustomClipPlane (portalrenderable_flatbasic.cpp:454).
            // Plane normal = +dst.normal; plane is shifted 5 cm BACK
            // (in -dst.normal direction) so half-in objects (e.g. the
            // player straddling the dst portal right after teleport)
            // render correctly without their body being clipped.
            constexpr float kClipPlaneOffsetBack = 0.05f;
            const glm::vec3 dstN = dst.normal;
            const glm::vec3 dstO = glm::vec3(dst.origin);
            const glm::vec4 clipPlane(dstN, -glm::dot(dstN, dstO) + kClipPlaneOffsetBack);
            ChunkRenderer::SetPortalClipPlane(clipPlane);

            renderScene(virt, virtFrust, virtProj);

            ChunkRenderer::SetPortalClipPlane(glm::vec4(0.0f));  // reset
            g_renderBackend->SetStencilOverride(false);

            // 3.5. Refraction sub-pass — DISABLED per user feedback.
            // Portal 1's portals don't visibly pinch the see-through
            // view (the Valve PortalRefract Stage 0 warp is too subtle
            // to notice in Portal, but our screen-tangent-magnitude
            // approximation made it dominant). The whole sub-pass is
            // skipped; the see-through view from step 3 stays as-is.
            // To re-enable for experimentation, change to
            // `if (m_refractionShader != INVALID_SHADER)`.
            if (false) {
                int fbW = 0, fbH = 0;
                glfwGetFramebufferSize(g_renderBackend->GetWindow(), &fbW, &fbH);
                if (fbW > 0 && fbH > 0) {
                    // Lazy resize of the snapshot texture to match the
                    // framebuffer. Recreate when window size changes.
                    if (fbW != m_snapshotWidth || fbH != m_snapshotHeight) {
                        if (m_sceneSnapshot != INVALID_TEXTURE) {
                            g_renderBackend->DestroyTexture(m_sceneSnapshot);
                        }
                        // RGBA16F so HDR colors from the bloomed
                        // rim aren't clamped during the refraction
                        // copy (Phase J). Falls back gracefully on
                        // backends that don't support HDR — the
                        // refraction shader works with either format.
                        m_sceneSnapshot = g_renderBackend->CreateTexture2D(
                            fbW, fbH, TextureFormat::RGBA16F, nullptr);
                        if (m_sceneSnapshot != INVALID_TEXTURE) {
                            g_renderBackend->SetTextureFilter(m_sceneSnapshot,
                                TextureFilter::Linear, TextureFilter::Linear);
                            g_renderBackend->SetTextureWrap(m_sceneSnapshot,
                                TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
                        }
                        m_snapshotWidth  = fbW;
                        m_snapshotHeight = fbH;
                    }
                    if (m_sceneSnapshot != INVALID_TEXTURE) {
                        // Capture: copy framebuffer color → snapshot.
                        g_renderBackend->CopyFramebufferToTexture(m_sceneSnapshot);

                        // Distorted re-draw inside the silhouette.
                        // depth-test=Always so we overwrite whatever the
                        // chunk render put down; depth-write off so we
                        // don't disturb depth (refill below sets it).
                        // Stencil gates to the silhouette.
                        PipelineState refractState;
                        refractState.depthTestEnabled  = true;
                        refractState.depthCompareOp    = CompareOp::Always;
                        refractState.depthWriteEnabled = false;
                        refractState.colorWriteEnabled = true;
                        refractState.blendEnabled      = false;
                        refractState.cullMode          = CullMode::None;
                        refractState.primitiveType     = PrimitiveType::Triangles;
                        refractState.stencilTestEnabled = true;
                        refractState.stencilCompareOp  = CompareOp::Equal;
                        refractState.stencilReference  = kStencilRef;
                        refractState.stencilReadMask   = 0xFFu;
                        refractState.stencilWriteMask  = 0;
                        refractState.stencilFailOp     = StencilOp::Keep;
                        refractState.stencilDepthFailOp = StencilOp::Keep;
                        refractState.stencilPassOp     = StencilOp::Keep;
                        g_renderBackend->SetPipelineState(refractState);

                        // Compute screen-space tangent + binormal in UV
                        // coords for Valve's Stage 0 refraction. Project
                        // (origin), (origin + right), (origin + upDir)
                        // through the view-projection, divide by w, map
                        // to [0,1] UV space, then take differences.
                        // Simplification of Valve's per-pixel tangent
                        // projection (we use one direction per portal,
                        // not per pixel — accurate enough for typical
                        // viewing angles).
                        const glm::mat4 vp = projectionMatrix * viewMatrix;
                        auto toScreenUV = [&](const glm::vec3& worldPt) {
                            glm::vec4 clip = vp * glm::vec4(worldPt, 1.0f);
                            if (std::abs(clip.w) < 1e-6f) clip.w = 1e-6f;
                            return glm::vec2(
                                (clip.x / clip.w) * 0.5f + 0.5f,
                                (clip.y / clip.w) * 0.5f + 0.5f);
                        };
                        const glm::vec3 portalOrigin = glm::vec3(src.origin);
                        const glm::vec2 uvOrigin = toScreenUV(portalOrigin);
                        const glm::vec2 uvTan    = toScreenUV(portalOrigin + src.right);
                        const glm::vec2 uvBin    = toScreenUV(portalOrigin + src.upDir);
                        const glm::vec2 screenTangent  = uvTan - uvOrigin;
                        const glm::vec2 screenBinormal = uvBin - uvOrigin;

                        g_renderBackend->BindShader(m_refractionShader);
                        g_renderBackend->BindTexture(m_sceneSnapshot, 0);
                        g_renderBackend->SetUniformMat4 (m_refractionShader, "uMVP", mvp);
                        g_renderBackend->SetUniformFloat(m_refractionShader, "uPulse", pulse);
                        g_renderBackend->SetUniformVec2 (m_refractionShader, "uScreenSize",
                            glm::vec2(static_cast<float>(fbW), static_cast<float>(fbH)));
                        g_renderBackend->SetUniformVec2 (m_refractionShader, "uScreenTangent",  screenTangent);
                        g_renderBackend->SetUniformVec2 (m_refractionShader, "uScreenBinormal", screenBinormal);
                        g_renderBackend->SetUniformFloat(m_refractionShader, "uOpenAmount", openAmount);
                        g_renderBackend->SetUniformInt  (m_refractionShader, "uSceneSnapshot", 0);
                        g_renderBackend->DrawIndexed(m_mesh, m_indexCount);
                        g_renderBackend->UnbindMesh();
                    }
                }
            }

            // 4. Depth refill — stamp the portal-surface depth back so
            //    later translucent draws composite correctly. Same
            //    openAmount gating as stencil-mark / depth-clear above
            //    so the refilled region matches.
            g_renderBackend->SetPipelineState(DepthRefillState(kStencilRef));
            g_renderBackend->BindShader(m_shader);
            g_renderBackend->BindTexture(m_dummyTexture, 0);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->SetUniformVec3(m_shader, "uPortalColor", glm::vec3(0.0f));
            g_renderBackend->SetUniformFloat(m_shader, "uPulse", pulse);
            g_renderBackend->SetUniformFloat(m_shader, "uForceFarDepth", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uOutlineMode", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uTime", t);
            g_renderBackend->SetUniformFloat(m_shader, "uFlashIntensity", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmount",   openAmount);
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmountVS", openAmount);
            g_renderBackend->DrawIndexed(m_mesh, m_indexCount);
            g_renderBackend->UnbindMesh();

            // 5. Portal-game-style energy outline — draws on top of the
            //    see-through view. Inner pixels are alpha=0 so the centre
            //    keeps showing the destination world; rim is the bright
            //    glowing ring (color from caller — blue or orange). Stencil-
            //    gated to the silhouette so the rim doesn't bleed onto the
            //    surrounding wall corners of the rectangular mesh bbox.
            g_renderBackend->SetPipelineState(OutlineState(kStencilRef));
            g_renderBackend->BindShader(m_shader);
            // Bind Portal-extracted noise + per-color radial ramp.
            // (Slot 0 = noise, slot 1 = color ramp.) Falls back to dummy
            // + procedural rendering when textures missing.
            const bool useTex = (m_noiseTexture != INVALID_TEXTURE &&
                                 (isOrange ? m_orangeColorRamp : m_blueColorRamp) != INVALID_TEXTURE);
            if (useTex) {
                g_renderBackend->BindTexture(m_noiseTexture, 0);
                g_renderBackend->BindTexture(isOrange ? m_orangeColorRamp : m_blueColorRamp, 1);
                g_renderBackend->SetUniformInt(m_shader, "uPortalNoiseTex", 0);
                g_renderBackend->SetUniformInt(m_shader, "uPortalColorTex", 1);
                g_renderBackend->SetUniformFloat(m_shader, "uColorScale", 4.0f);   // VMT $PortalColorScale
                g_renderBackend->SetUniformInt(m_shader, "uUseTextures", 1);
            } else {
                g_renderBackend->BindTexture(m_dummyTexture, 0);
                g_renderBackend->SetUniformInt(m_shader, "uUseTextures", 0);
            }
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->SetUniformVec3(m_shader, "uPortalColor", palette.mid);
            g_renderBackend->SetUniformVec3(m_shader, "uColorDark",   palette.dark);
            g_renderBackend->SetUniformVec3(m_shader, "uColorHot",    palette.hot);
            g_renderBackend->SetUniformFloat(m_shader, "uPulse", pulse);
            g_renderBackend->SetUniformFloat(m_shader, "uForceFarDepth", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uOutlineMode", 1.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uTime", t);
            g_renderBackend->SetUniformFloat(m_shader, "uTimeVS", t);                 // VS noise scroll
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmountVS", openAmount);  // VS noise scale
            g_renderBackend->SetUniformFloat(m_shader, "uFlashIntensity", flashIntensity);
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmount",   openAmount);
            g_renderBackend->SetUniformFloat(m_shader, "uStaticAmount", staticAmount);
            // Active path — fully active (1.0) unless sibling-ping is fading
            // back in, in which case portalActive ramps from 0 → 1.
            g_renderBackend->SetUniformFloat(m_shader, "uPortalActive",
                                             1.0f - staticAmount);
            g_renderBackend->DrawIndexed(m_mesh, m_indexCount);
            g_renderBackend->UnbindMesh();
        };

        // Inactive portal (sibling not placed yet): draw the Portal-game
        // flame outline with an opaque BLACK centre. No see-through pass,
        // no stencil — just a single alpha-blended draw of the mesh with
        // uOutlineMode=2. Inactive portals can also flash on... well,
        // they can't get teleported through (no destination), but we keep
        // the flash uniform plumbing consistent — always 0 for inactive.
        auto InactivePortal = [&](const Client::ClientPortal& p, const PortalPalette& palette,
                                  bool  isOrange,
                                  float openAmount) {
            g_renderBackend->SetPipelineState(InactivePortalState());
            g_renderBackend->BindShader(m_shader);
            // Same texture/uniform setup as the active path.
            const bool useTex = (m_noiseTexture != INVALID_TEXTURE &&
                                 (isOrange ? m_orangeColorRamp : m_blueColorRamp) != INVALID_TEXTURE);
            if (useTex) {
                g_renderBackend->BindTexture(m_noiseTexture, 0);
                g_renderBackend->BindTexture(isOrange ? m_orangeColorRamp : m_blueColorRamp, 1);
                g_renderBackend->SetUniformInt(m_shader, "uPortalNoiseTex", 0);
                g_renderBackend->SetUniformInt(m_shader, "uPortalColorTex", 1);
                g_renderBackend->SetUniformFloat(m_shader, "uColorScale", 4.0f);
                g_renderBackend->SetUniformInt(m_shader, "uUseTextures", 1);
            } else {
                g_renderBackend->BindTexture(m_dummyTexture, 0);
                g_renderBackend->SetUniformInt(m_shader, "uUseTextures", 0);
            }
            const glm::mat4 mvp = projectionMatrix * viewMatrix * PortalModel(p);
            g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
            g_renderBackend->SetUniformVec3(m_shader, "uPortalColor", palette.mid);
            g_renderBackend->SetUniformVec3(m_shader, "uColorDark",   palette.dark);
            g_renderBackend->SetUniformVec3(m_shader, "uColorHot",    palette.hot);
            g_renderBackend->SetUniformFloat(m_shader, "uPulse", pulse);
            g_renderBackend->SetUniformFloat(m_shader, "uForceFarDepth", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uOutlineMode", 2.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uTimeVS", t);                 // VS noise scroll
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmountVS", openAmount);  // VS noise scale
            // Inactive — no destination paired. Vortex fills the centre.
            g_renderBackend->SetUniformFloat(m_shader, "uPortalActive", 0.0f);
            g_renderBackend->SetUniformFloat(m_shader, "uTime", t);
            g_renderBackend->SetUniformFloat(m_shader, "uFlashIntensity", 0.0f);
            // Inactive portals still ramp open over kOpenDurationSec — when
            // a single portal is placed without a partner, the rim should
            // still fade in over Portal's authoritative 0.5s.
            g_renderBackend->SetUniformFloat(m_shader, "uOpenAmount",   openAmount);
            g_renderBackend->SetUniformFloat(m_shader, "uStaticAmount", 0.0f);
            g_renderBackend->DrawIndexed(m_mesh, m_indexCount);
            g_renderBackend->UnbindMesh();
        };

        const double now = glfwGetTime();
        mgr.ForEachPair([&](uint64_t /*gunId*/, const Client::ClientPortalPair& pair) {
            // Per-pair teleport flash intensity, decaying from 1 → 0 over
            // kFlashDurationSec after a teleport (server pushes a packet
            // that sets pair.flashEndTimeSec; renderer reads it here).
            float flash = 0.0f;
            if (pair.flashEndTimeSec > now) {
                const double remaining = pair.flashEndTimeSec - now;
                flash = static_cast<float>(remaining / Client::kFlashDurationSec);
                flash = glm::clamp(flash, 0.0f, 1.0f);
            }

            // Portal-authoritative open/static amounts per portal
            // (mirrors c_prop_portal.cpp:222-243). openAmount ramps 0→1
            // over kOpenDurationSec when the portal is first placed.
            // staticAmount decays 1→0 over kStaticDurationSec when the
            // portal's pair-mate gets re-fired ("ping" effect).
            auto OpenAmount = [&](const Client::ClientPortal& p) -> float {
                const double dt = now - p.openStartTimeSec;
                return glm::clamp(static_cast<float>(dt / Client::kOpenDurationSec),
                                  0.0f, 1.0f);
            };
            auto StaticAmount = [&](const Client::ClientPortal& p) -> float {
                const double dt = now - p.staticPingStartTimeSec;
                if (dt < 0.0 || dt > Client::kStaticDurationSec) return 0.0f;
                return 1.0f - static_cast<float>(dt / Client::kStaticDurationSec);
            };

            // Close-up override — when the camera is within the near-plane
            // distance (~5 cm) IN FRONT of a source portal AND within its
            // 1×2 lateral bounds, the portal mesh near-clips and the
            // regular SeeThroughPass produces NOTHING (mesh entirely
            // behind the near plane → no stencil mark → no see-through).
            //
            // Symptom: the player walks slowly toward a portal, the disc
            // disappears in the last 5 cm of approach, they see the wall
            // geometry until they cross (then client-side prediction
            // teleports them).
            //
            // Fix: when camera is in this zone for portal X, render the
            // destination scene FULLSCREEN via the virtual camera. The
            // disc is invisible at this distance anyway (it covers a
            // larger angle than the FOV when the camera is 5 cm away),
            // so visually the destination scene fills the view — exactly
            // what a real portal would show. This mirrors the technique
            // used in Portal (Source SDK).
            // No close-up special-case any more. Client-side teleport
            // prediction (ClientPortalManager::CheckEyeCrossing with
            // kEarlyPredictDistance = 10 cm) fires the moment the eye
            // enters the 10 cm zone in front of an active source plane.
            // Result: the camera is always either >10 cm from any portal
            // plane (regular SeeThroughPass works perfectly) or already
            // teleported (rendering from destination side). The disc
            // mesh never near-plane-clips because the camera never gets
            // within 5 cm. Mirrors Source SDK's eye-transform approach
            // (c_portal_player.cpp:CalcPortalView).
            const bool both = pair.blue.active && pair.orange.active;
            if (both) {
                SeeThroughPass(pair.blue,   pair.orange, kBluePalette,   /*isOrange=*/false, flash,
                               OpenAmount(pair.blue),   StaticAmount(pair.blue));
                SeeThroughPass(pair.orange, pair.blue,   kOrangePalette, /*isOrange=*/true,  flash,
                               OpenAmount(pair.orange), StaticAmount(pair.orange));
            } else {
                if (pair.blue.active)   InactivePortal(pair.blue,   kBluePalette,   /*isOrange=*/false,
                                                      OpenAmount(pair.blue));
                if (pair.orange.active) InactivePortal(pair.orange, kOrangePalette, /*isOrange=*/true,
                                                      OpenAmount(pair.orange));
            }
        });

        // Restore default pipeline so the HUD / overlays start from a known
        // state (stencil disabled, color writes on, depth normal).
        PipelineState defaultState;
        defaultState.depthTestEnabled  = true;
        defaultState.depthWriteEnabled = true;
        defaultState.colorWriteEnabled = true;
        defaultState.blendEnabled      = false;
        defaultState.cullMode          = CullMode::Back;
        defaultState.polygonMode       = PolygonMode::Fill;
        defaultState.stencilTestEnabled = false;
        g_renderBackend->SetPipelineState(defaultState);
    }

} // namespace Render

#endif // ENABLE_PORTAL_GUN
