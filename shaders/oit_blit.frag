#version 330 core
// File: shaders/oit_blit.frag (OpenGL) — MC 26.3 BLIT_DEPTH_BOUNDS
// (core/blit_screen.fsh into an RGBA32F target): the depth bounds copied
// texel for texel into the culled depth bounds before the cull.
uniform sampler2D Sampler0;
out vec4 fragColor;

void main() {
    fragColor = texelFetch(Sampler0, ivec2(gl_FragCoord.xy), 0);
}
