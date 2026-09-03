// File: shaders/entity_vk.vert (Vulkan version of the mob entity shader)
// Layout matches GetBlockVertexLayout(): pos3 (loc 0), uv2 (loc 1), color4 ubyte (loc 2)
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

// The field order must match VKBackend::PushConstantBlock exactly. A shader may
// declare fewer trailing fields than the block has (the extra bytes are simply
// ignored), but it may not reorder or omit a field in the middle.
layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 0-63
    vec2 uScreenSize;   // 64-71
    float uLineWidth;   // 72-75
    float uAlphaTest;   // 76-79
    vec4 uColor;        // 80-95  — the hurt/swell overlay
    vec4 uUVRange;      // 96-111 — here: the portal clip plane (uEntityClipPlane)
} pc;

// gl_ClipDistance must be advertised explicitly — see block_vk.vert.
out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    // Vertices are already in world space — ModelPart::Build applies the part
    // hierarchy and the entity transform on the CPU, so every mob sharing a
    // texture batches into one draw and only uMVP is needed here.
    vUV = aUV;
    vColor = aColor;
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    // Portal clip plane in aPos space (camera-relative world; the renderer
    // folds the camera offset into .w). Zero = no clipping.
    gl_ClipDistance[0] = (any(notEqual(pc.uUVRange.xyz, vec3(0.0))))
        ? dot(pc.uUVRange.xyz, aPos) + pc.uUVRange.w
        : 1.0;
}
