// File: shaders/portal_crosshair_vk.frag
// Vulkan version of the inline crosshair fragment shader in PortalCrosshair.cpp.
// Push-constant layout MUST match VKBackend.hpp::PushConstantBlock exactly.
#version 450

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

layout(push_constant) uniform PC {
    mat4  uMVP;
    vec2  uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4  uColor;       // tint (rgba)
    vec4  uUVRange;
    vec4  uScalars;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

void main() {
    vec4 t = texture(uAtlas, vUV);
    FragColor = vec4(pc.uColor.rgb * t.rgb, pc.uColor.a * t.a);
}
