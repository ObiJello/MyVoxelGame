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
