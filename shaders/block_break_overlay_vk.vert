// File: shaders/block_break_overlay_vk.vert
//
// MC-style "crumbling" crack overlay drawn over the block currently being
// mined. Companion to BlockBreakOverlay.{hpp,cpp}.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

// The SHARED push-constant block — must match VKBackend::PushConstantBlock
// field for field, because Vulkan resolves push constants by BYTE OFFSET, not
// by name. This shader used to declare its own `vec2 uUvMin; vec2 uUvMax;`
// right after uMVP, which put them at offsets 64/72 — where uScreenSize,
// uLineWidth and uAlphaTest actually live — so it sampled the atlas at a
// garbage UV and the crack overlay never appeared under Vulkan (it worked on
// GL, which binds uniforms by name). Every other _vk shader carries this exact
// prefix; keep it that way.
layout(push_constant) uniform PC {
    mat4  uMVP;          // 0
    vec2  uScreenSize;   // 64
    float uLineWidth;    // 72
    float uAlphaTest;    // 76
    vec4  uColor;        // 80
    vec4  uUVRange;      // 96  — (uvMin.xy, uvMax.xy)
    vec4  uScalars;      // 112
} pc;

layout(location = 0) out vec2 vUV;

out gl_PerVertex { vec4 gl_Position; };

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    // aUV spans [0,1]² per face; map it into the atlas sub-rect for the
    // current destroy stage.
    vUV = mix(pc.uUVRange.xy, pc.uUVRange.zw, aUV);
}
