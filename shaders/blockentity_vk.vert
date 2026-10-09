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

// The Common UBO's head (SpirvUniforms.hpp CommonUBO; a shader may declare
// a prefix of the block): uTint_ carries the portal clip plane — a block
// entity seen through a portal is clipped at the far portal's plane like
// the terrain (block_vk.vert), since the Vulkan/Metal see-through pass
// draws with the plain projection (PortalRenderer::SceneRenderFn). The
// push constants cannot hold it: their last three rows are uLocalToRender.
// BlockEntityShader::ApplyWorld sets uPortalClipPlane before the model
// matrix, so the alias (SpirvUniforms::SetVec4) lands here and stays.
layout(std140, set = 1, binding = 0) uniform Common {
    mat4 uMVP_;
    mat4 uModel_;
    vec4 uPortalColor_;
    vec4 uColorDark_;
    vec4 uColorHot_;
    vec4 uKeyDir_;
    vec4 uTint_;        // the portal clip plane (render space); zero = none
} U;

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
    vec4 p = vec4(aPos, 1.0);
    vRenderPos = vec3(dot(pc.uLocalRow0, p), dot(pc.uLocalRow1, p), dot(pc.uLocalRow2, p));
    // Portal clip plane in render space, the space vRenderPos is in.
    gl_ClipDistance[0] = (any(notEqual(U.uTint_.xyz, vec3(0.0))))
        ? dot(U.uTint_.xyz, vRenderPos) + U.uTint_.w
        : 1.0;
}
