// File: shaders/entity_outline_vk.frag (Vulkan twin of entity_outline.frag).
#version 450

layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4 uColor;        // rgb = team colour
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    if (texture(uTex, vUV).a == 0.0) discard;
    FragColor = vec4(pc.uColor.rgb, 1.0);
}
