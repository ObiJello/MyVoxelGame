// File: shaders/portal_bloom_bright.frag
// Bright-pass extract for the bloom chain. Samples the HDR RT and
// outputs pixels whose luminance exceeds uThreshold, with a smooth
// knee so the cutoff isn't a hard step. Matches the SDK_Bloom_ps2x.fxc
// reference (Portal's bright-pass uses Reinhard-style soft threshold).
#version 330 core

uniform sampler2D uHDRColor;
uniform float     uThreshold;   // linear luminance cutoff (default 1.0)
uniform float     uSoftKnee;    // 0..1 — width of the soft knee (default 0.5)

in  vec2 vUV;
out vec4 FragColor;

void main() {
    vec3 c = texture(uHDRColor, vUV).rgb;
    // Perceptual luminance (Rec. 709 weights).
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Soft-knee threshold: smooth transition from 0 → 1 around uThreshold.
    float knee = uThreshold * uSoftKnee;
    float soft = clamp((lum - uThreshold + knee) / max(knee, 1e-4), 0.0, 1.0);
    soft = soft * soft * (3.0 - 2.0 * soft); // smoothstep
    // Scale color by the bright-pass mask. Output preserves the
    // color's hue, just multiplied by the soft cutoff.
    float bright = max(soft, max(0.0, lum - uThreshold) / max(lum, 1e-4));
    FragColor = vec4(c * bright, 1.0);
}
