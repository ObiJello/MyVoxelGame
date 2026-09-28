#version 450
// File: shaders/post_blit_vk.frag (Vulkan twin of post_blit.frag — MC post/blit.fsh).
// texCoord stays GL-convention; the frame's flipped viewport puts texture
// row 0 at the top, so every sample flips v on the way in.
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D uInSampler;
layout(set = 2, binding = 0) uniform sampler2D uAuxSampler;

layout(push_constant) uniform PushConstants {
    mat4  uPostParams;    //   0 — the uMVP slot (VKBackend routes uPostParams raw)
    vec2  uInSize;        //  64 — uScreenSize
    float uInBilinear;    //  72 — uLineWidth
    float uAuxBilinear;   //  76 — uAlphaTest
    vec2  uAuxSize;       //  80 — uColor.xy
    vec2  _pad0;          //  88
    vec2  _pad1;          //  96 — uUVRange.xy
    vec2  uOutSize;       // 104 — uUVRange.zw
    vec4  uScalars;       // 112 — x = uMenuBlurRadius
} pc;

vec4 SampleIn(vec2 uv) {
    if (pc.uInBilinear < 0.5) uv = (floor(uv * pc.uInSize) + 0.5) / pc.uInSize;
    return texture(uInSampler, vec2(uv.x, 1.0 - uv.y));
}
vec4 SampleAux(vec2 uv) {
    if (pc.uAuxBilinear < 0.5) uv = (floor(uv * pc.uAuxSize) + 0.5) / pc.uAuxSize;
    return texture(uAuxSampler, vec2(uv.x, 1.0 - uv.y));
}
#define P0 pc.uPostParams[0]
#define P1 pc.uPostParams[1]
#define P2 pc.uPostParams[2]
#define P3 pc.uPostParams[3]
#define InSize pc.uInSize
#define OutSize pc.uOutSize
#define MenuBlurRadius pc.uScalars.x

// BlitConfig { vec4 ColorModulate; }
void main() {
    fragColor = SampleIn(texCoord) * P0;
}
