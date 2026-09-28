#version 330 core
// File: shaders/post_box_blur.frag (OpenGL) — MC post/box_blur.fsh
// Post pass of Render::PostChain. texCoord is GL-convention (0,0 bottom-left).
// uPostParams packs the pass's config uniforms (MC's std140 blocks, each
// block starting on a vec4) as four vec4 columns; SamplerInfo is uInSize /
// uOutSize. An input the chain does not ask to sample bilinear is sampled at
// its texel centre (the targets are all linear-filtered — nearest by snap).
in vec2 texCoord;
out vec4 fragColor;

uniform sampler2D uInSampler;    // MC InSampler
uniform sampler2D uAuxSampler;   // the pass's second sampler (spiderclip's BlurSampler)
uniform vec2  uInSize;           // SamplerInfo.InSize
uniform vec2  uAuxSize;
uniform vec2  uOutSize;          // SamplerInfo.OutSize
uniform float uInBilinear;
uniform float uAuxBilinear;
uniform float uMenuBlurRadius;   // globals.MenuBlurRadius
uniform mat4  uPostParams;

vec4 SampleIn(vec2 uv) {
    if (uInBilinear < 0.5) uv = (floor(uv * uInSize) + 0.5) / uInSize;
    return texture(uInSampler, uv);
}
vec4 SampleAux(vec2 uv) {
    if (uAuxBilinear < 0.5) uv = (floor(uv * uAuxSize) + 0.5) / uAuxSize;
    return texture(uAuxSampler, uv);
}
#define P0 uPostParams[0]
#define P1 uPostParams[1]
#define P2 uPostParams[2]
#define P3 uPostParams[3]
#define InSize uInSize
#define OutSize uOutSize
#define MenuBlurRadius uMenuBlurRadius

// BlurConfig { vec2 BlurDir; float Radius; }
// Relies on bilinear sampling to halve the taps: samples between texel
// pairs with a step of 2, the last one at half weight.
void main() {
    vec2 BlurDir = P0.xy;
    float Radius = P0.z;
    vec2 oneTexel = 1.0 / InSize;
    vec2 sampleStep = oneTexel * BlurDir;

    vec4 blurred = vec4(0.0);
    float actualRadius = Radius >= 0.5 ? round(Radius) : MenuBlurRadius;
    for (float a = -actualRadius + 0.5; a <= actualRadius; a += 2.0) {
        blurred += SampleIn(texCoord + sampleStep * a);
    }
    blurred += SampleIn(texCoord + sampleStep * actualRadius) / 2.0;
    fragColor = blurred / (actualRadius + 0.5);
}
