// File: shaders/block_break_overlay_vk.vert
//
// MC-style "crumbling" crack overlay drawn over the block currently being
// mined. Companion to BlockBreakOverlay.{hpp,cpp}.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uUvMin;
    vec2 uUvMax;
} pc;

layout(location = 0) out vec2 vUV;

out gl_PerVertex { vec4 gl_Position; };

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vUV = mix(pc.uUvMin, pc.uUvMax, aUV);
}
