// File: shaders/portal_crosshair_vk.vert
// Vulkan version of the inline crosshair vertex shader in PortalCrosshair.cpp.
// Push-constant layout MUST match VKBackend.hpp::PushConstantBlock exactly.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PC {
    mat4  uMVP;          // 0
    vec2  uScreenSize;   // 64
    float uLineWidth;    // 72
    float uAlphaTest;    // 76
    vec4  uColor;        // 80  — tint (rgba)
    vec4  uUVRange;      // 96  — (uvMin.xy, uvMax.xy)
    vec4  uScalars;      // 112
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vUV = mix(pc.uUVRange.xy, pc.uUVRange.zw, aUV);
}
