// File: shaders/blockentity_vk.vert (Vulkan block-entity shader)
// Shared by Chest/ShulkerBox/Campfire/Skull renderers. UVs arrive already
// normalized: the GL inline shaders used to divide by the sheet size in the
// VS, but the divide is baked into the mesh at build time now so one shader
// serves every sheet size on both backends.
// Layout matches GetBlockVertexLayout(): pos3 (loc 0), uv2 (loc 1), rgba8 (loc 2).
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

// Field order must match VKBackend::PushConstantBlock (see entity_vk.vert).
layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 0-63
    vec2 uScreenSize;   // 64-71
    float uLineWidth;   // 72-75
    float uAlphaTest;   // 76-79
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
}
