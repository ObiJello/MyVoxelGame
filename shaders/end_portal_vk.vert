// File: shaders/end_portal_vk.vert
//
// Vulkan twin of shaders/end_portal.vert — same math, backend bindings only.
// Uses the UBO-aware ("portal") pipeline layout, because the fragment stage
// needs TWO samplers (set=0 and set=2) plus the Common UBO (set=1); the plain
// block layout only offers one texture and no UBO. EndPortalRenderer therefore
// creates this pair through VKBackend::CreateShaderFromFilesPortal.
#version 450

// Shared 24-byte block layout — the pipeline declares aUV/aColor at locations
// 1 and 2 as well, which this shader simply does not consume. See the GL twin
// for why the portal quads ride the standard layout at all.
layout(location = 0) in vec3 aPos;

// The SHARED push-constant block — must match VKBackend::PushConstantBlock
// field for field: Vulkan resolves push constants by BYTE OFFSET, not by name,
// so declaring a shorter prefix and then reading a later field would silently
// read the wrong bytes (the trap documented in block_break_overlay_vk.vert).
layout(push_constant) uniform PC {
    mat4  uMVP;          // 0
    vec2  uScreenSize;   // 64
    float uLineWidth;    // 72
    float uAlphaTest;    // 76
    vec4  uColor;        // 80
} pc;

layout(location = 0) out vec4 vTexProj;
layout(location = 1) out vec3 vWorldPos;

out gl_PerVertex { vec4 gl_Position; };

// assets/shaders/include/projection.glsl, verbatim.
//
// Note this stays correct under Vulkan even though VKBackend premultiplies
// every uMVP by kVkZCorrect: that matrix only remaps z, and textureProj
// divides by .w and uses only .xy. The negative-height viewport VKBackend
// sets in BeginFrame keeps the y direction matching OpenGL too, so the
// starfield is not mirrored between backends.
vec4 projection_from_position(vec4 position) {
    vec4 projection = position * 0.5;
    projection.xy = vec2(projection.x + projection.w, projection.y + projection.w);
    projection.zw = position.zw;
    return projection;
}

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vTexProj = projection_from_position(gl_Position);
    vWorldPos = aPos;
}
