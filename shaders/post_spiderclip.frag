#version 330 core
// File: shaders/post_spiderclip.frag (OpenGL) — MC post/spiderclip.fsh
// Post pass of Render::PostChain. texCoord is GL-convention (0,0 bottom-left).
// uPostParams packs the pass's config uniforms (MC's std140 blocks, each
// block starting on a vec4) as four vec4 columns; SamplerInfo is uInSize /
// uOutSize. An input the chain does not ask to sample bilinear is sampled at
// its texel centre (the targets are all linear-filtered — nearest by snap).
in vec2 texCoord;
in vec2 scaledCoord;
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

// RotScaleConfig { vec2 InScale; vec2 InOffset; float InRotation; } -> P0, P1.x
// SpiderConfig { vec4 Scissor; vec4 Vignette; }                     -> P2, P3
void main() {
    vec4 Scissor = P2;
    vec4 Vignette = P3;
    vec4 ScaledTexel = SampleIn(scaledCoord);
    vec4 BlurTexel = SampleAux(texCoord);
    vec4 OutTexel = ScaledTexel;

    // -- Alpha Clipping --
    if (scaledCoord.x < Scissor.x) OutTexel = BlurTexel;
    if (scaledCoord.y < Scissor.y) OutTexel = BlurTexel;
    if (scaledCoord.x > Scissor.z) OutTexel = BlurTexel;
    if (scaledCoord.y > Scissor.w) OutTexel = BlurTexel;

    if (scaledCoord.x < Vignette.x) OutTexel = mix(BlurTexel, OutTexel, (Scissor.x - scaledCoord.x) / (Scissor.x - Vignette.x));
    if (scaledCoord.y < Vignette.y) OutTexel = mix(BlurTexel, OutTexel, (Scissor.y - scaledCoord.y) / (Scissor.y - Vignette.y));
    if (scaledCoord.x > Vignette.z) OutTexel = mix(BlurTexel, OutTexel, (Scissor.z - scaledCoord.x) / (Scissor.z - Vignette.z));
    if (scaledCoord.y > Vignette.w) OutTexel = mix(BlurTexel, OutTexel, (Scissor.w - scaledCoord.y) / (Scissor.w - Vignette.w));
    fragColor = vec4(OutTexel.rgb, 1.0);
}
