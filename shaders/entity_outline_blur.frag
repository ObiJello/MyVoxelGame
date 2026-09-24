#version 330 core
// File: shaders/entity_outline_blur.frag (OpenGL) — MC
// post/entity_outline_box_blur.fsh verbatim (Radius 2, sampled bilinear as
// the post chain asks): two taps between texel pairs plus a half-weight end
// tap, along uBlurDir.
in vec2 texCoord;
out vec4 fragColor;

uniform sampler2D uInSampler;   // MC InSampler
uniform vec2 uInSize;     // MC SamplerInfo.InSize
uniform vec2 uBlurDir;    // MC BlurConfig.BlurDir

void main() {
    vec2 oneTexel = 1.0 / uInSize;
    vec2 sampleStep = oneTexel * uBlurDir;

    vec4 blurred = vec4(0.0);
    float radius = 2.0;
    for (float a = -radius + 0.5; a <= radius; a += 2.0) {
        blurred += texture(uInSampler, texCoord + sampleStep * a);
    }
    blurred += texture(uInSampler, texCoord + sampleStep * radius) / 2.0;
    fragColor = vec4((blurred / (radius + 0.5)).rgb, blurred.a);
}
