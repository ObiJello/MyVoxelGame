// File: shaders/blockentity_vk.vert (Vulkan block-entity shader)
// Vulkan twin of blockentity.vert — every block-entity renderer and their
// item forms. Created through CreateShaderFromFilesPortal: the fragment
// shader reads the frame's fog and light from the Common UBO.
// Layout matches GetBlockVertexLayout(): pos3 (loc 0), uv2 (loc 1), rgba8 (loc 2).
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

// Field order must match VKBackend::PushConstantBlock (see entity_vk.vert).
// The last three vec4s carry uLocalToRender — the model matrix into render
// space, for the fog — as its three ROWS (it is affine): VKBackend routes
// the name there, so a per-block-entity model matrix never costs a UBO slot.
layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 0-63
    vec2 uScreenSize;   // 64-71
    float uLineWidth;   // 72-75
    float uAlphaTest;   // 76-79
    vec4 uLocalRow0;    // 80-95
    vec4 uLocalRow1;    // 96-111
    vec4 uLocalRow2;    // 112-127
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vRenderPos;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    vec4 p = vec4(aPos, 1.0);
    vRenderPos = vec3(dot(pc.uLocalRow0, p), dot(pc.uLocalRow1, p), dot(pc.uLocalRow2, p));
}
