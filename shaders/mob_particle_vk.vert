// File: shaders/mob_particle_vk.vert
// Vulkan version of MobParticleSystem's particle shader. Camera-facing
// billboards built CPU-side in RENDER space, so all we do is project and
// hand the position on for the fog. Vertex format is the standard 24-byte
// block layout (pos3 + uv2 + RGBA8 normalized). Created through
// CreateShaderFromFilesPortal — the fragment shader reads the frame's fog
// from the Common UBO; uMVP stays a push constant.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;  // particle rCol/gCol/bCol/alpha

layout(push_constant) uniform PC {
    mat4  uMVP;          // 0
    vec2  uScreenSize;   // 64
    float uLineWidth;    // 72
    float uAlphaTest;    // 76
    vec4  uColor;        // 80
    vec4  uUVRange;      // 96 — here: the portal clip plane (uEntityClipPlane)
    vec4  uScalars;      // 112 — x: the draw's light (uEntityLight)
} pc;

// gl_ClipDistance must be advertised explicitly — see block_vk.vert.
out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vRenderPos;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    vRenderPos = aPos;
    // A portal view's clip plane in render space (uEntityClipPlane rides
    // uUVRange, as entity_vk.vert's). Zero = no clipping.
    gl_ClipDistance[0] = (any(notEqual(pc.uUVRange.xyz, vec3(0.0))))
        ? dot(pc.uUVRange.xyz, aPos) + pc.uUVRange.w
        : 1.0;
}
