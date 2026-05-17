// File: shaders/player_billboard_vk.vert (Vulkan version of PlayerRenderer GL shader)
// Layout matches GetBlockVertexLayout(): pos3 (loc 0), uv2 (loc 1), color4 ubyte (loc 2)
// PlayerRenderer draws unlit colored stick figures — texture is a 1x1 white dummy
// to satisfy the shader binding; vertex color is the actual visible color.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

// Must match C++ PushConstantBlock layout exactly. Slot at offset 80
// (uColor in the master struct) is reused here as uClipPlane — see
// VKBackend::SetUniformVec4 where the name "uClipPlane" is routed to
// m_pushConstants.uColor so the same byte range serves both purposes
// without colliding (no shader needs both a tint AND a clip plane).
layout(push_constant) uniform PushConstants {
    mat4 uMVP;          //  0  (64 bytes)
    vec2 uScreenSize;   // 64  (8 bytes)
    float uLineWidth;   // 72  (4 bytes)
    float uAlphaTest;   // 76  (4 bytes)
    vec4 uClipPlane;    // 80  (16 bytes) — xyz = normal, w = -dot(n, pointOnPlane)
} pc;

layout(location = 0) out vec4 vColor;
// World-space position passed to the fragment shader so it can test
// against uClipPlane (used by the portal ghost render to clip the
// player body to the "emerged" half on the destination side).
// PlayerRenderer pre-multiplies the per-call model matrix into the
// vertex positions on the CPU, so aPos IS world-space by the time it
// hits the shader — no separate uModel uniform needed.
layout(location = 1) out vec3 vWorldPos;

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vColor      = aColor;
    vWorldPos   = aPos;
}
