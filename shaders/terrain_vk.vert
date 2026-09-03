// File: shaders/terrain_vk.vert (Vulkan twin of terrain.vert)
// Chunk-TERRAIN vertex shader: block_vk.vert plus the greedy-meshing sprite
// tile rect at attribute location 3 (32-byte TerrainVertex). ChunkRenderer
// registers GetTerrainVertexLayout() on the three terrain shaders so their
// pipelines bake the 32-byte input — the backend's 24-byte fallback layout
// would leave location 3 unfed, which is invalid in Vulkan. That is also why
// the shared block_vk.vert must NOT grow this attribute: entity renderers
// feed it 24-byte buffers. Keep the rest in step with block_vk.vert.
#version 450

layout (location = 0) in vec3 aPos;       // Vertex position (world-space)
layout (location = 1) in vec2 aTexCoord;  // Atlas UV, or tile-space UV when merged
layout (location = 2) in vec4 aColor;     // Vertex color (RGBA8 normalized)
// Sprite tile rect (R16G16B16A16_UNORM): xy = sprite origin in atlas UV,
// zw = sprite size; zw == 0 marks an unmerged quad (aTexCoord is a direct
// atlas coordinate), nonzero a greedy-merged quad in tile space.
layout (location = 3) in vec4 aTileRect;

// Push constants — must match C++ PushConstantBlock layout exactly (see
// block_vk.vert for the uPortalClipPlane aliasing note).
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
layout (location = 3) out vec4 fragTileRect;

// Explicit gl_PerVertex redeclaration so gl_ClipDistance[0] actually lands —
// see the long note in block_vk.vert.
out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    // Portal-plane clipping — same contract and rationale as block_vk.vert.
    gl_ClipDistance[0] = (any(notEqual(pc.uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(pc.uPortalClipPlane.xyz, aPos) + pc.uPortalClipPlane.w
        : 1.0;
    fragTexCoord = aTexCoord;
    fragWorldPos = aPos;
    fragColor = aColor;
    fragTileRect = aTileRect;
}
