// File: shaders/player_billboard_vk.frag (Vulkan version of PlayerRenderer GL shader)
// Outputs the interpolated vertex color (set 0 binding 0 sampler is bound by
// PlayerRenderer to a 1x1 white dummy texture, but unused here — the GL shader
// also ignores the texture and just uses vColor.)
#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vWorldPos;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

// Push constants layout matches the vertex shader.
// uClipPlane (offset 80) is the world-space half-space test used by the
// portal ghost render to hide the half of the body that hasn't emerged
// from the destination portal yet. vec4(0) = no clipping.
layout(push_constant) uniform PushConstants {
    mat4 uMVP;          //  0
    vec2 uScreenSize;   // 64
    float uLineWidth;   // 72
    float uAlphaTest;   // 76
    vec4 uClipPlane;    // 80
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    // Mirrors the OpenGL player_billboard fragment shader's clip-plane
    // test: discard fragments on the negative side of the plane.
    // any(notEqual(pc.uClipPlane.xyz, vec3(0.0))) short-circuits the
    // test when the caller passes vec4(0) for "no clipping".
    if (any(notEqual(pc.uClipPlane.xyz, vec3(0.0))) &&
        dot(vWorldPos, pc.uClipPlane.xyz) + pc.uClipPlane.w < 0.0) {
        discard;
    }
    FragColor = vColor;
}
