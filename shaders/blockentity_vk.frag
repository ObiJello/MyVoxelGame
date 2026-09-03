// File: shaders/blockentity_vk.frag (Vulkan block-entity shader)
// The alpha-discard threshold differs per renderer (chest/shulker 0.05,
// campfire food 0.5, skull 0.1), so it rides the uAlphaTest push-constant
// slot instead of being hardcoded like the GL inline twins.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    vec4 t = texture(uTex, vUV);
    if (t.a < pc.uAlphaTest) discard;
    FragColor = t * vColor;
}
