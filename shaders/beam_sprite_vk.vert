// File: shaders/beam_sprite_vk.vert (Vulkan volumetric light beam — glare / light-pool quad)
// See src/client/renderer/effects/VolumetricBeam.hpp for the model.
// Vulkan twin of beam_sprite.vert: same maths (generated from one
// body), parameters from the Common UBO, created with CreateShaderFromFilesPortal.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;     // unused (block vertex layout)
layout(location = 2) in vec4 aColor;  // unused
// Field order must match VKBackend::PushConstantBlock (see entity_vk.vert).
// The last three vec4s carry uLocalToRender — the sprite's model matrix into
// render space — as its three ROWS (it is affine), as blockentity_vk.vert.
layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 0-63
    vec2 uScreenSize;   // 64-71
    float uLineWidth;   // 72-75
    float uAlphaTest;   // 76-79
    vec4 uLocalRow0;    // 80-95
    vec4 uLocalRow1;    // 96-111
    vec4 uLocalRow2;    // 112-127
} pc;
#define MVP pc.uMVP
#define LOCAL_TO_RENDER(p) vec3(dot(pc.uLocalRow0, p), dot(pc.uLocalRow1, p), dot(pc.uLocalRow2, p))
layout(location = 0) out vec3 vLocal;
layout(location = 1) out vec3 vRenderPos;

void main() {
    vec4 p = vec4(aPos, 1.0);
    gl_Position = MVP * p;
    vLocal = aPos;
    vRenderPos = LOCAL_TO_RENDER(p);
}
