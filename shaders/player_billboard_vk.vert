// File: shaders/player_billboard_vk.vert (Vulkan version of PlayerRenderer GL shader)
// Layout matches GetBlockVertexLayout(): pos3 (loc 0), uv2 (loc 1), color4 ubyte (loc 2)
// PlayerRenderer draws unlit colored stick figures — texture is a 1x1 white dummy
// to satisfy the shader binding; vertex color is the actual visible color.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
} pc;

layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vColor = aColor;
}
