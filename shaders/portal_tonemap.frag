// File: shaders/portal_tonemap.frag
// HDR → LDR composite. Samples the HDR offscreen RT and outputs to the
// LDR backbuffer.
//
// We previously ran ACES Filmic here, which is appropriate for actual
// HDR content (values > 1.0) but DARKENS and DESATURATES regular LDR
// scene colors — for example sky-blue (0.47, 0.65, 1.00) gets mapped
// to (0.60, 0.70, 0.80), reading as a greyer / less vivid sky.
//
// Since most of our scene is LDR (only the portal rim ever exceeds 1.0
// via the sRGB-corrected ramp × colorScale), a SIMPLE PASS-THROUGH +
// hardware-clamp behaves identically to rendering directly to the LDR
// backbuffer (the way the engine ran before the HDR pipeline landed)
// while still preserving additive bloom when enabled.
//
// Bloom (when present) is added BEFORE clamping so the additive
// contribution doesn't push pure-cyan over to white the way a normal-
// blended composite would.
#version 330 core

uniform sampler2D uHDRColor;
uniform sampler2D uBloomColor;  // optional, bound to dummy when not in use
uniform float     uExposure;    // linear scale (default 1.0)
uniform float     uHasBloom;    // 0 = no bloom add, 1 = add uBloomColor

in  vec2 vUV;
out vec4 FragColor;

void main() {
    vec3 hdr = texture(uHDRColor, vUV).rgb;
    if (uHasBloom > 0.5) {
        hdr += texture(uBloomColor, vUV).rgb;
    }
    hdr *= uExposure;
    // Pass-through: hardware clamps to [0, 1] when written to the LDR
    // backbuffer, matching the pre-HDR-pipeline render path exactly.
    FragColor = vec4(clamp(hdr, 0.0, 1.0), 1.0);
}
