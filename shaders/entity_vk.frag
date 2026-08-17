// File: shaders/entity_vk.frag (Vulkan version of the mob entity shader)
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4 uColor;        // hurt flash / creeper swell overlay: rgb + strength
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    vec4 t = texture(uTex, vUV);

    // Entity textures are cutout: the transparent regions of a 64x32 sheet must
    // be discarded, or every mob renders inside a black box.
    if (t.a < 0.05) discard;

    vec3 base = t.rgb * vColor.rgb;
    FragColor = vec4(mix(base, pc.uColor.rgb, pc.uColor.a), 1.0);
}
