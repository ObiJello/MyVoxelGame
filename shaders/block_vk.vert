// File: shaders/block_vk.vert (Vulkan version)
// Matches OpenGL block.vert — 24-byte compact vertex: pos + uv + RGBA8 color
#version 450

// Input vertex attributes (24-byte compact vertex)
layout (location = 0) in vec3 aPos;       // Vertex position (world-space)
layout (location = 1) in vec2 aTexCoord;  // Texture coordinates from atlas
layout (location = 2) in vec4 aColor;     // Vertex color (RGBA8 normalized)

// Push constants (must match C++ PushConstantBlock layout exactly).
// uPortalClipPlane is aliased onto the same byte range as uColor (offset
// 80) — VKBackend::SetUniformVec4 routes the name "uPortalClipPlane" to
// m_pushConstants.uColor so the same 16-byte slot serves both purposes
// (the chunk shader doesn't need a tint and the player shader doesn't
// need a portal clip plane — no shader uses both at once).
layout (push_constant) uniform PushConstants {
    mat4 uMVP;              // 64 bytes
    vec2 uScreenSize;       // 8 bytes
    float uLineWidth;       // 4 bytes
    float uAlphaTest;       // 4 bytes
    vec4 uPortalClipPlane;  // 16 bytes — xyz = world-space plane normal,
                            //            w = -dot(normal, pointOnPlane)
} pc;

// Output to fragment shader
layout (location = 0) out vec2 fragTexCoord;
layout (location = 1) out vec3 fragWorldPos;
layout (location = 2) out vec4 fragColor;

// Explicit gl_PerVertex redeclaration so the SPIR-V output advertises
// gl_ClipDistance[1] in the per-vertex output block. Without this,
// glslc's default per-vertex block has gl_ClipDistance[] of length 0
// and the gl_ClipDistance[0] = ... write below silently does nothing
// at runtime (no clip plane gets honored → portal-plane clipping is
// off). Mandatory for Vulkan; harmless on the GL shader equivalent.
out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    // Mirror the GL block.vert's portal-plane clipping. Without this,
    // the only clipping done on Vulkan came from the oblique projection
    // matrix, whose post-Z-correction asymptote pushes distant chunks
    // past z_ndc = 1 (Vulkan far) for kept-side geometry — visible
    // symptom: stand 5+ blocks from a portal and the see-through view
    // collapses to the depth-clear sky color, snapping back when you
    // walk closer. gl_ClipDistance does the plane clip precisely without
    // depending on the oblique math.
    gl_ClipDistance[0] = (any(notEqual(pc.uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(pc.uPortalClipPlane.xyz, aPos) + pc.uPortalClipPlane.w
        : 1.0;
    fragTexCoord = aTexCoord;
    fragWorldPos = aPos;
    fragColor = aColor;
}
