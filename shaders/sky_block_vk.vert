// File: shaders/sky_block_vk.vert (Vulkan version of sky_block.vert)
//
// The sky block's depth-only window into the sky — see sky_block.vert.
#version 450

// The shared 24-byte block vertex (GetBlockVertexLayout); only the position
// is read. RENDER space (camera-relative, see RenderOrigin.hpp).
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec4 aColor;

// Push constants (must match the C++ PushConstantBlock layout).
// uPortalClipPlane rides the uColor slot (offset 80) — VKBackend::
// SetUniformVec4 routes the name there, as for block_vk.vert.
layout (push_constant) uniform PushConstants {
    mat4  uMVP;              // 0
    vec2  uScreenSize;       // 64
    float uLineWidth;        // 72
    float uAlphaTest;        // 76
    vec4  uPortalClipPlane;  // 80 — render-space plane, vec4(0) = none
} pc;

// gl_ClipDistance[1] must be advertised explicitly or the write below is
// dropped (see block_vk.vert).
out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    gl_ClipDistance[0] = (any(notEqual(pc.uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(pc.uPortalClipPlane.xyz, aPos) + pc.uPortalClipPlane.w
        : 1.0;
}
