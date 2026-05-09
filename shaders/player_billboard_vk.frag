// File: shaders/player_billboard_vk.frag (Vulkan version of PlayerRenderer GL shader)
// Outputs the interpolated vertex color (set 0 binding 0 sampler is bound by
// PlayerRenderer to a 1x1 white dummy texture, but unused here — the GL shader
// also ignores the texture and just uses vColor.)
#version 450

layout(location = 0) in vec4 vColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    FragColor = vColor;
}
