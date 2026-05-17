// File: shaders/portal_tonemap.vert
// Fullscreen-quad vertex shader for the HDR tone-map composite pass.
// Generates 3 vertices that cover the screen with NDC coords and UVs in
// [0, 1] — no vertex buffer needed (gl_VertexID drives positions).
#version 330 core

out vec2 vUV;

void main() {
    // Bottom-left, bottom-right, top-left → triangle covers viewport.
    // We expand to a full-screen triangle via gl_VertexID lookup.
    vec2 pos = vec2((gl_VertexID == 1) ?  3.0 : -1.0,
                    (gl_VertexID == 2) ?  3.0 : -1.0);
    vUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
