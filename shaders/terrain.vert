// File: shaders/terrain.vert
// Chunk-TERRAIN vertex shader: block.vert plus the greedy-meshing sprite
// tile rect at attribute location 3 (32-byte TerrainVertex — see
// src/client/renderer/core/Vertex.hpp and GetTerrainVertexLayout()).
//
// Terrain-ONLY. Do not point entity/item/portal renderers at this shader:
// they feed 24-byte buffers without attribute 3, which is undefined on
// Vulkan (their block.vert stays as-is for exactly that reason). Keep the
// rest of this file in step with block.vert.
#version 330 core

layout (location = 0) in vec3 aPos;       // Vertex position (world-space)
layout (location = 1) in vec2 aTexCoord;  // Atlas UV, or tile-space UV when merged
layout (location = 2) in vec4 aColor;     // Vertex color (RGBA8 normalized by GL)
// Sprite tile rect (4x unorm16): xy = sprite origin in atlas UV, zw = sprite
// size. zw == 0 marks an UNMERGED quad: aTexCoord is then a direct atlas
// coordinate. Nonzero marks a greedy-merged quad whose aTexCoord is in TILE
// space (0..N block repeats); the fragment shader folds it back per block.
layout (location = 3) in vec4 aTileRect;

// Uniforms
uniform mat4 uMVP;  // Model-View-Projection matrix
// World-space clip plane for portal see-through rendering — same contract
// as block.vert (see the comment there).
uniform vec4 uPortalClipPlane;

// Output to fragment shader
out vec2 fragTexCoord;
out vec3 fragWorldPos;
out vec4 fragColor;
out vec4 fragTileRect;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, aPos) + uPortalClipPlane.w
        : 1.0;
    fragTexCoord = aTexCoord;
    fragWorldPos = aPos;
    fragColor = aColor;
    fragTileRect = aTileRect;
}
