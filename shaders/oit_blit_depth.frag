#version 330 core
// File: shaders/oit_blit_depth.frag (OpenGL) — MC 26.3 core/blit_depth.fsh
// (BLIT_DEPTH_DURING_DEPTH_BOUNDS): the frame's depth, as the OIT passes
// left it, copied into the clouds' own depth before their depth bounds.
uniform sampler2D Sampler0;

void main() {
    gl_FragDepth = texelFetch(Sampler0, ivec2(gl_FragCoord.xy), 0).r;
}
