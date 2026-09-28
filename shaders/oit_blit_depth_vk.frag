#version 450
// File: shaders/oit_blit_depth_vk.frag (Vulkan twin of oit_blit_depth.frag) — MC 26.3 core/blit_depth.fsh
// (BLIT_DEPTH_DURING_DEPTH_BOUNDS): the frame's depth, as the OIT passes
// left it, copied into the clouds' own depth before their depth bounds.
layout(set = 0, binding = 0) uniform sampler2D Sampler0;

void main() {
    gl_FragDepth = texelFetch(Sampler0, ivec2(gl_FragCoord.xy), 0).r;
}
