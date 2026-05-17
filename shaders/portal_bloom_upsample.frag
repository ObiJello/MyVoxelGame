// File: shaders/portal_bloom_upsample.frag
// Upsample-and-combine pass for the bloom chain. Samples both the
// previous (smaller) bloom level and the current (larger) blurred
// level, then adds them. Output: combined bloom at the larger
// resolution. Linear filtering on the input texture gives the
// upsample interpolation for free.
//
// Used in the upsample chain: start from the smallest blurred level,
// progressively combine with each larger level until back at full res.
#version 330 core

uniform sampler2D uSmaller;  // smaller mip's combined bloom (already blurred)
uniform sampler2D uLarger;   // current level's blurred bright-pass
uniform float     uStrength; // additive scale on the smaller mip (default 1.0)

in  vec2 vUV;
out vec4 FragColor;

void main() {
    vec3 small = texture(uSmaller, vUV).rgb;
    vec3 large = texture(uLarger,  vUV).rgb;
    FragColor = vec4(large + small * uStrength, 1.0);
}
