// File: shaders/mob_particle_vk.frag
// Vulkan version of MobParticleSystem's fragment shader. Mirrors the
// OpenGL inline source: texture × per-particle colour, MC's 0.1 alpha
// cutout on the blended pass (blend state comes from the pipeline).
#version 450

layout(set = 0, binding = 0) uniform sampler2D uSprite;

layout(push_constant) uniform PC {
    mat4  uMVP;
    vec2  uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4  uColor;
    vec4  uUVRange;
    vec4  uScalars;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 FragColor;

void main() {
    vec4 s = texture(uSprite, vUV);
    vec4 c = s * vColor;
    if (c.a < 0.1) discard;
    FragColor = c;
}
