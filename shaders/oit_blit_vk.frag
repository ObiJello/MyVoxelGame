#version 450
// File: shaders/oit_blit_vk.frag (Vulkan twin of oit_blit.frag) — MC 26.3 BLIT_DEPTH_BOUNDS
// (core/blit_screen.fsh into an RGBA32F target): the depth bounds copied
// texel for texel into the culled depth bounds before the cull.
layout(set = 0, binding = 0) uniform sampler2D Sampler0;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texelFetch(Sampler0, ivec2(gl_FragCoord.xy), 0);
}
