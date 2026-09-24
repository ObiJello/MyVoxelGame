// File: shaders/entity_outline_vk.vert (Vulkan twin of entity_outline.vert).
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;          //  0
    vec2 uScreenSize;   // 64
    float uLineWidth;   // 72
    float uAlphaTest;   // 76
    vec4 uColor;        // 80 — the team colour (fragment stage)
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vUV = aUV;
}
