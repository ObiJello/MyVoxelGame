// File: shaders/crosshair_vk.vert (Vulkan version of crosshair GL shader)
// Layout matches GetBlockVertexLayout(): pos3 (loc 0), uv2 (loc 1), color4 ubyte (loc 2)
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

layout(location = 0) out vec2 TexCoord;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    TexCoord = aTexCoord;
}
