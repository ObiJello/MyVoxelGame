#version 450
// File: shaders/post_rotscale_vk.vert (Vulkan twin of post_rotscale.vert —
// MC post/rotscale.vsh). Both coordinates stay GL-convention (the fragment
// shader flips v when it samples), so the rotation turns the same way.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec2 texCoord;
layout(location = 1) out vec2 scaledCoord;

layout(push_constant) uniform PushConstants {
    mat4 uPostParams;   // 0 — see post_blit_vk.frag for the whole block
} pc;
#define P0 pc.uPostParams[0]
#define P1 pc.uPostParams[1]

void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    texCoord = aUV;

    // RotScaleConfig { vec2 InScale; vec2 InOffset; float InRotation; }
    vec2 InScale = P0.xy;
    vec2 InOffset = P0.zw;
    float InRotation = P1.x;
    float Deg2Rad = 0.0174532925;
    float InRadians = InRotation * Deg2Rad;
    float Cosine = cos(InRadians);
    float Sine = sin(InRadians);
    float RotU = texCoord.x * Cosine - texCoord.y * Sine;
    float RotV = texCoord.y * Cosine + texCoord.x * Sine;
    scaledCoord = vec2(RotU, RotV) * InScale + InOffset;
}
