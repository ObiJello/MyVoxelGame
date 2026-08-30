// File: shaders/block_instanced.vert
#version 330 core

// The instanced twin of block.vert, for BlockCubeEntityRenderer.
//
// Same geometry, same fragment shader (block.frag, unchanged), one difference:
// the model transform arrives as ONE per-instance vec4 attribute — xyz the
// world translation, w a uniform scale — instead of being folded into a uMVP
// uniform on the CPU. That is the whole point — a hundred thousand primed TNT
// were a hundred thousand glDrawElements calls at Apple's ~1 us/sub-draw
// floor, and this collapses each block state into one glDrawElementsInstanced.
//
// Translate + uniform scale is the ENTIRE transform space of the two block
// entities: MC's FallingBlockRenderer is a pure translate, and TntRenderer's
// lift/swell/centre chain collapses to a scale about the cube's centre plus a
// translate (BlockCubeEntityRenderer derives it). A full mat4 per instance
// carried 64 bytes for 16 bytes of information, four times the upload.
//
// The overlay colour is deliberately NOT per-instance. TNT's white flash has
// exactly two states, so the renderer groups by it and keeps block.frag's
// uOverlayColor uniform — which is what lets this share that shader instead of
// forking it.
//
// NOTE the world-position difference from block.vert. There, aPos is already
// world-space (the chunk mesher emits it that way) and fragWorldPos is aPos
// verbatim. Here aPos is MODEL-space, so both fragWorldPos and the portal clip
// distance must use the transformed position — using aPos would fog and clip
// every instance as if it stood at the origin.

layout (location = 0) in vec3 aPos;       // Model-space position
layout (location = 1) in vec2 aTexCoord;  // Texture coordinates from atlas
layout (location = 2) in vec4 aColor;     // Vertex color (RGBA8 normalized by GL)

// Per-instance: xyz = world translation, w = uniform scale.
layout (location = 3) in vec4 aInstance;

uniform mat4 uViewProj;         // View-projection only; model is per-instance
uniform vec4 uPortalClipPlane;  // See block.vert for the contract

out vec2 fragTexCoord;
out vec3 fragWorldPos;
out vec4 fragColor;

void main() {
    vec4 worldPos = vec4(aInstance.xyz + aInstance.w * aPos, 1.0);

    gl_Position = uViewProj * worldPos;
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, worldPos.xyz) + uPortalClipPlane.w
        : 1.0;
    fragTexCoord = aTexCoord;
    fragWorldPos = worldPos.xyz;
    fragColor = aColor;
}
