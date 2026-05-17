// File: shaders/portal_particle_vk.vert
// Vulkan version of PortalParticleSystem's particle shader. Camera-facing
// billboards built CPU-side, so all we do is project. Vertex format is
// the standard 24-byte block layout (pos3 + uv2 + RGBA8 normalized).
// Push constants match VKBackend's PushConstantBlock — uses only uMVP
// from the head, and uScalars.x (aliased as uHasSprite) for the frag.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;  // rgb = hot color, a = lifetime fraction

layout(push_constant) uniform PC {
    mat4  uMVP;          // 0
    vec2  uScreenSize;   // 64
    float uLineWidth;    // 72
    float uAlphaTest;    // 76
    vec4  uColor;        // 80
    vec4  uUVRange;      // 96
    vec4  uScalars;      // 112 — x = uHasSprite (int as float)
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
}
