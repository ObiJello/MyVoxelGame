#version 330 core
// File: shaders/post_bits.frag (OpenGL) — MC post/bits.fsh
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

// BitsConfig { float Resolution; float MosaicSize; }
const float Saturation = 1.5;

void main() {
    float Resolution = P0.x;
    float MosaicSize = P0.y;
    vec2 mosaicInSize = InSize / MosaicSize;
    vec2 fractPix = fract(texCoord * mosaicInSize) / mosaicInSize;

    vec4 baseTexel = SampleIn(texCoord - fractPix);

    vec3 fractTexel = baseTexel.rgb - fract(baseTexel.rgb * Resolution) / Resolution;
    float luma = dot(fractTexel, vec3(0.3, 0.59, 0.11));
    vec3 chroma = (fractTexel - luma) * Saturation;
    baseTexel.rgb = luma + chroma;
    baseTexel.a = 1.0;

    fragColor = baseTexel;
}
