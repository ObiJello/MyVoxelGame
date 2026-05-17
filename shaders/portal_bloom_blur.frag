// File: shaders/portal_bloom_blur.frag
// Separable Gaussian blur for the bloom downsample/upsample chain.
// uDirection = (1, 0) for horizontal, (0, 1) for vertical. uTexelSize
// gives one texel offset in UV space (1.0 / RT size).
//
// 9-tap Gaussian (sigma ~= 2.0) — same kernel weights as Portal's
// SDK_Bloom_ps2x.fxc one-axis pass. Two passes (H then V) give a
// proper 2D Gaussian blur.
#version 330 core

uniform sampler2D uColor;
uniform vec2      uTexelSize;   // 1.0 / RT dimensions
uniform vec2      uDirection;   // (1,0) horizontal, (0,1) vertical

in  vec2 vUV;
out vec4 FragColor;

void main() {
    // Discrete 9-tap Gaussian, normalised: sum = 1.0.
    const float w[5] = float[5](
        0.2270270270,
        0.1945945946,
        0.1216216216,
        0.0540540541,
        0.0162162162
    );

    vec3 acc = texture(uColor, vUV).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        vec2 off = uDirection * uTexelSize * float(i);
        acc += texture(uColor, vUV + off).rgb * w[i];
        acc += texture(uColor, vUV - off).rgb * w[i];
    }
    FragColor = vec4(acc, 1.0);
}
